#include "synchronization.h"
#include "data_structures.h"
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstring>
#include <vector>
#include <algorithm>
#include <sys/select.h>
#include <fcntl.h>
#include <signal.h>
#include <atomic>
#include <memory>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef __APPLE__
    #include <libkern/OSByteOrder.h>
    #define htobe64(x) OSSwapHostToBigInt64(x)
    #define be64toh(x) OSSwapBigToHostInt64(x)
#else
    #include <endian.h>
#endif

using namespace std;

// Constants
constexpr int SYNC_PORT = 8001;           // Default sync port for tracker 1
constexpr int OTHER_SYNC_PORT = 8002;     // Default sync port for tracker 2
constexpr int SYNC_INTERVAL_SEC = 5;      // How often to sync with other tracker
constexpr int SYNC_TIMEOUT_MS = 5000;     // 5 second timeout for sync operations
constexpr int MAX_SYNC_RETRIES = 3;       // Number of times to retry failed syncs

// Global variables
namespace {
    // Thread control
    thread sync_thread;
    thread server_thread;
    
    // Server socket
    int server_socket = -1;

    // Configuration
    TrackerSyncConfig sync_config;

    // State version counter
    atomic<uint64_t> state_version{0};
    
    // Mutex for thread-safe operations
    mutex sync_mutex;
    
    // Condition variable for forced sync
    condition_variable sync_cv;
    bool sync_requested = false;
}

// Define the global sync_thread_run variable
atomic<bool> sync_thread_run{true};

// Function to set socket options for better performance
bool set_socket_options(int sock) {
    if (sock < 0) return false;
    
    // Enable TCP_NODELAY to disable Nagle's algorithm
    int flag = 1;
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
        perror("setsockopt(TCP_NODELAY) failed");
        return false;
    }
    
    // Set send and receive timeouts
    struct timeval tv;
    tv.tv_sec = SYNC_TIMEOUT_MS / 1000;
    tv.tv_usec = (SYNC_TIMEOUT_MS % 1000) * 1000;
    
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt(SO_RCVTIMEO) failed");
        return false;
    }
    
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt(SO_SNDTIMEO) failed");
        return false;
    }
    
    return true;
}

// Initialize the synchronization system
bool initialize_sync_system(const TrackerSyncConfig& config) {
    sync_config = config;
    state_version = static_cast<uint64_t>(time(nullptr)) << 32;
    return true;
}

// Get the current state version
uint64_t get_current_state_version() {
    return state_version.load();
}

// Force a sync with the other tracker immediately
void force_sync() {
    unique_lock<mutex> lock(sync_mutex);
    sync_requested = true;
    sync_cv.notify_one();
}
// Shutdown the sync system
void shutdown_sync_system() {
    // Signal threads to stop
    sync_thread_run = false;
    
    // Close the server socket to unblock accept()
    if (server_socket >= 0) {
        int sock = server_socket;
        server_socket = -1;  // Mark as closed first
        shutdown(sock, SHUT_RDWR);
        close(sock);
    }
    
    // Wait for threads to finish with a timeout
    auto wait_for_thread = [](thread& t) {
        if (t.joinable()) {
            if (t.get_id() != this_thread::get_id()) {
                // Simple join with timeout
                for (int i = 0; i < 5 && t.joinable(); ++i) {
                    this_thread::sleep_for(chrono::milliseconds(100));
                }
                if (t.joinable()) {
                    t.detach();
                }
            } else {
                t.detach();
            }
        }
    };
    
    wait_for_thread(sync_thread);
    wait_for_thread(server_thread);
}

