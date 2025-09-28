#ifndef TRACKER_CLIENT_H
#define TRACKER_CLIENT_H

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>

namespace p2p {

// Represents a peer in the network
struct PeerInfo {
    std::string peer_id;
    std::string ip;
    uint16_t port;
    uint64_t downloaded;
    uint64_t uploaded;
    uint64_t left;
    std::string event; // "started", "stopped", "completed", or empty
};

// Represents a tracker response
struct TrackerResponse {
    int interval;           // How often to re-announce (seconds)
    int min_interval;       // Minimum allowed announce interval
    std::string tracker_id; // For subsequent announces
    int complete;           // Number of seeders
    int incomplete;         // Number of leechers
    std::vector<PeerInfo> peers;
    std::string failure_reason; // If the request failed
    std::string warning_message; // Non-fatal warning
};

// Represents tracker statistics
struct TrackerStats {
    uint64_t downloaded = 0;
    uint64_t uploaded = 0;
    uint64_t left = 0;
    uint64_t corrupt = 0;
    uint64_t downloaded_prev = 0;
    uint64_t uploaded_prev = 0;
    std::chrono::steady_clock::time_point last_update;
};

class TrackerClient {
public:
    // Event types for announce
    enum class Event {
        NONE,
        STARTED,
        STOPPED,
        COMPLETED
    };
    
    // Callback type for peer list updates
    using PeerListCallback = std::function<void(const std::vector<PeerInfo>& peers)>;
    
    // Callback type for tracker errors
    using ErrorCallback = std::function<void(const std::string& error)>;
    
    // Constructor
    TrackerClient(const std::string& peer_id, uint16_t port);
    
    // Destructor
    ~TrackerClient();
    
    // Start the tracker client
    void start();
    
    void stop();
    
    // Set the peer list callback
    void set_peer_list_callback(PeerListCallback callback);
    
    // Set the error callback
    void set_error_callback(ErrorCallback callback);
    
    // Add a tracker URL
    void add_tracker(const std::string& url);
    
    // Remove a tracker URL
    void remove_tracker(const std::string& url);
    
    // Get the list of tracker URLs
    std::vector<std::string> get_trackers() const;
    
    // Announce to all trackers
    void announce(const std::string& info_hash, Event event = Event::NONE);
    
    // Get the list of peers
    std::vector<PeerInfo> get_peers() const;
    
    // Helper function to communicate with tracker
    bool communicate_with_tracker(const std::string& tracker_url, const std::string& request, std::string& response);
    
    // Update download/upload statistics
    void update_stats(uint64_t downloaded, uint64_t uploaded, uint64_t left);
    
    // Get the current tracker statistics
    TrackerStats get_stats() const;
    
private:
    struct TrackerState {
        std::string url;
        std::string tracker_id;
        int interval = 30 * 60; // Default 30 minutes
        int min_interval = 60;  // Default 1 minute
        std::chrono::steady_clock::time_point last_announce;
        std::string last_info_hash;
        bool announced = false;
        std::string failure_reason;
    };
    
    // Constants
    static constexpr int DEFAULT_ANNOUNCE_INTERVAL = 30 * 60; // 30 minutes
    static constexpr int MIN_ANNOUNCE_INTERVAL = 60;          // 1 minute
    static constexpr int MAX_FAILED_ATTEMPTS = 3;             // Max failed attempts before giving up
    
    // Member variables
    std::string peer_id_;
    uint16_t port_;
    std::string info_hash_;
    std::atomic<bool> running_{false};
    
    // Trackers
    mutable std::mutex trackers_mutex_;
    std::vector<TrackerState> trackers_;
    
    // Peer list
    mutable std::mutex peers_mutex_;
    std::vector<PeerInfo> peers_;
    
    // Statistics
    mutable std::mutex stats_mutex_;
    TrackerStats stats_;
    
    // Callbacks
    PeerListCallback peer_list_callback_;
    ErrorCallback error_callback_;
    
    // Threads
    std::thread announce_thread_;
    
    // Private methods
    void announce_loop();
    TrackerResponse announce_to_tracker(TrackerState& tracker, 
                                       const std::string& info_hash, 
                                       Event event);
    
    TrackerResponse parse_tracker_response(const std::string& response) const;
    std::string event_to_string(Event event) const;
    void handle_error(const std::string& error);
    void update_peers(const std::vector<PeerInfo>& new_peers);
};

    // Helper function to communicate with tracker
    bool communicate_with_tracker(const std::string& tracker_url, const std::string& request, std::string& response);

} // namespace p2p

#endif // TRACKER_CLIENT_H
