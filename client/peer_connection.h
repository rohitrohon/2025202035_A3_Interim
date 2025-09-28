#ifndef PEER_CONNECTION_H
#define PEER_CONNECTION_H

#include "p2p_protocol.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <queue>
#include <map>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>  // For TCP_NODELAY
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

namespace p2p {

// Forward declarations
class PeerConnection;

// Callback types
using PieceCallback = std::function<void(uint32_t piece_index, const std::vector<uint8_t>& data)>;
using PeerStateCallback = std::function<void(const std::string& peer_id, PeerState state)>;
using ErrorCallback = std::function<void(const std::string& error)>;

struct PendingRequest {
    uint32_t piece_index;
    uint32_t begin;
    uint32_t length;
    std::chrono::time_point<std::chrono::steady_clock> timestamp;
};
// Represents a peer connection
class PeerConnection : public std::enable_shared_from_this<PeerConnection> {
public:
    // Constructor for outgoing connection
    PeerConnection(const std::string& peer_id, const std::string& peer_ip, uint16_t peer_port,
                  const std::string& info_hash, const std::string& our_peer_id);
    
    // Constructor for incoming connection
    PeerConnection(int sock_fd, const sockaddr_in& addr, 
                  const std::string& info_hash, const std::string& our_peer_id);
    
    ~PeerConnection();
    
    // Disable copying
    PeerConnection(const PeerConnection&) = delete;
    PeerConnection& operator=(const PeerConnection&) = delete;
    
    // Start the connection
    void start();
    
    // Stop the connection
    void stop();
    
    // Send a request for a piece
    void request_piece(uint32_t piece_index, uint32_t begin, uint32_t length);
    
    // Cancel a pending request
    void cancel_request(uint32_t piece_index, uint32_t begin, uint32_t length);
    
    // Set callbacks
    void set_piece_callback(PieceCallback callback) { piece_callback_ = std::move(callback); }
    void set_state_callback(PeerStateCallback callback) { state_callback_ = std::move(callback); }
    void set_error_callback(ErrorCallback callback) { error_callback_ = std::move(callback); }
    
    // Get peer information
    std::string get_peer_id() const { return peer_id_; }
    std::string get_peer_ip() const { return peer_ip_; }
    uint16_t get_peer_port() const { return peer_port_; }
    PeerState get_state() const { return state_; }
    
    // Get peer's bitfield
    std::vector<bool> get_bitfield() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return peer_bitfield_;
    }
    
    // Check if peer has a piece
    bool has_piece(uint32_t piece_index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return piece_index < peer_bitfield_.size() && peer_bitfield_[piece_index];
    }
    
    // Check if we are choked by this peer
    bool is_choked() const { 
        std::lock_guard<std::mutex> lock(mutex_);
        return am_choked_;
    }
    
    // Check if we are interested in this peer
    bool is_interested() const { 
        std::lock_guard<std::mutex> lock(mutex_);
        return am_interested_;
    }
    
    // Check if the peer is interested in us
    bool is_peer_interested() const { 
        std::lock_guard<std::mutex> lock(mutex_);
        return peer_interested_;
    }
    
    // Check if the peer is choking us
    bool is_peer_choking() const { 
        std::lock_guard<std::mutex> lock(mutex_);
        return peer_choking_;
    }
    
private:
    // Connection state
    std::string peer_id_;
    std::string peer_ip_;
    uint16_t peer_port_;
    std::string info_hash_;
    std::string our_peer_id_;
    
    // Socket and I/O
    int sock_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread io_thread_;
    
    // State
    std::atomic<PeerState> state_{PeerState::DISCONNECTED};
    
    // Peer state
    std::vector<bool> peer_bitfield_;
    bool peer_choking_ = true;
    bool peer_interested_ = false;
    bool am_choked_ = true;
    bool am_interested_ = false;
    
    // Callbacks
    PieceCallback piece_callback_;
    PeerStateCallback state_callback_;
    ErrorCallback error_callback_;
    
    // Request tracking
    std::mutex request_mutex_;
    std::map<uint64_t, PendingRequest> pending_requests_;
    
    // Buffer for incoming data
    std::vector<uint8_t> recv_buffer_;
    size_t message_length_ = 0;
    bool in_message_ = false;
    
    // Mutex for thread safety
    mutable std::mutex mutex_;
    
    // Private methods
    void set_state(PeerState new_state);
    void handle_error(const std::string& error);
    void io_loop();
    bool send_handshake();
    bool send_message(const std::vector<uint8_t>& data);
    bool process_incoming_data(std::vector<uint8_t>& data);
    void handle_message(std::vector<uint8_t>& message);
    void check_timeouts();
    void handle_handshake(const std::vector<uint8_t>& data);
    void handle_keep_alive();
    void handle_choke();
    void handle_unchoke();
    void handle_interested();
    void handle_not_interested();
    void handle_have(uint32_t piece_index);
    void handle_bitfield(const std::vector<uint8_t>& bitfield, size_t num_pieces);
    void handle_request(uint32_t piece_index, uint32_t begin, uint32_t length);
    void handle_piece(uint32_t piece_index, uint32_t begin, const std::vector<uint8_t>& block);
    void handle_cancel(uint32_t piece_index, uint32_t begin, uint32_t length);
    void handle_port(uint16_t port);
    
    // Helper to generate a unique request ID
    uint64_t generate_request_id() {
        static std::atomic<uint64_t> counter{0};
        return ++counter;
    }
};

// Peer connection manager
class PeerManager {
public:
    PeerManager(const std::string& info_hash, const std::string& our_peer_id);
    ~PeerManager();
    
    // Disable copying
    PeerManager(const PeerManager&) = delete;
    PeerManager& operator=(const PeerManager&) = delete;
    
    // Start/stop the peer manager
    void start();
    void stop();
    
    // Add a peer to connect to
    void add_peer(const std::string& peer_id, const std::string& ip, uint16_t port);
    
    // Remove a peer
    void remove_peer(const std::string& peer_id);
    
    // Get a peer connection
    std::shared_ptr<PeerConnection> get_peer(const std::string& peer_id) const;
    
    // Get all peer connections
    std::vector<std::shared_ptr<PeerConnection>> get_all_peers() const;
    
    // Set callbacks
    void set_piece_callback(PieceCallback callback) { piece_callback_ = std::move(callback); }
    void set_peer_state_callback(PeerStateCallback callback) { peer_state_callback_ = std::move(callback); }
    void set_error_callback(ErrorCallback callback) { error_callback_ = std::move(callback); }
    
private:
    std::string info_hash_;
    std::string our_peer_id_;
    std::atomic<bool> running_{false};
    
    mutable std::mutex peers_mutex_;
    std::map<std::string, std::shared_ptr<PeerConnection>> peers_;
    
    // Callbacks
    PieceCallback piece_callback_;
    PeerStateCallback peer_state_callback_;
    ErrorCallback error_callback_;
    
    // Thread for managing peers
    std::thread manager_thread_;
    
    // Private methods
    void manager_loop();
    void handle_peer_state_change(const std::string& peer_id, PeerState state);
    void handle_piece(uint32_t piece_index, const std::vector<uint8_t>& data);
    void handle_error(const std::string& error);
};

} // namespace p2p

#endif // PEER_CONNECTION_H