// Start the sync server to handle incoming sync requests
void sync_server_thread() {
    // Create server socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Failed to create server socket");
        return;
    }
    
    // Set SO_REUSEADDR to allow quick restart of the server
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
        close(server_socket);
        return;
    }
    
    // Bind to the specified port
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(sync_config.sync_port);
    
    if (::bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Failed to bind server socket");
        close(server_socket);
        return;
    }
    
    // Listen for incoming connections
    if (listen(server_socket, 5) < 0) {
        perror("Failed to listen on server socket");
        close(server_socket);
        return;
    }
    
    // Sync server started on port
    
    // Main server loop
    while (sync_thread_run) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        // Use select to make accept() non-blocking
        fd_set read_fds;
        FD_ZERO(&read_fds);
        
        // Only add server_socket to the set if it's still valid
        if (server_socket >= 0) {
            FD_SET(server_socket, &read_fds);
        } else {
            // If server_socket is invalid, sleep briefly and continue
            this_thread::sleep_for(chrono::milliseconds(100));
            continue;
        }
        
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int ready = select(server_socket + 1, &read_fds, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;  // Interrupted by signal
            if (errno == EBADF) {
                // Socket was closed, exit the loop
                break;
            }
            // Only log unexpected errors
            if (errno != EBADF) {
                perror("select() failed");
            }
            break;
        }
        
        if (ready == 0) continue;  // Timeout
        
        // Accept the connection
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            perror("accept() failed");
            continue;
        }
        
        // Set socket options
        set_socket_options(client_socket);
        
        // Handle the sync request in the current thread (for simplicity)
        // In a production system, you might want to handle this in a separate thread
        handle_tracker_sync_request(client_socket);
    }
    
    // Cleanup
    if (server_socket != -1) {
        close(server_socket);
        server_socket = -1;
    }
}

// Start the sync server in a separate thread
bool start_sync_server() {
    try {
        server_thread = thread(sync_server_thread);
        sync_thread = thread(synchronize_with_other_tracker);
        return true;
    } catch (const exception& e) {
        cerr << "Failed to start sync server: " << e.what() << endl;
        return false;
    }
}

// Helper function to serialize tracker state to a string
static string serialize_state() {
    stringstream ss;
    
    // Start with version
    ss << "VERSION:" << state_version << "\n";
    
    // Serialize user data
    pthread_mutex_lock(&user_data_mutex);
    ss << "USERS:{";
    for (const auto& [username, password] : user_data) {
        ss << username << "=" << password << ";";
    }
    ss << "}\n";
    pthread_mutex_unlock(&user_data_mutex);
    
    // Serialize login status
    pthread_mutex_lock(&login_mutex);
    ss << "LOGINS:{";
    for (const auto& [username, logged_in] : is_logged_in) {
        ss << username << "=" << (logged_in ? "1" : "0") << ";";
    }
    ss << "}\n";
    pthread_mutex_unlock(&login_mutex);
    
    // Serialize groups
    pthread_mutex_lock(&all_groups_mutex);
    ss << "GROUPS:{";
    for (const auto& group : all_groups) {
        ss << group << ";";
    }
    ss << "}\n";
    pthread_mutex_unlock(&all_groups_mutex);
    
    // Serialize group admins
    pthread_mutex_lock(&group_admin_mutex);
    ss << "GROUP_ADMINS:{";
    for (const auto& [group, admin] : group_admin) {
        ss << group << "=" << admin << ";";
    }
    ss << "}\n";
    pthread_mutex_unlock(&group_admin_mutex);
    
    // Serialize group members
    pthread_mutex_lock(&group_members_mutex);
    ss << "GROUP_MEMBERS:{";
    for (const auto& [group, members] : group_members) {
        ss << group << "=";
        for (const auto& member : members) {
            ss << member << ",";
        }
        ss << ";";
    }
    ss << "}\n";
    pthread_mutex_unlock(&group_members_mutex);
    
    // Serialize pending requests
    pthread_mutex_lock(&pending_requests_mutex);
    ss << "PENDING_REQUESTS:{";
    for (const auto& [group, users] : pending_requests) {
        ss << group << "=";
        for (const auto& user : users) {
            ss << user << ",";
        }
        ss << ";";
    }
    ss << "}\n";
    pthread_mutex_unlock(&pending_requests_mutex);
    
    // Serialize user IP/Port mappings
    pthread_mutex_lock(&user_ip_port_mutex);
    ss << "USER_CONNECTIONS:{";
    for (const auto& [user, conn] : user_ip_port) {
        ss << user << "=" << conn.second << ":" << conn.first << ";";
    }
    ss << "}\n";
    pthread_mutex_unlock(&user_ip_port_mutex);
    
    return ss.str();
}

