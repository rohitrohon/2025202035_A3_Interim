#include "tracker_client.h"
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <stdexcept>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

using namespace std::chrono;

namespace p2p {

bool TrackerClient::communicate_with_tracker(const std::string& tracker_url, 
                                          const std::string& request, 
                                          std::string& response) {
    // Parse tracker URL (format: "host:port")
    size_t colon_pos = tracker_url.find(':');
    if (colon_pos == std::string::npos) {
        return false;
    }
    
    std::string host = tracker_url.substr(0, colon_pos);
    int port = std::stoi(tracker_url.substr(colon_pos + 1));
    
    // Create socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return false;
    }
    
    // Set up server address
    struct sockaddr_in server_addr;
    struct hostent *server;
    
    server = gethostbyname(host.c_str());
    if (server == nullptr) {
        close(sockfd);
        return false;
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(port);
    
    // Connect to tracker
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sockfd);
        return false;
    }
    
    // Send request
    if (send(sockfd, request.c_str(), request.length(), 0) < 0) {
        close(sockfd);
        return false;
    }
    
    // Shutdown write side to signal end of request
    shutdown(sockfd, SHUT_WR);
    
    // Receive response
    char buffer[4096];
    ssize_t bytes_received;
    while ((bytes_received = recv(sockfd, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        response.append(buffer);
    }
    
    close(sockfd);
    return !response.empty();
}

// TrackerClient implementation
TrackerClient::TrackerClient(const std::string& peer_id, uint16_t port)
    : peer_id_(peer_id)
    , port_(port)
    , running_(false)
    , stats_() {
    stats_.last_update = std::chrono::steady_clock::now();
}

TrackerClient::~TrackerClient() {
    stop();
}

void TrackerClient::start() {
    if (running_.load()) {
        return;
    }
    
    running_ = true;
    announce_thread_ = std::thread(&TrackerClient::announce_loop, this);
}

void TrackerClient::stop() {
    if (!running_.load()) {
        return;
    }
    
    running_ = false;
    
    if (announce_thread_.joinable()) {
        announce_thread_.join();
    }
}

void TrackerClient::set_peer_list_callback(PeerListCallback callback) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    peer_list_callback_ = std::move(callback);
}

void TrackerClient::set_error_callback(ErrorCallback callback) {
    error_callback_ = std::move(callback);
}

void TrackerClient::add_tracker(const std::string& url) {
    std::lock_guard<std::mutex> lock(trackers_mutex_);
    
    // Check if tracker already exists
    auto it = std::find_if(trackers_.begin(), trackers_.end(),
                          [&url](const TrackerState& t) { return t.url == url; });
    
    if (it == trackers_.end()) {
        TrackerState tracker;
        tracker.url = url;
        trackers_.push_back(tracker);
    }
}

void TrackerClient::remove_tracker(const std::string& url) {
    std::lock_guard<std::mutex> lock(trackers_mutex_);
    
    trackers_.erase(
        std::remove_if(trackers_.begin(), trackers_.end(),
                      [&url](const TrackerState& t) { return t.url == url; }),
        trackers_.end()
    );
}

std::vector<std::string> TrackerClient::get_trackers() const {
    std::lock_guard<std::mutex> lock(trackers_mutex_);
    std::vector<std::string> urls;
    
    for (const auto& tracker : trackers_) {
        urls.push_back(tracker.url);
    }
    
    return urls;
}

void TrackerClient::announce(const std::string& info_hash, Event event) {
    std::lock_guard<std::mutex> lock(trackers_mutex_);
    
    for (auto& tracker : trackers_) {
        try {
            tracker.last_info_hash = info_hash;
            announce_to_tracker(tracker, info_hash, event);
        } catch (const std::exception& e) {
            handle_error(std::string("Error announcing to tracker: ") + e.what());
        }
    }
}

std::vector<PeerInfo> TrackerClient::get_peers() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    return peers_;
}

void TrackerClient::update_stats(uint64_t downloaded, uint64_t uploaded, uint64_t left) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    stats_.downloaded = downloaded;
    stats_.uploaded = uploaded;
    stats_.left = left;
    stats_.last_update = std::chrono::steady_clock::now();
}

