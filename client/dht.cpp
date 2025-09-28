#include "dht.h"
#include <algorithm>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/select.h>
#include <openssl/sha.h>

namespace p2p {

// DhtBucket implementation
DhtBucket::DhtBucket(const NodeId& min, const NodeId& max) 
    : min_id_(min), max_id_(max), last_changed_(std::chrono::steady_clock::now()) {}

bool DhtBucket::contains(const NodeId& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::find_if(nodes_.begin(), nodes_.end(), 
        [&id](const DhtNode& node) { return node.id == id; }) != nodes_.end();
}

bool DhtBucket::is_full() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nodes_.size() >= MAX_NODES;
}

bool DhtBucket::add_node(const DhtNode& node) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check if node already exists
    auto it = std::find_if(nodes_.begin(), nodes_.end(), 
        [&node](const DhtNode& n) { return n.id == node.id; });
    
    if (it != nodes_.end()) {
        // Update existing node
        *it = node;
        last_changed_ = std::chrono::steady_clock::now();
        return true;
    }
    
    // If bucket is full, don't add
    if (nodes_.size() >= MAX_NODES) {
        return false;
    }
    
    // Add new node
    nodes_.push_back(node);
    last_changed_ = std::chrono::steady_clock::now();
    return true;
}

bool DhtBucket::remove_node(const NodeId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::remove_if(nodes_.begin(), nodes_.end(),
        [&id](const DhtNode& node) { return node.id == id; });
    
    if (it != nodes_.end()) {
        nodes_.erase(it, nodes_.end());
        last_changed_ = std::chrono::steady_clock::now();
        return true;
    }
    return false;
}

std::vector<DhtNode> DhtBucket::get_nodes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return nodes_;
}

DhtNode* DhtBucket::find_node(const NodeId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
        [&id](const DhtNode& node) { return node.id == id; });
    return it != nodes_.end() ? &(*it) : nullptr;
}

std::chrono::steady_clock::time_point DhtBucket::last_changed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_changed_;
}

// Helper function to calculate distance between two node IDs
static size_t calculate_distance(const NodeId& a, const NodeId& b) {
    for (size_t i = 0; i < a.size(); ++i) {
        uint8_t x = a[i] ^ b[i];
        if (x != 0) {
            // Find the first differing bit
            for (int j = 0; j < 8; ++j) {
                if (x & (1 << (7 - j))) {
                    return i * 8 + j;
                }
            }
        }
    }
    return a.size() * 8; // If IDs are identical
}

// Helper function to convert NodeId to string
static std::string node_id_to_string(const NodeId& id) {
    return std::string(reinterpret_cast<const char*>(id.data()), id.size());
}

// Helper function to convert string to NodeId
static NodeId string_to_node_id(const std::string& str) {
    if (str.size() != 20) {
        throw std::runtime_error("Invalid node ID length");
    }
    NodeId id;
    std::copy(str.begin(), str.end(), id.begin());
    return id;
}

// DHT implementation
Dht::Dht(const std::string& node_id_str, uint16_t port)
    : port_(port) {
    // Initialize node ID
    if (node_id_str.empty()) {
        // Generate a random node ID if none provided
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint8_t> dist(0, 255);
        for (auto& b : node_id_) {
            b = dist(gen);
        }
    } else {
        if (node_id_str.size() != 20) {
            throw std::runtime_error("Invalid node ID length, expected 20 bytes");
        }
        std::copy(node_id_str.begin(), node_id_str.end(), node_id_.begin());
    }
    
    // Initialize the routing table with a single bucket that spans the entire ID space
    NodeId min_id = {};
    NodeId max_id;
    std::fill(max_id.begin(), max_id.end(), 0xFF);
    buckets_.push_back(std::make_unique<DhtBucket>(min_id, max_id));
    
    // Initialize random number generator for transaction IDs
    std::random_device rd;
    rng_.seed(rd());
}

Dht::~Dht() {
    stop();
}