// Helper function to deserialize and apply state from a string
static void apply_state(const string& state_str) {
    stringstream ss(state_str);
    string line;
    
    while (getline(ss, line)) {
        size_t colon_pos = line.find(':');
        if (colon_pos == string::npos) continue;
        
        string type = line.substr(0, colon_pos);
        string content = line.substr(colon_pos + 1);
        
        if (type == "VERSION") {
            // For now, just update our version to match
            state_version = stoull(content);
        }
        else if (type == "USERS") {
            // Format: {user1=pass1;user2=pass2;...}
            string users_str = content.substr(1, content.length() - 2);
            vector<string> user_entries;
            size_t pos = 0;
            while ((pos = users_str.find(';')) != string::npos) {
                string entry = users_str.substr(0, pos);
                if (!entry.empty()) {
                    user_entries.push_back(entry);
                }
                users_str.erase(0, pos + 1);
            }
            
            pthread_mutex_lock(&user_data_mutex);
            for (const auto& entry : user_entries) {
                size_t eq_pos = entry.find('=');
                if (eq_pos != string::npos) {
                    string user = entry.substr(0, eq_pos);
                    string pass = entry.substr(eq_pos + 1);
                    if (!user.empty()) {
                        user_data[user] = pass;
                    }
                }
            }
            pthread_mutex_unlock(&user_data_mutex);
        }
        else if (type == "GROUPS") {
            // Format: {group1;group2;...}
            string groups_str = content.substr(1, content.length() - 2);
            vector<string> groups;
            size_t pos = 0;
            while ((pos = groups_str.find(';')) != string::npos) {
                string group = groups_str.substr(0, pos);
                if (!group.empty()) {
                    groups.push_back(group);
                }
                groups_str.erase(0, pos + 1);
            }
            
            pthread_mutex_lock(&all_groups_mutex);
            // Merge groups instead of replacing
            for (const auto& group : groups) {
                if (find(all_groups.begin(), all_groups.end(), group) == all_groups.end()) {
                    all_groups.push_back(group);
                }
            }
            pthread_mutex_unlock(&all_groups_mutex);
        }
        else if (type == "GROUP_ADMINS") {
            // Format: {group1=admin1;group2=admin2;...}
            string admins_str = content.substr(1, content.length() - 2);
            unordered_map<string, string> admins;
            size_t pos = 0;
            while ((pos = admins_str.find(';')) != string::npos) {
                string entry = admins_str.substr(0, pos);
                size_t eq_pos = entry.find('=');
                if (eq_pos != string::npos) {
                    string group = entry.substr(0, eq_pos);
                    string admin = entry.substr(eq_pos + 1);
                    if (!group.empty() && !admin.empty()) {
                        admins[group] = admin;
                    }
                }
                admins_str.erase(0, pos + 1);
            }
            
            pthread_mutex_lock(&group_admin_mutex);
            // Merge group admins instead of replacing
            for (const auto& [group, admin] : admins) {
                // Only add if the group doesn't exist or if we're the admin
                if (group_admin.find(group) == group_admin.end() || 
                    group_admin[group] == admin) {  // Only update if admin is the same
                    group_admin[group] = admin;
                }
            }
            pthread_mutex_unlock(&group_admin_mutex);
        }
        else if (type == "GROUP_MEMBERS") {
            // Format: {group1=member1,member2;group2=member3,member4;...}
            string members_str = content.substr(1, content.length() - 2);
            unordered_map<string, set<string>> members;
            size_t pos = 0;
            while ((pos = members_str.find(';')) != string::npos) {
                string entry = members_str.substr(0, pos);
                size_t eq_pos = entry.find('=');
                if (eq_pos != string::npos) {
                    string group = entry.substr(0, eq_pos);
                    string members_list = entry.substr(eq_pos + 1);
                    set<string> member_set;
                    
                    size_t comma_pos = 0;
                    while ((comma_pos = members_list.find(',')) != string::npos) {
                        string member = members_list.substr(0, comma_pos);
                        if (!member.empty()) {
                            member_set.insert(member);
                        }
                        members_list.erase(0, comma_pos + 1);
                    }
                    if (!members_list.empty()) {
                        member_set.insert(members_list);
                    }
                    
                    if (!group.empty() && !member_set.empty()) {
                        members[group] = member_set;
                    }
                }
                members_str.erase(0, pos + 1);
            }
            
            pthread_mutex_lock(&group_members_mutex);
            // Merge group members instead of replacing
            for (const auto& [group, member_set] : members) {
                // If group doesn't exist, add it with its members
                if (group_members.find(group) == group_members.end()) {
                    group_members[group] = member_set;
                } else {
                    // Otherwise, merge the member sets
                    group_members[group].insert(member_set.begin(), member_set.end());
                }
            }
            pthread_mutex_unlock(&group_members_mutex);
        }
        else if (type == "PENDING_REQUESTS") {
            // Format: {group1=user1,user2;group2=user3,user4;...}
            string requests_str = content.substr(1, content.length() - 2);
            unordered_map<string, set<string>> new_requests;
            size_t pos = 0;
            
            while ((pos = requests_str.find(';')) != string::npos) {
                string entry = requests_str.substr(0, pos);
                size_t eq_pos = entry.find('=');
                if (eq_pos != string::npos) {
                    string group = entry.substr(0, eq_pos);
                    string users_str = entry.substr(eq_pos + 1);
                    set<string> users;
                    
                    size_t comma_pos = 0;
                    while ((comma_pos = users_str.find(',')) != string::npos) {
                        string user = users_str.substr(0, comma_pos);
                        if (!user.empty()) {
                            users.insert(user);
                        }
                        users_str.erase(0, comma_pos + 1);
                    }
                    if (!users_str.empty()) {
                        users.insert(users_str);
                    }
                    
                    if (!group.empty() && !users.empty()) {
                        new_requests[group] = users;
                    }
                }
                requests_str.erase(0, pos + 1);
            }
            
            // Merge pending requests
            pthread_mutex_lock(&pending_requests_mutex);
            for (const auto& [group, users] : new_requests) {
                // If group doesn't exist in our requests, add it
                if (pending_requests.find(group) == pending_requests.end()) {
                    pending_requests[group] = users;
                } else {
                    // Otherwise merge the user sets
                    pending_requests[group].insert(users.begin(), users.end());
                }
            }
            pthread_mutex_unlock(&pending_requests_mutex);
        }
        // Handle other data types as needed...
    }
}

