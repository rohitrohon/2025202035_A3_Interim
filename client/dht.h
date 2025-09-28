#ifndef DHT_H
#define DHT_H

#include <string>
#include <vector>
#include <map>
#include <set>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <random>
#include <chrono>
#include <array>

namespace p2p {

// DHT node ID (20 bytes)
using NodeId = std::array<uint8_t, 20>;

// DHT transaction ID (2 bytes)
using TransactionId = std::array<uint8_t, 2>;

// Represents a DHT node
struct DhtNode {
    NodeId id;
    std::string address;
    uint16_t port;
    std::chrono::steady_clock::time_point last_seen;
    uint8_t failed_queries = 0;
    
    bool operator==(const DhtNode& other) const {
        return id == other.id && address == other.address && port == other.port;
    }
    
    bool operator<(const DhtNode& other) const {
        if (id != other.id) return id < other.id;
        if (address != other.address) return address < other.address;
        return port < other.port;
    }
};

// Represents a peer found via DHT
struct DhtPeer {
    std::string id;
    std::string address;
    uint16_t port;
    std::chrono::steady_clock::time_point last_seen;
};

// DHT message types
enum class DhtMessageType {
    QUERY,
    RESPONSE,
    ERROR
};

// DHT query types
enum class DhtQueryType {
    PING = 0,
    FIND_NODE = 1,
    GET_PEERS = 2,
    ANNOUNCE_PEER = 3,
    // BEP 32: Extension for PeX
    GET_PEERS_EXTENDED = 4
};

// DHT error codes
enum class DhtErrorCode {
    GENERIC = 201,
    SERVER = 202,
    PROTOCOL = 203,
    METHOD_UNKNOWN = 204
};

// DHT message
struct DhtMessage {
    // Common fields
    DhtMessageType type = DhtMessageType::QUERY;  // Default to QUERY
    std::string transaction_id;
    std::string client_version; // For version handshake
    bool read_only = false;     // Indicates if the node is read-only
    NodeId node_id;             // ID of the node sending the message
    
    // Query-specific fields
    DhtQueryType query_type;
    NodeId target_id;           // For FIND_NODE, GET_PEERS
    std::vector<uint8_t> info_hash; // For GET_PEERS, ANNOUNCE_PEER
    uint16_t port = 0;          // For ANNOUNCE_PEER
    std::string token;          // For ANNOUNCE_PEER
    
    // Response fields
    std::vector<DhtNode> nodes;
    std::vector<DhtPeer> values; // Peers from GET_PEERS
    std::string token_granted;   // Token for future ANNOUNCE_PEER
    
    // Error fields
    DhtErrorCode error_code = DhtErrorCode::GENERIC;
    std::string error_message;
};

// DHT routing table bucket
class DhtBucket {
public:
    static constexpr size_t MAX_NODES = 8;
    
    DhtBucket(const NodeId& min, const NodeId& max);
    
    bool contains(const NodeId& id) const;
    bool is_full() const;
    bool add_node(const DhtNode& node);
    bool remove_node(const NodeId& id);
    std::vector<DhtNode> get_nodes() const;
    DhtNode* find_node(const NodeId& id);
    
    // Get the time of the last change
    std::chrono::steady_clock::time_point last_changed() const;
    
private:
    NodeId min_id_;
    NodeId max_id_;
    std::vector<DhtNode> nodes_;
    mutable std::mutex mutex_;
    std::chrono::steady_clock::time_point last_changed_;
};

// Main DHT class
class Dht {
public:
    // Callback types
    using PeersFoundCallback = std::function<void(const std::vector<DhtPeer>& peers)>;
    using NodeDiscoveredCallback = std::function<void(const DhtNode& node)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    
    // Constants
    static constexpr size_t BUCKET_COUNT = 160; // Number of bits in SHA-1
    static constexpr size_t MAX_BUCKET_SIZE = 8;
    static constexpr size_t MAX_NODES = 2000;
    static constexpr std::chrono::minutes NODE_EXPIRE_TIME{15};
    static constexpr std::chrono::minutes BUCKET_REFRESH_TIME{15};
    