void Dht::start() {
    if (running_) return;
    
    // Create socket
    socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ < 0) {
        if (error_callback_) {
            error_callback_("Failed to create socket: " + std::string(strerror(errno)));
        }
        return;
    }
    
    // Set socket to non-blocking
    int flags = fcntl(socket_, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_, F_SETFL, flags | O_NONBLOCK) < 0) {
        if (error_callback_) {
            error_callback_("Failed to set socket to non-blocking: " + std::string(strerror(errno)));
        }
        close(socket_);
        return;
    }
    
    // Bind socket
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    if (bind(socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (error_callback_) {
            error_callback_("Failed to bind socket: " + std::string(strerror(errno)));
        }
        close(socket_);
        return;
    }
    
    // Start network thread
    running_ = true;
    network_thread_ = std::thread(&Dht::network_loop, this);
    
    // Bootstrap if we have bootstrap nodes
    if (!bootstrap_nodes_.empty()) {
        for (const auto& node : bootstrap_nodes_) {
            send_ping(node.first, node.second);
        }
    }
}

void Dht::stop() {
    if (!running_) return;
    
    running_ = false;
    
    if (network_thread_.joinable()) {
        network_thread_.join();
    }
    
    if (socket_ >= 0) {
        close(socket_);
        socket_ = -1;
    }
}

void Dht::add_bootstrap_node(const std::string& address, uint16_t port) {
    bootstrap_nodes_.emplace_back(address, port);
    
    // If we're already running, ping the new bootstrap node
    if (running_) {
        send_ping(address, port);
    }
}

void Dht::find_peers(const std::vector<uint8_t>& info_hash, const PeersFoundCallback& callback) {
    if (info_hash.size() != 20) {
        if (error_callback_) {
            error_callback_("Invalid info hash size: " + std::to_string(info_hash.size()));
        }
        return;
    }
    
    // Find the closest nodes to the info hash
    NodeId target_id;
    std::memcpy(target_id.data(), info_hash.data(), 20);
    
    auto nodes = find_closest_nodes(target_id, 8);
    
    if (nodes.empty()) {
        if (error_callback_) {
            error_callback_("No nodes available for peer lookup");
        }
        return;
    }
    
    // Send GET_PEERS to the closest nodes
    for (const auto& node : nodes) {
        send_get_peers(node.address, node.port, info_hash, 
            [this, callback](const std::vector<DhtPeer>& peers, 
                            const std::vector<DhtNode>& nodes, 
                            const std::string& token) {
                
                if (!peers.empty() && callback) {
                    callback(peers);
                }
                
                // Add any new nodes to our routing table
                for (const auto& node : nodes) {
                    add_node(node);
                }
            });
    }
}

void Dht::announce_peer(const std::vector<uint8_t>& info_hash, uint16_t port) {
    if (info_hash.size() != 20) {
        if (error_callback_) {
            error_callback_("Invalid info hash size: " + std::to_string(info_hash.size()));
        }
        return;
    }
    
    // Find the closest nodes to the info hash
    NodeId target_id;
    std::memcpy(target_id.data(), info_hash.data(), 20);
    
    auto nodes = find_closest_nodes(target_id, 8);
    
    if (nodes.empty()) {
        if (error_callback_) {
            error_callback_("No nodes available for announce");
        }
        return;
    }
    
    // First, get a token from each node
    for (const auto& node : nodes) {
        // Store node in local variable to capture it by value in the lambda
        const std::string node_address = node.address;
        const uint16_t node_port = node.port;
        
        send_get_peers(node_address, node_port, info_hash,
            [this, port, info_hash, node_address, node_port]
            (const std::vector<DhtPeer>& /*peers*/, 
             const std::vector<DhtNode>& /*nodes*/, 
             const std::string& token) {
                
                if (!token.empty()) {
                    // Now announce our peer
                    send_announce_peer(node_address, node_port, info_hash, port, token);
                }
            });
    }
}

NodeId Dht::get_node_id() const {
    return node_id_;
}

size_t Dht::get_node_count() const {
    size_t count = 0;
    for (const auto& bucket : buckets_) {
        count += bucket->get_nodes().size();
    }
    return count;
}

void Dht::set_peers_found_callback(PeersFoundCallback callback) {
    peers_found_callback_ = std::move(callback);
}

void Dht::set_node_discovered_callback(NodeDiscoveredCallback callback) {
    node_discovered_callback_ = std::move(callback);
}

void Dht::set_error_callback(ErrorCallback callback) {
    error_callback_ = std::move(callback);
}

