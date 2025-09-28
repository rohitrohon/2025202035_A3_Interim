#include "synchronization.h"
#include <iostream>
#include <csignal>
#include <cstdlib>

using namespace std;

// Global flag for signal handling
volatile sig_atomic_t keep_running = 1;

// Signal handler for clean shutdown
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        cout << "\nShutting down tracker..." << endl;
        keep_running = 0;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        cerr << "Usage: " << argv[0] << " <sync_port> <other_tracker_ip:other_tracker_port>" << endl;
        return 1;
    }

    // Parse command line arguments
    int sync_port = atoi(argv[1]);
    
    string other_tracker = argv[2];
    size_t colon_pos = other_tracker.find(':');
    if (colon_pos == string::npos) {
        cerr << "Invalid format for other tracker. Use IP:PORT format." << endl;
        return 1;
    }
    
    string other_tracker_ip = other_tracker.substr(0, colon_pos);
    int other_tracker_port = stoi(other_tracker.substr(colon_pos + 1));

    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize the sync system
    TrackerSyncConfig config;
    config.other_tracker_ip = other_tracker_ip;
    config.other_tracker_port = other_tracker_port;
    config.sync_port = sync_port;
    config.sync_interval_sec = 5;  // Sync every 5 seconds
    config.sync_timeout_ms = 2000; // 2 second timeout

    if (!initialize_sync_system(config)) {
        cerr << "Failed to initialize sync system" << endl;
        return 1;
    }

    // Start the sync server
    if (!start_sync_server()) {
        cerr << "Failed to start sync server" << endl;
        shutdown_sync_system();
        return 1;
    }

    cout << "Tracker started with sync port: " << sync_port << endl;
    cout << "Other tracker: " << other_tracker_ip << ":" << other_tracker_port << endl;

    // Main loop
    while (keep_running) {
        // Here you would normally handle tracker requests
        // For this example, we'll just sleep
        this_thread::sleep_for(chrono::seconds(1));
        
        // Force a sync every 30 seconds for demonstration
        static int counter = 0;
        if (++counter >= 30) {
            counter = 0;
            cout << "Forcing sync with other tracker..." << endl;
            force_sync();
        }
    }

    // Clean up
    cout << "Shutting down..." << endl;
    shutdown_sync_system();
    
    cout << "Tracker stopped" << endl;
    return 0;
}
