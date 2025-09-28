#include "peer_connection.h"
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <poll.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

using namespace std::chrono_literals;

namespace p2p {

// Socket helper functions
namespace {

void set_nonblocking(int sock_fd, bool nonblocking = true) {
    int flags = fcntl(sock_fd, F_GETFL, 0);
    if (flags == -1) {
        throw std::system_error(errno, std::system_category(), "fcntl F_GETFL failed");
    }
    
    if (nonblocking) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    
    if (fcntl(sock_fd, F_SETFL, flags) == -1) {
        throw std::system_error(errno, std::system_category(), "fcntl F_SETFL failed");
    }
}

void connect_with_timeout(int sock_fd, const sockaddr* addr, socklen_t addr_len, int timeout_ms) {
    // Set non-blocking
    set_nonblocking(sock_fd, true);
    
    // Start connection
    int result = connect(sock_fd, addr, addr_len);
    if (result == 0) {
        // Connected immediately
        set_nonblocking(sock_fd, false);
        return;
    }
    
    if (errno != EINPROGRESS) {
        throw std::system_error(errno, std::system_category(), "connect failed");
    }
    
    // Wait for connection to complete with timeout
    pollfd pfd;
    pfd.fd = sock_fd;
    pfd.events = POLLOUT;
    
    int poll_result = poll(&pfd, 1, timeout_ms);
    if (poll_result == 0) {
        close(sock_fd);
        throw std::runtime_error("Connection timed out");
    }
    
    if (poll_result < 0) {
        close(sock_fd);
        throw std::system_error(errno, std::system_category(), "poll failed");
    }
    
    // Check for errors
    int error = 0;
    socklen_t error_len = sizeof(error);
    if (getsockopt(sock_fd, SOL_SOCKET, SO_ERROR, &error, &error_len) < 0) {
        close(sock_fd);
        throw std::system_error(errno, std::system_category(), "getsockopt failed");
    }
    
    if (error != 0) {
        close(sock_fd);
        throw std::system_error(error, std::system_category(), "connection failed");
    }
    
    // Set back to blocking mode
    set_nonblocking(sock_fd, false);
}

} // anonymous namespace

// PeerConnection implementation
PeerConnection::PeerConnection(const std::string& peer_id, const std::string& peer_ip, uint16_t peer_port,
                             const std::string& info_hash, const std::string& our_peer_id)
    : peer_id_(peer_id), 
      peer_ip_(peer_ip), 
      peer_port_(peer_port),
      info_hash_(info_hash),
      our_peer_id_(our_peer_id) {
    
    // Create socket
    sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd_ < 0) {
        throw std::system_error(errno, std::system_category(), "socket creation failed");
    }
    
    // Set socket options
    int flag = 1;
    if (setsockopt(sock_fd_, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) {
        close(sock_fd_);
        throw std::system_error(errno, std::system_category(), "setsockopt SO_REUSEADDR failed");
    }
    
    // Set TCP_NODELAY to disable Nagle's algorithm
    if (setsockopt(sock_fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
        close(sock_fd_);
        throw std::system_error(errno, std::system_category(), "setsockopt TCP_NODELAY failed");
    }
    
    // Set keepalive
    if (setsockopt(sock_fd_, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag)) < 0) {
        close(sock_fd_);
        throw std::system_error(errno, std::system_category(), "setsockopt SO_KEEPALIVE failed");
    }
}

PeerConnection::PeerConnection(int sock_fd, const sockaddr_in& addr, 
                             const std::string& info_hash, const std::string& our_peer_id)
    : sock_fd_(sock_fd),
      info_hash_(info_hash),
      our_peer_id_(our_peer_id) {
    
    // Get peer IP and port
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(addr.sin_addr), ip_str, INET_ADDRSTRLEN);
    
    peer_ip_ = ip_str;
    peer_port_ = ntohs(addr.sin_port);
    