void Dht::network_loop() {
    uint8_t buffer[65536];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    
    while (running_) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(socket_, &read_fds);
        
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int ret = select(socket_ + 1, &read_fds, nullptr, nullptr, &tv);
        
        if (ret < 0) {
            if (errno == EINTR) continue;
            if (error_callback_) {
                error_callback_("Select error: " + std::string(strerror(errno)));
            }
            break;
        }
        
        if (ret == 0) continue; // Timeout
        
        if (FD_ISSET(socket_, &read_fds)) {
            ssize_t len = recvfrom(socket_, buffer, sizeof(buffer), 0, 
                                 (struct sockaddr*)&from_addr, &from_len);
            
            if (len > 0) {
                char addr_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &from_addr.sin_addr, addr_str, INET_ADDRSTRLEN);
                uint16_t port = ntohs(from_addr.sin_port);
                
                try {
                    process_packet(buffer, len, addr_str, port);
                } catch (const std::exception& e) {
                    if (error_callback_) {
                        error_callback_(std::string("Error processing packet: ") + e.what());
                    }
                }
            }
        }
    }
}

void Dht::process_packet(const uint8_t* data, size_t size, const std::string& from_addr, uint16_t from_port) {
    try {
        DhtMessage msg = decode_message(data, size);
        
        // Add the node to our routing table if it's a query or response
        if (msg.type == DhtMessageType::QUERY || msg.type == DhtMessageType::RESPONSE) {
            DhtNode node;
            node.id = msg.node_id;  // Assuming node_id is set in decode_message
            node.address = from_addr;
            node.port = from_port;
            node.last_seen = std::chrono::steady_clock::now();
            add_node(node);
        }
        
        switch (msg.type) {
            case DhtMessageType::QUERY:
                handle_query(msg, from_addr, from_port);
                break;
            case DhtMessageType::RESPONSE: {
                std::lock_guard<std::mutex> lock(transactions_mutex_);
                auto it = transactions_.find(msg.transaction_id);
                if (it != transactions_.end()) {
                    it->second.callback(msg);
                    transactions_.erase(it);
                }
                break;
            }
            case DhtMessageType::ERROR:
                handle_error(msg);
                break;
            default:
                if (error_callback_) {
                    error_callback_("Received message with unknown type");
                }
                break;
        }
    } catch (const std::exception& e) {
        if (error_callback_) {
            error_callback_(std::string("Error processing packet: ") + e.what());
        }
    }
}

void Dht::handle_query(const DhtMessage& query, const std::string& from_addr, uint16_t from_port) {
    DhtMessage response;
    response.transaction_id = query.transaction_id;
    response.type = DhtMessageType::RESPONSE;
    response.node_id = node_id_;  // Set our node ID in the response
    
    // Add the node to our routing table if it's not our own node
    if (query.node_id != node_id_) {
        DhtNode node;
        node.id = query.node_id;
        node.address = from_addr;
        node.port = from_port;
        node.last_seen = std::chrono::steady_clock::now();
        add_node(node);
    }
    
    switch (query.query_type) {
        case DhtQueryType::PING: {
            // Simple ping response - nothing more to do
            break;
        }
        case DhtQueryType::FIND_NODE: {
            // Find nodes close to the target ID
            response.nodes = find_closest_nodes(query.target_id, 8);
            break;
        }
        case DhtQueryType::GET_PEERS: {
            // Generate a token for the announce
            response.token = generate_token(from_addr, from_port);
            
            // For now, we don't store peers, so just return the closest nodes
            response.nodes = find_closest_nodes(query.target_id, 8);
            break;
        }
        case DhtQueryType::ANNOUNCE_PEER: {
            // Verify the token
            if (verify_token(query.token, from_addr, from_port)) {
                // Add the peer to our local storage
                if (peers_found_callback_ && !query.info_hash.empty()) {
                    DhtPeer peer;
                    peer.id = std::string(query.node_id.begin(), query.node_id.end());
                    peer.address = from_addr;
                    peer.port = query.port;
                    peer.last_seen = std::chrono::steady_clock::now();
                    
                    std::vector<DhtPeer> peers = {peer};
                    peers_found_callback_(peers);
                }
            } else {
                // Invalid token, send error
                DhtMessage error;
                error.transaction_id = query.transaction_id;
                error.type = DhtMessageType::ERROR;
                error.error_code = DhtErrorCode::PROTOCOL;
                error.error_message = "Invalid token";
                send_message(error, from_addr, from_port);
                return;
            }
            break;
        }
        case DhtQueryType::GET_PEERS_EXTENDED: {
            // BEP 32: Extension for PeX
            // For now, handle it the same as GET_PEERS
            response.token = generate_token(from_addr, from_port);
            response.nodes = find_closest_nodes(query.target_id, 8);
            break;
        }
        default: {
            // Unknown query type
            DhtMessage error;
            error.transaction_id = query.transaction_id;
            error.type = DhtMessageType::ERROR;
            error.error_code = DhtErrorCode::METHOD_UNKNOWN;
            error.error_message = "Unknown method";
            send_message(error, from_addr, from_port);
            return;
        }
    }
    
    // Send the response
    send_message(response, from_addr, from_port);
}