    // Constructor/Destructor
    Dht(const std::string& node_id, uint16_t port);
    ~Dht();
    
    // Start/Stop the DHT
    void start();
    void stop();
    
    // Add bootstrap nodes
    void add_bootstrap_node(const std::string& address, uint16_t port);
    
    // Find peers for an info hash
    void find_peers(const std::vector<uint8_t>& info_hash, const PeersFoundCallback& callback);
    
    // Announce that we have a peer for an info hash
    void announce_peer(const std::vector<uint8_t>& info_hash, uint16_t port);
    
    // Get the current node ID
    NodeId get_node_id() const;
    
    // Get the number of known nodes
    size_t get_node_count() const;
    
    // Set callbacks
    void set_peers_found_callback(PeersFoundCallback callback);
    void set_node_discovered_callback(NodeDiscoveredCallback callback);
    void set_error_callback(ErrorCallback callback);
    
private:
    // Node state
    NodeId node_id_;
    uint16_t port_;
    std::atomic<bool> running_{false};
    
    // Network
    int socket_ = -1;
    std::thread network_thread_;
    
    // Routing table
    std::vector<std::unique_ptr<DhtBucket>> buckets_;
    std::mutex buckets_mutex_;
    
    // Bootstrap nodes
    std::vector<std::pair<std::string, uint16_t>> bootstrap_nodes_;
    
    // Callbacks
    PeersFoundCallback peers_found_callback_;
    NodeDiscoveredCallback node_discovered_callback_;
    ErrorCallback error_callback_;
    
    // Transaction tracking
    struct Transaction {
        std::chrono::steady_clock::time_point timestamp;
        std::function<void(const DhtMessage&)> callback;
    };
    
    std::map<std::string, Transaction> transactions_;
    std::mutex transactions_mutex_;
    std::mt19937 rng_;
    
    // Private methods
    void network_loop();
    void process_packet(const uint8_t* data, size_t size, const std::string& from_addr, uint16_t from_port);
    void handle_message(const DhtMessage& msg, const std::string& from_addr, uint16_t from_port);
    void handle_query(const DhtMessage& query, const std::string& from_addr, uint16_t from_port);
    void handle_response(const DhtMessage& response);
    void handle_error(const DhtMessage& error);
    
    // Node management
    void add_node(const DhtNode& node);
    void remove_node(const NodeId& id);
    void refresh_buckets();
    
    // Message sending
    std::string generate_transaction_id();
    void send_message(const DhtMessage& msg, const std::string& address, uint16_t port);
    void send_ping(const std::string& address, uint16_t port, 
                  const std::function<void(bool)>& callback = nullptr);
    void send_find_node(const std::string& address, uint16_t port, 
                       const NodeId& target_id,
                       const std::function<void(const std::vector<DhtNode>&)>& callback = nullptr);
    void send_get_peers(const std::string& address, uint16_t port,
                       const std::vector<uint8_t>& info_hash,
                       const std::function<void(const std::vector<DhtPeer>&, const std::vector<DhtNode>&, const std::string&)>& callback = nullptr);
    void send_announce_peer(const std::string& address, uint16_t port,
                           const std::vector<uint8_t>& info_hash, uint16_t peer_port,
                           const std::string& token,
                           const std::function<void(bool)>& callback = nullptr);
    
    // Helper functions
    static size_t get_distance_rank(const NodeId& a, const NodeId& b);
    std::vector<DhtNode> find_closest_nodes(const NodeId& target, size_t count);
    std::string generate_token(const std::string& address, uint16_t port) const;
    bool verify_token(const std::string& token, const std::string& address, uint16_t port) const;
    
    // Serialization/Deserialization
    static DhtMessage decode_message(const uint8_t* data, size_t size);
    static std::vector<uint8_t> encode_message(const DhtMessage& msg);
    
    // Thread-safe random number generation
    template<typename T>
    T random_number(T min, T max) {
        std::uniform_int_distribution<T> dist(min, max);
        return dist(rng_);
    }
};

} // namespace p2p

#endif // DHT_H
