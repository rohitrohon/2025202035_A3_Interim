#include "tracker.h"
#include "client_handler.h"
#include "synchronization.h"
#include "data_structures.h"
#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <sstream>
#include <thread>
#include <vector>
#include <unordered_map>
#include <set>
#include <csignal>
#include <fstream>

using namespace std;

// Global flag to control the tracker loop
atomic<bool> thread_run{true};

// Signal handler for clean shutdown
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        cout << "\nShutting down tracker..." << endl;
        thread_run = false;
    }
}

void tracker_loop(int tracker_sock) {
    sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (thread_run) {
        int client_sock = accept(tracker_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) {
            cout << "Failed to accept client connection." << endl;
            continue;
        }

        thread client_thread(handle_client, client_sock, client_addr);
        client_thread.detach();
    }
    close(tracker_sock);
}

void shutdown_tracker() {
    thread_run = false;
}

bool initialize_tracker(const string& tracker_file, int tracker_no, string& ip_address, int& port) {
    int fd = open(tracker_file.c_str(), O_RDONLY);
    if (fd < 0) {
        cerr << "Failed to open " << tracker_file << endl;
        return false;
    }

    const int BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];
    ssize_t bytesRead = read(fd, buffer, BUFFER_SIZE - 1);
    if (bytesRead < 0) {
        cerr << "Failed to read from file." << endl;
        close(fd);
        return false;
    }

    buffer[bytesRead] = '\0';
    close(fd);

    stringstream ss(buffer);
    string firstLine, secondLine;
    getline(ss, firstLine);
    getline(ss, secondLine);

    if (tracker_no == 1) {
        size_t i = firstLine.find(':');
        if (i != string::npos) {
            ip_address = firstLine.substr(0, i);
            port = stoi(firstLine.substr(i + 1));
        }

        const char* filepath = "../client/tracker_info.txt";
        fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (fd < 0) {
            cerr << "Failed to open file for writing: " << filepath << endl;
            return false;
        }
        write(fd, firstLine.c_str(), firstLine.size());
        close(fd);

    } else if (tracker_no == 2) {
        size_t i = secondLine.find(':');
        if (i != string::npos) {
            ip_address = secondLine.substr(0, i);
            port = stoi(secondLine.substr(i + 1));
        }

        const char* filepath = "../client/tracker_info.txt";
        fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (fd < 0) {
            cerr << "Failed to open file for writing: " << filepath << endl;
            return false;
        }
        write(fd, secondLine.c_str(), secondLine.size());
        close(fd);
    }

    return !ip_address.empty();
}

void cleanup_tracker() {
    // Shutdown synchronization system
    shutdown_sync_system();
    
    // Destroy mutexes
    destroy_mutexes();
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        cout << "Invalid Arguments Format: ./tracker tracker_info.txt tracker_no" << endl;
        return 1;
    }

    string tracker_file = argv[1];
    int tracker_no = atoi(argv[2]);

    if (tracker_no != 1 && tracker_no != 2) {
        cerr << "Tracker number must be either 1 or 2" << endl;
        return 1;
    }

    string ip_address;
    int port;
    
    // Initialize tracker configuration
    if (!initialize_tracker(tracker_file, tracker_no, ip_address, port)) {
        cerr << "Failed to initialize tracker configuration" << endl;
        return 1;
    }

    if (ip_address.empty()) {
        cout << "Tracker IP Address not found in the tracker info file." << endl;
        return 1;
    }

    cout << "Using IP Address: " << ip_address << " and Port: " << port << endl;

    int tracker_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tracker_sock == -1) {
        cout << "Failed to create socket." << endl;
        return 1;
    }

    int no = 1;
    // Set SO_REUSEADDR
    if (setsockopt(tracker_sock, SOL_SOCKET, SO_REUSEADDR, &no, sizeof(no)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        // Not fatal, continue
    }
    // Set SO_REUSEPORT (may not be available or behave differently across OS)
#ifdef SO_REUSEPORT
    if (setsockopt(tracker_sock, SOL_SOCKET, SO_REUSEPORT, &no, sizeof(no)) < 0) {
        perror("setsockopt SO_REUSEPORT");
        // Not fatal, continue
    }
#endif
    sockaddr_in tracker_addr;
    tracker_addr.sin_family = AF_INET;
    tracker_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip_address.c_str(), &tracker_addr.sin_addr) <= 0) {
        cout << "Invalid IP address." << endl;
        return 1;
    }

    // Bind the socket to the specified port
    if (::bind(tracker_sock, (struct sockaddr*)&tracker_addr, sizeof(tracker_addr)) < 0) {
        perror("bind failed");
        close(tracker_sock);
        return -1;
    }

    if (listen(tracker_sock, 5) < 0) {
        cout << "Failed to listen on socket." << endl;
        return 1;
    }

    cout << "Tracker is listening on IP Address: " << ip_address << " and Port NO: " << port << endl;

    // Initialize data structures and mutexes
    initialize_mutexes();
    
    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize synchronization system
    TrackerSyncConfig sync_config;
    
    // For testing: if this is tracker 1, it will sync with tracker 2 and vice versa
    if (tracker_no == 1) {
        sync_config.other_tracker_ip = "127.0.0.1";
        sync_config.other_tracker_port = 8002;  // Assuming tracker 2 runs on port 8002
        sync_config.sync_port = 8001;           // This tracker's sync port
    } else {
        sync_config.other_tracker_ip = "127.0.0.1";
        sync_config.other_tracker_port = 8001;  // Assuming tracker 1 runs on port 8001
        sync_config.sync_port = 8002;           // This tracker's sync port
    }
    
    sync_config.sync_interval_sec = 5;  // Sync every 5 seconds
    sync_config.sync_timeout_ms = 2000; // 2 second timeout for sync operations
    
    if (!initialize_sync_system(sync_config)) {
        cerr << "Failed to initialize synchronization system" << endl;
        cleanup_tracker();
        return 1;
    }
    
    // Start the sync server
    if (!start_sync_server()) {
        cerr << "Failed to start sync server" << endl;
        cleanup_tracker();
        return 1;
    }
    
    cout << "Tracker " << tracker_no << " started with sync port: " << sync_config.sync_port << endl;
    cout << "Other tracker: " << sync_config.other_tracker_ip << ":" << sync_config.other_tracker_port << endl;

    // Start tracker loop in a separate thread
    thread tracker_thread(tracker_loop, tracker_sock);
    
    // Main thread handles console input
    string input;
    while (thread_run) {
        getline(cin, input);
        if (input == "quit") {
            thread_run = false;
            break;
        } else if (input == "sync") {
            cout << "Forcing sync with other tracker..." << endl;
            force_sync();
        } else if (input == "status") {
            cout << "Current state version: " << get_current_state_version() << endl;
        }
    }
    
    // Cleanup
    cout << "Shutting down tracker..." << endl;
    
    // Close the server socket to unblock accept()
    if (tracker_sock >= 0) {
        shutdown(tracker_sock, SHUT_RDWR);
        close(tracker_sock);
    }
    
    // Wait for tracker thread to finish
    if (tracker_thread.joinable()) {
        tracker_thread.join();
    }
    
    // Perform cleanup
    cleanup_tracker();
    
    cout << "Tracker stopped" << endl;
    return 0;
}