// Function to send tracker state to another tracker
void send_tracker_state(int sock) {
    try {
        // Get the current state
        string state = serialize_state();
        uint64_t state_len = state.size();
            
        // Send state length (network byte order)
        uint64_t net_len = htobe64(state_len);
        if (send(sock, &net_len, sizeof(net_len), 0) != sizeof(net_len)) {
            throw runtime_error("Failed to send state length");
        }
            
        // Send the actual state
        size_t total_sent = 0;
        while (total_sent < state_len) {
            ssize_t sent = send(sock, state.data() + total_sent, 
                               state_len - total_sent, 0);
            if (sent <= 0) {
                throw runtime_error("Failed to send state data");
            }
            total_sent += sent;
        }
    } catch (const exception& e) {
        cerr << "Error in send_tracker_state: " << e.what() << endl;
        throw;
    }
}

// Function to receive tracker state from another tracker
void receive_tracker_state(int sock) {
    try {
        // Read state length first
        uint64_t net_len;
        if (recv(sock, &net_len, sizeof(net_len), MSG_WAITALL) != sizeof(net_len)) {
            throw runtime_error("Failed to receive state length");
        }
            
        // Convert from network byte order
        uint64_t state_len = be64toh(net_len);
            
        // Sanity check
        if (state_len > 100 * 1024 * 1024) {  // 100MB max
            throw runtime_error("State size too large: " + to_string(state_len));
        }
            
        // Read the actual state
        vector<char> buffer(state_len);
        size_t total_received = 0;
        while (total_received < state_len) {
            ssize_t received = recv(sock, buffer.data() + total_received, 
                                   state_len - total_received, 0);
            if (received <= 0) {
                throw runtime_error("Failed to receive state data");
            }
            total_received += received;
        }
            
        // Apply the received state
        string state_str(buffer.begin(), buffer.end());
        apply_state(state_str);
            
    } catch (const exception& e) {
        cerr << "Error in receive_tracker_state: " << e.what() << endl;
        throw;
    }
}