void Dht::handle_response(const DhtMessage& response) {
    std::lock_guard<std::mutex> lock(transactions_mutex_);
    auto it = transactions_.find(response.transaction_id);
    if (it != transactions_.end()) {
        it->second.callback(response);
        transactions_.erase(it);
    }
}

void Dht::handle_error(const DhtMessage& error) {
    if (error_callback_) {
        error_callback_("DHT error: " + error.error_message);
    }
}

void Dht::add_node(const DhtNode& node) {
    // Don't add ourselves
    if (node.id == node_id_) {
        return;
    }
    
    // Calculate the distance to our node ID
    size_t distance = calculate_distance(node_id_, node.id);
    
    // Add to the appropriate bucket
    if (distance < buckets_.size()) {
        if (!buckets_[distance]->add_node(node)) {
            // Bucket is full, ping the oldest node
            auto nodes = buckets_[distance]->get_nodes();
            if (!nodes.empty()) {
                auto oldest = std::min_element(nodes.begin(), nodes.end(),
                    [](const DhtNode& a, const DhtNode& b) {
                        return a.last_seen < b.last_seen;
                    });
                
                // If the oldest node doesn't respond, replace it
                send_ping(oldest->address, oldest->port, 
                    [this, node, distance](bool responded) {
                        if (!responded) {
                            // Remove the old node and try adding again
                            buckets_[distance]->remove_node(oldest->id);
                            add_node(node);
                        }
                    });
            }
        } else if (node_discovered_callback_) {
            node_discovered_callback_(node);
        }
    }
}

void Dht::remove_node(const NodeId& id) {
    size_t distance = calculate_distance(node_id_, id);
    if (distance < buckets_.size()) {
        buckets_[distance]->remove_node(id);
    }
}

void Dht::refresh_buckets() {
    // For each bucket that hasn't been modified in a while, do a find_node on a random ID in that bucket
    auto now = std::chrono::steady_clock::now();
    
    for (size_t i = 0; i < buckets_.size(); ++i) {
        auto last_changed = buckets_[i]->last_changed();
        if (now - last_changed > std::chrono::minutes(15)) {
            // Generate a random ID in this bucket's range
            NodeId target_id;
            for (size_t j = 0; j < 20; ++j) {
                if (j < i / 8) {
                    target_id[j] = node_id_[j];
                } else if (j == i / 8) {
                    uint8_t mask = 0xFF << (8 - (i % 8));
                    target_id[j] = (node_id_[j] & mask) | (random_number<uint8_t>(0, 255) & ~mask);
                } else {
                    target_id[j] = random_number<uint8_t>(0, 255);
                }
            }
            
            // Find the closest nodes to this random ID
            auto nodes = find_closest_nodes(target_id, 8);
            
            // Send FIND_NODE to the closest nodes we know
            for (const auto& node : nodes) {
                send_find_node(node.address, node.port, target_id);
            }
            
            // Mark this bucket as refreshed
            buckets_[i]->add_node(node_id_);
        }
    }
}

std::string Dht::generate_transaction_id() {
    std::string id(2, 0);
    id[0] = random_number<uint8_t>(0, 255);
    id[1] = random_number<uint8_t>(0, 255);
    return id;
}

