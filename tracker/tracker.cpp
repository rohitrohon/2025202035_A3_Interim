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
        thread_run = false;
    }
}

void tracker_loop(int tracker_sock) {
    sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (thread_run) {
        int client_sock = accept(tracker_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) {
            // Don't print error during normal shutdown
            if (thread_run) {
                perror("Client connection error");
            }
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

    const char* filepath = "../client/tracker_info.txt";
    string currentLine = (tracker_no == 1) ? firstLine : secondLine;
    string otherLine = (tracker_no == 1) ? secondLine : firstLine;
    
    // Extract IP and port from the current line
    size_t i = currentLine.find(':');
    if (i != string::npos) {
        ip_address = currentLine.substr(0, i);
        port = stoi(currentLine.substr(i + 1));
    }

    // Read existing content if file exists
    vector<string> existingLines;
    ifstream infile(filepath);
    if (infile) {
        string line;
        while (getline(infile, line)) {
            if (!line.empty()) {
                // Skip if this is the same as the other tracker's line to avoid duplicates
                if (line != otherLine) {
                    existingLines.push_back(line);
                }
            }
        }
        infile.close();
    }
    
    // Open file for writing (this will truncate the file)
    ofstream outfile(filepath, ios::out | ios::trunc);
    if (!outfile) {
        cerr << "Failed to open file for writing: " << filepath << endl;
        return false;
    }
    
    // Write the current tracker's info first
    outfile << currentLine << "\n";
    
    // Then write any existing lines that aren't from the other tracker
    for (const auto& line : existingLines) {
        outfile << line << "\n";
    }
    
    // If this is the first tracker starting, also write the other tracker's info
    if (existingLines.empty() && !otherLine.empty()) {
        outfile << otherLine << "\n";
    }
    
    outfile.close();

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

    // Removed debug output

    int tracker_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tracker_sock == -1) {
        cout << "Failed to create socket." << endl;
        return 1;
    }

    // Set SO_REUSEADDR to allow quick reuse of the port after restart
    int yes = 1;
    if (setsockopt(tracker_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        perror("Warning: setsockopt(SO_REUSEADDR) failed");
        // Not fatal, continue
    }
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
    // Removed other tracker output

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
    
    // Close the server socket to unblock accept()
    if (tracker_sock >= 0) {
        shutdown(tracker_sock, SHUT_RDWR);
        close(tracker_sock);
    }
    
    // Signal the tracker thread to stop
    thread_run = false;
    
    // Wait for the tracker thread to finish
    if (tracker_thread.joinable()) {
        tracker_thread.join();
    }
    
    // Perform cleanup
    cleanup_tracker();
    
    cout << "Tracker stopped" << endl;
    return 0;
}