// Function to handle incoming sync requests
void handle_tracker_sync_request(int sync_sock) {
    if (sync_sock < 0) return;
        
    try {
        // First receive their state
        receive_tracker_state(sync_sock);
            
        // Then send our state
        send_tracker_state(sync_sock);
            
    } catch (const exception& e) {
        cerr << "Error in handle_tracker_sync_request: " << e.what() << endl;
    }
        
    close(sync_sock);
}

bool is_other_tracker_alive(const string& ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }
        
    // Set a short timeout for the connection attempt
    struct timeval tv;
    tv.tv_sec = 2;  // 2 second timeout
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
        
    if (inet_pton(AF_INET, ip.c_str(), &server_addr.sin_addr) <= 0) {
        close(sock);
        return false;
    }
        
    bool connected = (::connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) >= 0);
    close(sock);
    return connected;
}

// Main synchronization function (runs in a separate thread)
void synchronize_with_other_tracker() {
    // Synchronization thread started
    
    while (true) {
        // Check if we should exit
        {
            lock_guard<mutex> lock(sync_mutex);
            if (!sync_thread_run.load()) {
                break;
            }
        }
        
        // Wait for either a sync request or timeout
        {
            unique_lock<mutex> lock(sync_mutex);
            if (!sync_requested) {
                auto status = sync_cv.wait_for(lock, chrono::seconds(sync_config.sync_interval_sec));
                if (status == std::cv_status::timeout) {
                    // Timeout occurred, proceed with sync
                    sync_requested = true;
                }
            }
            
            // If we were woken up by a signal or timeout, sync_requested will be true
            if (sync_requested) {
                sync_requested = false;
            } else {
                // Spurious wakeup, go back to waiting
                continue;
            }
        }
        
        // Skip if we don't have another tracker configured
        if (sync_config.other_tracker_ip.empty() || sync_config.other_tracker_port == 0) {
            this_thread::sleep_for(chrono::seconds(sync_config.sync_interval_sec));
            continue;
        }
        
        // Try to connect to the other tracker
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("Failed to create socket for synchronization");
            continue;
        }
        
        // Set socket options
        if (!set_socket_options(sock)) {
            close(sock);
            continue;
        }
        
        // Connect to the other tracker
        struct sockaddr_in server_addr = {0};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(sync_config.other_tracker_port);
        
        if (inet_pton(AF_INET, sync_config.other_tracker_ip.c_str(), &server_addr.sin_addr) <= 0) {
            perror("Invalid address/Address not supported");
            close(sock);
            continue;
        }
        
        if (::connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            // Connection failed, other tracker might be down
            close(sock);
            continue;
        }
        
        try {
            // Exchange states with the other tracker
            send_tracker_state(sock);
            receive_tracker_state(sock);
            
            // Update the state version
            state_version++;
        } catch (const exception& e) {
            cerr << "Synchronization error: " << e.what() << endl;
        }
        
        close(sock);
    }
    
    // Synchronization thread stopped
}