void Dht::send_message(const DhtMessage& msg, const std::string& address, uint16_t port) {
    std::vector<uint8_t> data = encode_message(msg);
    
    struct sockaddr_in dest_addr = {};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);
    inet_pton(AF_INET, address.c_str(), &dest_addr.sin_addr);
    
    ssize_t sent = sendto(socket_, data.data(), data.size(), 0,
                         (struct sockaddr*)&dest_addr, sizeof(dest_addr));
    
    if (sent != static_cast<ssize_t>(data.size())) {
        if (error_callback_) {
            error_callback_("Failed to send message to " + address + ":" + std::to_string(port));
        }
    }
}

void Dht::send_ping(const std::string& address, uint16_t port, const std::function<void(bool)>& callback) {
    DhtMessage msg;
    msg.transaction_id = generate_transaction_id();
    msg.type = DhtMessageType::QUERY;
    msg.query_type = DhtQueryType::PING;
    
    if (callback) {
        std::lock_guard<std::mutex> lock(transactions_mutex_);
        transactions_[msg.transaction_id] = {
            std::chrono::steady_clock::now(),
            [callback](const DhtMessage& response) {
                callback(true);
            }
        };
    }
    
    send_message(msg, address, port);
}

void Dht::send_find_node(const std::string& address, uint16_t port, 
                        const NodeId& target_id,
                        const std::function<void(const std::vector<DhtNode>&)>& callback) {
    DhtMessage msg;
    msg.transaction_id = generate_transaction_id();
    msg.type = DhtMessageType::QUERY;
    msg.query_type = DhtQueryType::FIND_NODE;
    msg.target_id = target_id;
    
    if (callback) {
        std::lock_guard<std::mutex> lock(transactions_mutex_);
        transactions_[msg.transaction_id] = {
            std::chrono::steady_clock::now(),
            [callback](const DhtMessage& response) {
                callback(response.nodes);
            }
        };
    }
    
    send_message(msg, address, port);
}

void Dht::send_get_peers(const std::string& address, uint16_t port,
                        const std::vector<uint8_t>& info_hash,
                        const std::function<void(const std::vector<DhtPeer>&, 
                                               const std::vector<DhtNode>&, 
                                               const std::string&)>& callback) {
    if (info_hash.size() != 20) {
        if (error_callback_) {
            error_callback_("Invalid info hash size: " + std::to_string(info_hash.size()));
        }
        return;
    }
    
    DhtMessage msg;
    msg.transaction_id = generate_transaction_id();
    msg.type = DhtMessageType::QUERY;
    msg.query_type = DhtQueryType::GET_PEERS;
    msg.info_hash = info_hash;
    
    if (callback) {
        std::lock_guard<std::mutex> lock(transactions_mutex_);
        transactions_[msg.transaction_id] = {
            std::chrono::steady_clock::now(),
            [callback](const DhtMessage& response) {
                callback(response.values, response.nodes, response.token_granted);
            }
        };
    }
    
    send_message(msg, address, port);
}

void Dht::send_announce_peer(const std::string& address, uint16_t port,
                           const std::vector<uint8_t>& info_hash, 
                           uint16_t peer_port,
                           const std::string& token,
                           const std::function<void(bool)>& callback) {
    if (info_hash.size() != 20) {
        if (error_callback_) {
            error_callback_("Invalid info hash size: " + std::to_string(info_hash.size()));
        }
        return;
    }
    
    DhtMessage msg;
    msg.transaction_id = generate_transaction_id();
    msg.type = DhtMessageType::QUERY;
    msg.query_type = DhtQueryType::ANNOUNCE_PEER;
    msg.info_hash = info_hash;
    msg.port = peer_port;
    msg.token = token;
    
    if (callback) {
        std::lock_guard<std::mutex> lock(transactions_mutex_);
        transactions_[msg.transaction_id] = {
            std::chrono::steady_clock::now(),
            [callback](const DhtMessage& response) {
                callback(true);
            }
        };
    }
    
    send_message(msg, address, port);
}

size_t Dht::get_distance_rank(const NodeId& a, const NodeId& b) {
    return calculate_distance(a, b);
}

