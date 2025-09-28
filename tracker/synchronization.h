#ifndef SYNCHRONIZATION_H
#define SYNCHRONIZATION_H

#include <string>
#include <unordered_map>
#include <pthread.h>
#include <vector>
#include <set>
#include <cstdint>
#include <atomic>

using namespace std;

// Global flag to control the tracker loop
extern std::atomic<bool> sync_thread_run;

// Global mutex for login synchronization
extern pthread_mutex_t login_mutex;

// Global data structures
extern unordered_map<string, string> user_data;
extern unordered_map<string, bool> is_logged_in;
extern vector<string> all_groups;
extern unordered_map<string, string> group_admin;
extern unordered_map<string, set<string>> group_members;
extern unordered_map<string, set<string>> pending_requests;
extern unordered_map<string, pair<int, string>> user_ip_port;

// Mutexes for thread safety
extern pthread_mutex_t user_data_mutex;
extern pthread_mutex_t all_groups_mutex;
extern pthread_mutex_t group_admin_mutex;
extern pthread_mutex_t group_members_mutex;
extern pthread_mutex_t pending_requests_mutex;
extern pthread_mutex_t user_ip_port_mutex;

// Configuration structure for tracker sync
struct TrackerSyncConfig {
    string other_tracker_ip;
    int other_tracker_port;
    int sync_port;
    int sync_interval_sec;
    int sync_timeout_ms;
};

// Initialize synchronization system
bool initialize_sync_system(const TrackerSyncConfig& config);

// Start the sync server in a separate thread
bool start_sync_server();

// Main synchronization function (runs in a separate thread)
void synchronize_with_other_tracker();

// Handle incoming sync requests
void handle_tracker_sync_request(int sync_sock);

// Send tracker state to another tracker
void send_tracker_state(int sock);

// Receive tracker state from another tracker
void receive_tracker_state(int sock);

// Check if other tracker is alive
bool is_other_tracker_alive(const string& ip, int port);

// Shutdown the tracker sync system
void shutdown_sync_system();

// Get the current state version
uint64_t get_current_state_version();

// Force a sync with the other tracker immediately
void force_sync();

#endif // SYNCHRONIZATION_H