    // Set socket options
    int flag = 1;
    if (setsockopt(sock_fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
        close(sock_fd_);
        throw std::system_error(errno, std::system_category(), "setsockopt TCP_NODELAY failed");
    }
}

PeerConnection::~PeerConnection() {
    stop();
}

void PeerConnection::start() {
    if (running_) {
        return;
    }
    
    // If this is an outgoing connection, connect to the peer
    if (state_ == PeerState::DISCONNECTED && !peer_ip_.empty() && peer_port_ > 0) {
        set_state(PeerState::CONNECTING);
        
        // Resolve hostname if necessary
        hostent* host = gethostbyname(peer_ip_.c_str());
        if (!host) {
            handle_error("Could not resolve host: " + peer_ip_);
            return;
        }
        
        // Set up the address structure
        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(peer_port_);
        std::memcpy(&addr.sin_addr, host->h_addr, host->h_length);
        
        try {
            // Connect with timeout
            connect_with_timeout(sock_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr), 5000);
            set_state(PeerState::HANDSHAKING);
            
            // Send handshake
            if (!send_handshake()) {
                handle_error("Failed to send handshake");
                return;
            }
        } catch (const std::exception& e) {
            handle_error(std::string("Connection failed: ") + e.what());
            return;
        }
    } else if (sock_fd_ >= 0) {
        // This is an incoming connection, already connected
        set_state(PeerState::HANDSHAKING);
    } else {
        handle_error("No valid connection");
        return;
    }
    
    // Start I/O thread
    running_ = true;
    io_thread_ = std::thread(&PeerConnection::io_loop, this);
}

void PeerConnection::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    // Close the socket to wake up any blocking calls
    if (sock_fd_ >= 0) {
        shutdown(sock_fd_, SHUT_RDWR);
        close(sock_fd_);
        sock_fd_ = -1;
    }
    
    // Wait for the I/O thread to finish
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
    
    set_state(PeerState::DISCONNECTED);
}

void PeerConnection::request_piece(uint32_t piece_index, uint32_t begin, uint32_t length) {
    if (state_ != PeerState::CONNECTED) {
        throw std::runtime_error("Not connected to peer");
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    if (peer_choking_) {
        throw std::runtime_error("Peer is choking us");
    }
    
    // Create and send request message
    RequestMessage msg(piece_index, begin, length);
    auto data = msg.serialize();
    
    if (!send_message(data)) {
        handle_error("Failed to send request message");
        return;
    }
    
    // Track the request
    uint64_t request_id = generate_request_id();
    PendingRequest req{
        .piece_index = piece_index,
        .begin = begin,
        .length = length,
        .timestamp = std::chrono::steady_clock::now()
    };
    
    std::lock_guard<std::mutex> req_lock(request_mutex_);
    pending_requests_[request_id] = req;
}

void PeerConnection::cancel_request(uint32_t piece_index, uint32_t begin, uint32_t length) {
    std::lock_guard<std::mutex> lock(request_mutex_);
    
    // Find and remove the request
    for (auto it = pending_requests_.begin(); it != pending_requests_.end(); ) {
        const auto& req = it->second;
        if (req.piece_index == piece_index && 
            req.begin == begin && 
            req.length == length) {
            it = pending_requests_.erase(it);
            
            // Send cancel message
            CancelMessage msg(piece_index, begin, length);
            auto data = msg.serialize();
            send_message(data);
            
            break;
        } else {
            ++it;
        }
    }
}

void PeerConnection::set_state(PeerState new_state) {
    if (state_ == new_state) {
        return;
    }
    
    state_ = new_state;
    
    if (state_callback_) {
        state_callback_(peer_id_, new_state);
    }
}

void PeerConnection::handle_error(const std::string& error) {
    if (error_callback_) {
        error_callback_(peer_id_ + ": " + error);
    }
    
    stop();
}

void PeerConnection::io_loop() {
    const size_t BUFFER_SIZE = 16 * 1024; // 16KB buffer
    std::vector<uint8_t> buffer(BUFFER_SIZE);
    
    // For incoming connections, send handshake first
    if (state_ == PeerState::HANDSHAKING && !peer_ip_.empty()) {
        if (!send_handshake()) {
            handle_error("Failed to send handshake");
            return;
        }
    }
    
    while (running_) {
        // Set up poll
        pollfd pfd;
        pfd.fd = sock_fd_;
        pfd.events = POLLIN | POLLERR | POLLHUP;
        
        // Poll with timeout (100ms)
        int result = poll(&pfd, 1, 100);
        
        if (result < 0) {
            if (errno == EINTR) {
                continue; // Interrupted by signal, try again
            }
            
            handle_error("Poll error: " + std::string(strerror(errno)));
            break;
        }
        
        if (result == 0) {
            // Timeout, check for timeouts on pending requests
            check_timeouts();
            continue;
        }
        
        // Check for errors
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            handle_error("Socket error");
            break;
        }
        
        // Read data if available
        if (pfd.revents & POLLIN) {
            ssize_t bytes_read = recv(sock_fd_, buffer.data(), buffer.size(), 0);
            
            if (bytes_read <= 0) {
                if (bytes_read == 0 || (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    handle_error("Connection closed by peer");
                    break;
                }
                continue;
            }
            
            // Process the received data
            recv_buffer_.insert(recv_buffer_.end(), buffer.begin(), buffer.begin() + bytes_read);
            
            if (!process_incoming_data(recv_buffer_)) {
                handle_error("Error processing incoming data");
                break;
            }
        }
    }
    
    // Clean up
    if (sock_fd_ >= 0) {
        close(sock_fd_);
        sock_fd_ = -1;
    }
    
    set_state(PeerState::DISCONNECTED);
}

bool PeerConnection::send_handshake() {
    HandshakeMessage handshake;
    handshake.info_hash = info_hash_;
    handshake.peer_id = our_peer_id_;
    
    auto data = handshake.serialize();
    return send_message(data);
}

bool PeerConnection::send_message(const std::vector<uint8_t>& data) {
    if (sock_fd_ < 0) {
        return false;
    }
    
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        ssize_t sent = send(sock_fd_, data.data() + total_sent, data.size() - total_sent, 0);
        if (sent <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Would block, wait for socket to be writable
                pollfd pfd;
                pfd.fd = sock_fd_;
                pfd.events = POLLOUT;
                
                if (poll(&pfd, 1, 5000) <= 0) {
                    return false; // Timeout or error
                }
                
                continue;
            }
            
            return false; // Error
        }
        
        total_sent += sent;
    }
    
    return true;
}