TrackerStats TrackerClient::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void TrackerClient::announce_loop() {
    while (running_.load()) {
        try {
            // Get a copy of trackers to avoid holding the lock too long
            std::vector<TrackerState> trackers_copy;
            {
                std::lock_guard<std::mutex> lock(trackers_mutex_);
                trackers_copy = trackers_;
            }
            
            // Announce to each tracker
            for (auto& tracker : trackers_copy) {
                try {
                    if (tracker.last_info_hash.empty()) {
                        continue; // Skip if no info_hash has been set yet
                    }
                    
                    auto now = std::chrono::steady_clock::now();
                    auto time_since_last = std::chrono::duration_cast<std::chrono::seconds>(
                        now - tracker.last_announce).count();
                    
                    // Only announce if enough time has passed
                    if (!tracker.announced || time_since_last >= tracker.interval) {
                        announce_to_tracker(tracker, tracker.last_info_hash, Event::NONE);
                        tracker.last_announce = now;
                        tracker.announced = true;
                    }
                } catch (const std::exception& e) {
                    handle_error(std::string("Error in announce loop: ") + e.what());
                }
            }
            
            // Sleep for a short time before checking again
            std::this_thread::sleep_for(std::chrono::seconds(5));
            
        } catch (const std::exception& e) {
            handle_error(std::string("Error in announce loop: ") + e.what());
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    }
}

TrackerResponse TrackerClient::announce_to_tracker(TrackerState& tracker, 
                                                 const std::string& info_hash, 
                                                 Event event) {
    // Build the announce request
    std::ostringstream request_ss;
    request_ss << "ANNOUNCE " << info_hash << " HTTP/1.1\r\n";
    request_ss << "Peer-ID: " << peer_id_ << "\r\n";
    request_ss << "Port: " << port_ << "\r\n";
    request_ss << "Uploaded: " << stats_.uploaded << "\r\n";
    request_ss << "Downloaded: " << stats_.downloaded << "\r\n";
    request_ss << "Left: " << stats_.left << "\r\n";
    
    if (event != Event::NONE) {
        request_ss << "Event: " << event_to_string(event) << "\r\n";
    }
    
    if (!tracker.tracker_id.empty()) {
        request_ss << "Tracker-ID: " << tracker.tracker_id << "\r\n";
    }
    
    request_ss << "\r\n"; // End of headers
    
    std::string request = request_ss.str();
    std::string response;
    
    // Send the request to the tracker
    if (!communicate_with_tracker(tracker.url, request, response)) {
        TrackerResponse error_response;
        error_response.failure_reason = "Failed to connect to tracker";
        return error_response;
    }
    
    // Parse the response
    return parse_tracker_response(response);
}

TrackerResponse TrackerClient::parse_tracker_response(const std::string& response) const {
    TrackerResponse result;
    
    // Simple response parsing (in a real implementation, you would parse the actual response format)
    // For now, we'll just return a success response with empty peer list
    result.interval = 1800; // 30 minutes
    result.min_interval = 300; // 5 minutes
    
    return result;
}

std::string TrackerClient::event_to_string(Event event) const {
    switch (event) {
        case Event::STARTED:   return "started";
        case Event::STOPPED:   return "stopped";
        case Event::COMPLETED: return "completed";
        default:               return "";
    }
}

void TrackerClient::handle_error(const std::string& error) {
    if (error_callback_) {
        error_callback_(error);
    }
}

void TrackerClient::update_peers(const std::vector<PeerInfo>& new_peers) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    
    // Merge new peers with existing ones
    for (const auto& new_peer : new_peers) {
        auto it = std::find_if(peers_.begin(), peers_.end(),
                             [&new_peer](const PeerInfo& p) { 
                                 return p.ip == new_peer.ip && p.port == new_peer.port; 
                             });
        
        if (it == peers_.end()) {
            peers_.push_back(new_peer);
        }
    }
    
    // Notify the callback if set
    if (peer_list_callback_) {
        peer_list_callback_(peers_);
    }
}

} // namespace p2p