std::vector<DhtNode> Dht::find_closest_nodes(const NodeId& target, size_t count) {
    std::vector<std::pair<size_t, DhtNode>> nodes_with_distance;
    
    // Get all nodes and calculate their distance to the target
    for (const auto& bucket : buckets_) {
        auto nodes = bucket->get_nodes();
        for (const auto& node : nodes) {
            size_t distance = calculate_distance(target, node.id);
            nodes_with_distance.emplace_back(distance, node);
        }
    }
    
    // Sort by distance
    std::sort(nodes_with_distance.begin(), nodes_with_distance.end(),
        [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
    
    // Return the closest 'count' nodes
    std::vector<DhtNode> result;
    for (size_t i = 0; i < std::min(count, nodes_with_distance.size()); ++i) {
        result.push_back(nodes_with_distance[i].second);
    }
    
    return result;
}

std::string Dht::generate_token(const std::string& address, uint16_t port) const {
    // In a real implementation, this would use a secret key to generate a token
    // that can be verified later. For simplicity, we'll just use a hash.
    std::string data = address + ":" + std::to_string(port);
    
    uint8_t hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const uint8_t*>(data.data()), data.size(), hash);
    
    // Return first 8 bytes as hex string
    std::ostringstream oss;
    for (int i = 0; i < 8; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    
    return oss.str();
}

bool Dht::verify_token(const std::string& token, const std::string& address, uint16_t port) const {
    // In a real implementation, this would verify the token using the same secret key
    // For now, we'll just regenerate it and compare
    return token == generate_token(address, port);
}

DhtMessage Dht::decode_message(const uint8_t* data, size_t size) {
    DhtMessage msg;
    std::string input(reinterpret_cast<const char*>(data), size);
    
    try {
        // Parse the Bencoded dictionary
        auto dict = bencode::decode(input);
        if (!dict.is_dict()) {
            throw std::runtime_error("Expected dictionary at root level");
        }
        
        // Get transaction ID
        const auto& t = dict.dict_find("t");
        if (!t || !t->is_string()) {
            throw std::runtime_error("Missing or invalid transaction ID");
        }
        msg.transaction_id = t->string_value();
        
        // Get message type
        const auto& y = dict.dict_find("y");
        if (!y || !y->is_string() || y->string_value().size() != 1) {
            throw std::runtime_error("Missing or invalid message type");
        }
        
        char msg_type = y->string_value()[0];
        switch (msg_type) {
            case 'q': msg.type = DhtMessageType::QUERY; break;
            case 'r': msg.type = DhtMessageType::RESPONSE; break;
            case 'e': msg.type = DhtMessageType::ERROR; break;
            default: throw std::runtime_error("Unknown message type");
        }
        
        if (msg.type == DhtMessageType::QUERY) {
            // Parse query
            const auto& q = dict.dict_find("q");
            if (!q || !q->is_string()) {
                throw std::runtime_error("Missing or invalid query type");
            }
            
            const auto& a = dict.dict_find("a");
            if (!a || !a->is_dict()) {
                throw std::runtime_error("Missing or invalid query arguments");
            }
            
            std::string query_type = q->string_value();
            if (query_type == "ping") {
                msg.query_type = DhtQueryType::PING;
            } else if (query_type == "find_node") {
                msg.query_type = DhtQueryType::FIND_NODE;
                const auto& target = a->dict_find("target");
                if (!target || !target->is_string() || target->string_value().size() != 20) {
                    throw std::runtime_error("Missing or invalid target ID");
                }
                msg.target_id = string_to_node_id(target->string_value());
            } else if (query_type == "get_peers") {
                msg.query_type = DhtQueryType::GET_PEERS;
                const auto& info_hash = a->dict_find("info_hash");
                if (!info_hash || !info_hash->is_string() || info_hash->string_value().size() != 20) {
                    throw std::runtime_error("Missing or invalid info_hash");
                }
                const auto& hash_str = info_hash->string_value();
                msg.info_hash.assign(hash_str.begin(), hash_str.end());
            } else if (query_type == "announce_peer") {
                msg.query_type = DhtQueryType::ANNOUNCE_PEER;
                const auto& info_hash = a->dict_find("info_hash");
                const auto& port = a->dict_find("port");
                const auto& token = a->dict_find("token");
                
                if (!info_hash || !info_hash->is_string() || info_hash->string_value().size() != 20 ||
                    !port || !port->is_int() || port->int_value() < 1 || port->int_value() > 65535 ||
                    !token || !token->is_string()) {
                    throw std::runtime_error("Invalid announce_peer arguments");
                }
                
                const auto& hash_str = info_hash->string_value();
                msg.info_hash.assign(hash_str.begin(), hash_str.end());
                msg.port = port->int_value();
                msg.token = token->string_value();
            } else {
                msg.query_type = DhtQueryType::UNKNOWN;
            }
            
            // Get node ID from query
            const auto& id = a->dict_find("id");
            if (!id || !id->is_string() || id->string_value().size() != 20) {
                throw std::runtime_error("Missing or invalid node ID in query");
            }
            msg.node_id = string_to_node_id(id->string_value());
            
        } else if (msg.type == DhtMessageType::RESPONSE) {
            // Parse response
            const auto& r = dict.dict_find("r");
            if (!r || !r->is_dict()) {
                throw std::runtime_error("Missing or invalid response");
            }
            
            // Get node ID from response
            const auto& id = r->dict_find("id");
            if (!id || !id->is_string() || id->string_value().size() != 20) {
                throw std::runtime_error("Missing or invalid node ID in response");
            }
            msg.node_id = string_to_node_id(id->string_value());
            
            // Check for nodes field (for find_node response)
            const auto& nodes = r->dict_find("nodes");
            if (nodes && nodes->is_string()) {
                const std::string& nodes_str = nodes->string_value();
                if (nodes_str.size() % 26 != 0) {
                    throw std::runtime_error("Invalid nodes field length");
                }
                
                for (size_t i = 0; i < nodes_str.size(); i += 26) {
                    DhtNode node;
                    node.id = string_to_node_id(nodes_str.substr(i, 20));
                    
                    // Parse IP address (4 bytes)
                    uint32_t ip = 0;
                    for (int j = 0; j < 4; ++j) {
                        ip = (ip << 8) | (uint8_t)nodes_str[i + 20 + j];
                    }
                    
                    // Parse port (2 bytes, network byte order)
                    uint16_t port = (nodes_str[i + 24] << 8) | (uint8_t)nodes_str[i + 25];
                    
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &ip, ip_str, INET_ADDRSTRLEN);
                    
                    node.address = ip_str;
                    node.port = port;
                    node.last_seen = std::chrono::steady_clock::now();
                    
                    msg.nodes.push_back(node);
                }
            }
            
            // Check for values field (for get_peers response with peers)
            const auto& values = r->dict_find("values");
            if (values && values->is_list()) {
                for (const auto& value : values->list_value()) {
                    if (!value.is_string()) continue;
                    const std::string& peer_str = value.string_value();
                    if (peer_str.size() != 6) continue; // 4 bytes IP + 2 bytes port
                    
                    DhtPeer peer;
                    
                    // Parse IP address (4 bytes)
                    uint32_t ip = 0;
                    for (int j = 0; j < 4; ++j) {
                        ip = (ip << 8) | (uint8_t)peer_str[j];
                    }
                    
                    // Parse port (2 bytes, network byte order)
                    peer.port = (peer_str[4] << 8) | (uint8_t)peer_str[5];
                    
                    char ip_str[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &ip, ip_str, INET_ADDRSTRLEN);
                    peer.address = ip_str;
                    
                    msg.peers.push_back(peer);
                }
            }
            
            // Check for token field (for get_peers response)
            const auto& token = r->dict_find("token");
            if (token && token->is_string()) {
                msg.token = token->string_value();
            }
            
        } else if (msg.type == DhtMessageType::ERROR) {
            // Parse error
            const auto& e = dict.dict_find("e");
            if (!e || !e->is_list() || e->list_value().size() < 2 ||
                !e->list_value()[0].is_int() || !e->list_value()[1].is_string()) {
                throw std::runtime_error("Invalid error message format");
            }
            
            msg.error_code = static_cast<DhtErrorCode>(e->list_value()[0].int_value());
            msg.error_message = e->list_value()[1].string_value();
        }
        
        return msg;
        
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Failed to decode message: ") + e.what());
    }
}

std::vector<uint8_t> Dht::encode_message(const DhtMessage& msg) {
    bencode::dict dict;
    
    // Common fields
    dict["t"] = bencode::string(msg.transaction_id);
    
    switch (msg.type) {
        case DhtMessageType::QUERY: {
            dict["y"] = bencode::string("q");
            
            // Set query type
            std::string query_type;
            switch (msg.query_type) {
                case DhtQueryType::PING: query_type = "ping"; break;
                case DhtQueryType::FIND_NODE: query_type = "find_node"; break;
                case DhtQueryType::GET_PEERS: query_type = "get_peers"; break;
                case DhtQueryType::ANNOUNCE_PEER: query_type = "announce_peer"; break;
                default: throw std::runtime_error("Unsupported query type");
            }
            dict["q"] = bencode::string(query_type);
            
            // Set query arguments
            bencode::dict args;
            args["id"] = bencode::string(node_id_to_string(msg.node_id));
            
            switch (msg.query_type) {
                case DhtQueryType::FIND_NODE:
                    args["target"] = bencode::string(node_id_to_string(msg.target_id));
                    break;
                    
                case DhtQueryType::GET_PEERS:
                    args["info_hash"] = bencode::string(
                        std::string(msg.info_hash.begin(), msg.info_hash.end()));
                    break;
                    
                case DhtQueryType::ANNOUNCE_PEER:
                    args["info_hash"] = bencode::string(
                        std::string(msg.info_hash.begin(), msg.info_hash.end()));
                    args["port"] = bencode::integer(msg.port);
                    args["token"] = bencode::string(msg.token);
                    break;
                    
                default:
                    break;
            }
            
            dict["a"] = args;
            break;
        }
            
        case DhtMessageType::RESPONSE: {
            dict["y"] = bencode::string("r");
            
            bencode::dict r;
            r["id"] = bencode::string(node_id_to_string(msg.node_id));
            
            // Add nodes if present (for find_node response)
            if (!msg.nodes.empty()) {
                std::string nodes_str;
                for (const auto& node : msg.nodes) {
                    // Add node ID (20 bytes)
                    nodes_str += node_id_to_string(node.id);
                    
                    // Add IP address (4 bytes)
                    struct in_addr addr;
                    inet_pton(AF_INET, node.address.c_str(), &addr);
                    uint32_t ip = ntohl(addr.s_addr);
                    nodes_str.push_back(static_cast<char>((ip >> 24) & 0xFF));
                    nodes_str.push_back(static_cast<char>((ip >> 16) & 0xFF));
                    nodes_str.push_back(static_cast<char>((ip >> 8) & 0xFF));
                    nodes_str.push_back(static_cast<char>(ip & 0xFF));
                    
                    // Add port (2 bytes, network byte order)
                    nodes_str.push_back(static_cast<char>((node.port >> 8) & 0xFF));
                    nodes_str.push_back(static_cast<char>(node.port & 0xFF));
                }
                r["nodes"] = bencode::string(nodes_str);
            }
            
            // Add token if present (for get_peers response)
            if (!msg.token.empty()) {
                r["token"] = bencode::string(msg.token);
            }
            
            // Add values if present (for get_peers response with peers)
            if (!msg.peers.empty()) {
                bencode::list values;
                for (const auto& peer : msg.peers) {
                    std::string peer_str;
                    
                    // Add IP address (4 bytes)
                    struct in_addr addr;
                    inet_pton(AF_INET, peer.address.c_str(), &addr);
                    uint32_t ip = ntohl(addr.s_addr);
                    peer_str.push_back(static_cast<char>((ip >> 24) & 0xFF));
                    peer_str.push_back(static_cast<char>((ip >> 16) & 0xFF));
                    peer_str.push_back(static_cast<char>((ip >> 8) & 0xFF));
                    peer_str.push_back(static_cast<char>(ip & 0xFF));
                    
                    // Add port (2 bytes, network byte order)
                    peer_str.push_back(static_cast<char>((peer.port >> 8) & 0xFF));
                    peer_str.push_back(static_cast<char>(peer.port & 0xFF));
                    
                    values.push_back(bencode::string(peer_str));
                }
                r["values"] = values;
            }
            
            dict["r"] = r;
            break;
        }
            
        case DhtMessageType::ERROR: {
            dict["y"] = bencode::string("e");
            
            bencode::list error_list;
            error_list.push_back(bencode::integer(static_cast<int64_t>(msg.error_code)));
            error_list.push_back(bencode::string(msg.error_message));
            
            dict["e"] = error_list;
            break;
        }
            
        default:
            throw std::runtime_error("Unsupported message type");
    }
    
    // Encode to Bencode
    std::string encoded = bencode::encode(dict);
    return std::vector<uint8_t>(encoded.begin(), encoded.end());
}

} // namespace p2p