bool PeerConnection::process_incoming_data(std::vector<uint8_t>& data) {
    while (!data.empty()) {
        // If we're not in a message, check if we have enough data for the length prefix
        if (!in_message_) {
            if (data.size() < 4) {
                break; // Not enough data for length prefix
            }
            
            // Get message length (big-endian)
            message_length_ = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
            
            // Handle keep-alive message (length = 0)
            if (message_length_ == 0) {
                data.erase(data.begin(), data.begin() + 4);
                handle_keep_alive();
                continue;
            }
            
            // Check if we have the complete message
            if (data.size() < message_length_ + 4) {
                in_message_ = true;
                break;
            }
            
            // Extract the message
            std::vector<uint8_t> message(data.begin() + 4, data.begin() + 4 + message_length_);
            data.erase(data.begin(), data.begin() + 4 + message_length_);
            
            handle_message(message);
        } else {
            // We're in the middle of a message, check if we have enough data
            if (data.size() < message_length_ + 4) {
                break;
            }
            
            // Extract the message
            std::vector<uint8_t> message(data.begin() + 4, data.begin() + 4 + message_length_);
            data.erase(data.begin(), data.begin() + 4 + message_length_);
            
            in_message_ = false;
            handle_message(message);
        }
    }
    
    return true;
}

void PeerConnection::handle_message(std::vector<uint8_t>& message) {
    if (message.empty()) {
        return;
    }
    
    uint8_t message_id = message[0];
    std::vector<uint8_t> payload(message.begin() + 1, message.end());
    
    try {
        switch (static_cast<MessageType>(message_id)) {
            case MessageType::CHOKE:
                handle_choke();
                break;
                
            case MessageType::UNCHOKE:
                handle_unchoke();
                break;
                
            case MessageType::INTERESTED:
                handle_interested();
                break;
                
            case MessageType::NOT_INTERESTED:
                handle_not_interested();
                break;
                
            case MessageType::HAVE: {
                if (payload.size() != 4) {
                    throw std::runtime_error("Invalid HAVE message length");
                }
                uint32_t piece_index = (payload[0] << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3];
                handle_have(piece_index);
                break;
            }
                
            case MessageType::BITFIELD:
                handle_bitfield(payload, message_length_ * 8);
                break;
                
            case MessageType::REQUEST: {
                if (payload.size() != 12) {
                    throw std::runtime_error("Invalid REQUEST message length");
                }
                uint32_t index = (payload[0] << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3];
                uint32_t begin = (payload[4] << 24) | (payload[5] << 16) | (payload[6] << 8) | payload[7];
                uint32_t length = (payload[8] << 24) | (payload[9] << 16) | (payload[10] << 8) | payload[11];
                handle_request(index, begin, length);
                break;
            }
                
            case MessageType::PIECE: {
                if (payload.size() < 8) {
                    throw std::runtime_error("Invalid PIECE message length");
                }
                uint32_t index = (payload[0] << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3];
                uint32_t begin = (payload[4] << 24) | (payload[5] << 16) | (payload[6] << 8) | payload[7];
                std::vector<uint8_t> block(payload.begin() + 8, payload.end());
                handle_piece(index, begin, block);
                break;
            }
                
            case MessageType::CANCEL: {
                if (payload.size() != 12) {
                    throw std::runtime_error("Invalid CANCEL message length");
                }
                uint32_t index = (payload[0] << 24) | (payload[1] << 16) | (payload[2] << 8) | payload[3];
                uint32_t begin = (payload[4] << 24) | (payload[5] << 16) | (payload[6] << 8) | payload[7];
                uint32_t length = (payload[8] << 24) | (payload[9] << 16) | (payload[10] << 8) | payload[11];
                handle_cancel(index, begin, length);
                break;
            }
                
            case MessageType::PORT:
                if (payload.size() != 2) {
                    throw std::runtime_error("Invalid PORT message length");
                }
                handle_port((payload[0] << 8) | payload[1]);
                break;
                
            default:
                // Unknown message type, ignore
                break;
        }
    } catch (const std::exception& e) {
        handle_error(std::string("Error processing message: ") + e.what());
    }
}

