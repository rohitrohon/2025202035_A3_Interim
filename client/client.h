#ifndef CLIENT_H
#define CLIENT_H

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>

using namespace std;

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

// Tracker event types
enum class TrackerEvent {
    NONE,
    STARTED,
    STOPPED,
    COMPLETED
};

// Client main functions
void start_listening(int listen_port);
void handle_peer_connection(int peer_sock);
int connect_to_tracker(const string& ip, int port, const string& client_ip_port);

// Tracker communication functions
bool communicate_with_tracker(const std::string& tracker_url, 
                            const std::string& request, 
                            std::string& response);

#endif