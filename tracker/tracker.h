#ifndef TRACKER_H
#define TRACKER_H

#include <string>
#include <thread>
#include <atomic>

using namespace std;

// Global flag to control the tracker loop
extern atomic<bool> thread_run;

// Tracker main functions
void tracker_loop(int tracker_sock);
void shutdown_tracker();

// Initialize tracker
bool initialize_tracker(const string& tracker_file, int tracker_no, string& ip_address, int& port);

// Cleanup function
void cleanup_tracker();

#endif