void PeerConnection::handle_handshake(const std::vector<uint8_t>& data) {
    if (data.size() < HandshakeMessage::HANDSHAKE_LENGTH) {
        throw std::runtime_error("Handshake message too short");
    }
    
    HandshakeMessage handshake = HandshakeMessage::deserialize(data);
    
    // Verify info hash
    if (handshake.info_hash != info_hash_) {
        throw std::runtime_error("Mismatched info hash");
    }
    
    // Store peer ID
    peer_id_ = handshake.peer_id;
    
    // If we're the initiator, we've already sent our handshake
    // If we're the receiver, send our handshake now
    if (state_ == PeerState::HANDSHAKING) {
        if (!send_handshake()) {
            throw std::runtime_error("Failed to send handshake");
        }
    }
    
    // Send bitfield (if we have any pieces)
    // TODO: Implement bitfield sending when we have pieces
    
    // Send interested
    InterestedMessage interested_msg;
    auto interested_data = interested_msg.serialize();
    if (!send_message(interested_data)) {
        throw std::runtime_error("Failed to send INTERESTED message");
    }
    
    set_state(PeerState::CONNECTED);
}

void PeerConnection::handle_keep_alive() {
    // No action needed for keep-alive
}

void PeerConnection::handle_choke() {
    std::lock_guard<std::mutex> lock(mutex_);
    peer_choking_ = true;
    
    // Cancel all pending requests
    std::lock_guard<std::mutex> req_lock(request_mutex_);
    pending_requests_.clear();
}

void PeerConnection::handle_unchoke() {
    std::lock_guard<std::mutex> lock(mutex_);
    peer_choking_ = false;
}

void PeerConnection::handle_interested() {
    std::lock_guard<std::mutex> lock(mutex_);
    peer_interested_ = true;
}

void PeerConnection::handle_not_interested() {
    std::lock_guard<std::mutex> lock(mutex_);
    peer_interested_ = false;
}

void PeerConnection::handle_have(uint32_t piece_index) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Resize bitfield if needed
    if (piece_index >= peer_bitfield_.size()) {
        peer_bitfield_.resize(piece_index + 1, false);
    }
    
    // Mark piece as available
    peer_bitfield_[piece_index] = true;
}

void PeerConnection::handle_bitfield(const std::vector<uint8_t>& bitfield, size_t num_pieces) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Convert bitfield to vector of bools
    peer_bitfield_.resize(num_pieces, false);
    
    for (size_t i = 0; i < num_pieces; ++i) {
        size_t byte_idx = i / 8;
        size_t bit_idx = 7 - (i % 8);
        
        if (byte_idx < bitfield.size()) {
            peer_bitfield_[i] = (bitfield[byte_idx] >> bit_idx) & 1;
        }
    }
}

void PeerConnection::handle_request(uint32_t piece_index, uint32_t begin, uint32_t length) {
    // TODO: Implement piece serving logic
    // For now, just ignore the request (we'll implement this when we have the file storage)
}

void PeerConnection::handle_piece(uint32_t piece_index, uint32_t begin, const std::vector<uint8_t>& block) {
    // Find the matching request
    std::lock_guard<std::mutex> lock(request_mutex_);
    
    for (auto it = pending_requests_.begin(); it != pending_requests_.end(); ) {
        const auto& req = it->second;
        if (req.piece_index == piece_index && 
            req.begin == begin && 
            req.length == block.size()) {
            
            // Found the matching request
            pending_requests_.erase(it);
            
            // Notify the callback
            if (piece_callback_) {
                piece_callback_(piece_index, block);
            }
            
            return;
        } else {
            ++it;
        }
    }
    
    // No matching request found
    handle_error("Received piece with no matching request");
}

void PeerConnection::handle_cancel(uint32_t piece_index, uint32_t begin, uint32_t length) {
    // For now, just ignore cancel messages
    // We'll implement this when we support uploading
}

void PeerConnection::handle_port(uint16_t port) {
    // For now, just ignore port messages
    // This is used for DHT, which we're not implementing yet
}

void PeerConnection::check_timeouts() {
    auto now = std::chrono::steady_clock::now();
    const auto TIMEOUT = 30s;
    
    std::lock_guard<std::mutex> lock(request_mutex_);
    
    for (auto it = pending_requests_.begin(); it != pending_requests_.end(); ) {
        const auto& req = it->second;
        
        if (now - req.timestamp > TIMEOUT) {
            // Request timed out, remove it
            it = pending_requests_.erase(it);
        } else {
            ++it;
        }
    }
}

// PeerManager implementation
PeerManager::PeerManager(const std::string& info_hash, const std::string& our_peer_id)
    : info_hash_(info_hash), our_peer_id_(our_peer_id) {}

PeerManager::~PeerManager() {
    stop();
}

void PeerManager::start() {
    if (running_) {
        return;
    }
    
    running_ = true;
    manager_thread_ = std::thread(&PeerManager::manager_loop, this);
}

void PeerManager::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    // Stop all peer connections
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (auto& pair : peers_) {
            pair.second->stop();
        }
        peers_.clear();
    }
    
    // Wait for the manager thread to finish
    if (manager_thread_.joinable()) {
        manager_thread_.join();
    }
}

void PeerManager::add_peer(const std::string& peer_id, const std::string& ip, uint16_t port) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    
    // Don't add duplicate peers
    if (peers_.find(peer_id) != peers_.end()) {
        return;
    }
    
    // Don't connect to ourselves
    if (peer_id == our_peer_id_) {
        return;
    }
    
    // Create and start the peer connection
    auto peer = std::make_shared<PeerConnection>(peer_id, ip, port, info_hash_, our_peer_id_);
    
    // Set up callbacks
    peer->set_piece_callback([this](uint32_t piece_index, const std::vector<uint8_t>& data) {
        handle_piece(piece_index, data);
    });
    
    peer->set_state_callback([this](const std::string& peer_id, PeerState state) {
        handle_peer_state_change(peer_id, state);
    });
    
    peer->set_error_callback([this](const std::string& error) {
        handle_error(error);
    });
    
    // Add to our map and start the connection
    peers_[peer_id] = peer;
    peer->start();
}

void PeerManager::remove_peer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    
    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
        it->second->stop();
        peers_.erase(it);
    }
}

std::shared_ptr<PeerConnection> PeerManager::get_peer(const std::string& peer_id) const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    
    auto it = peers_.find(peer_id);
    if (it != peers_.end()) {
        return it->second;
    }
    
    return nullptr;
}

std::vector<std::shared_ptr<PeerConnection>> PeerManager::get_all_peers() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    
    std::vector<std::shared_ptr<PeerConnection>> result;
    for (const auto& pair : peers_) {
        result.push_back(pair.second);
    }
    
    return result;
}

void PeerManager::manager_loop() {
    while (running_) {
        // Check for dead peers and remove them
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            
            for (auto it = peers_.begin(); it != peers_.end(); ) {
                if (it->second->get_state() == PeerState::DISCONNECTED) {
                    it = peers_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        
        // Sleep for a bit before checking again
        std::this_thread::sleep_for(5s);
    }
}

void PeerManager::handle_peer_state_change(const std::string& peer_id, PeerState state) {
    if (peer_state_callback_) {
        peer_state_callback_(peer_id, state);
    }
}

void PeerManager::handle_piece(uint32_t piece_index, const std::vector<uint8_t>& data) {
    if (piece_callback_) {
        piece_callback_(piece_index, data);
    }
}

void PeerManager::handle_error(const std::string& error) {
    if (error_callback_) {
        error_callback_(error);
    }
}

} // namespace p2p
