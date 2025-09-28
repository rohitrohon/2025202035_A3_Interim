#ifndef DATA_STRUCTURES_H
#define DATA_STRUCTURES_H

#include <string>
#include <unordered_map>
#include <vector>
#include <set>
#include <map>
#include <pthread.h>
#include <chrono>
using namespace std;

// Data structures for tracker state
extern unordered_map<string, string> user_data;
extern unordered_map<string, bool> is_logged_in;
extern vector<string> all_groups;
extern unordered_map<string, string> group_admin;
extern unordered_map<string, set<string>> group_members;
// Maps group_id to a map of user_id to join time (in seconds since epoch)
extern unordered_map<string, map<long long, string>> group_join_times;
extern unordered_map<string, set<string>> pending_requests;
extern unordered_map<string, pair<int, string>> user_ip_port;

// Mutexes for thread safety
extern pthread_mutex_t user_data_mutex;
extern pthread_mutex_t login_mutex;
extern pthread_mutex_t all_groups_mutex;
extern pthread_mutex_t group_admin_mutex;
extern pthread_mutex_t group_members_mutex;
extern pthread_mutex_t group_join_times_mutex;
extern pthread_mutex_t pending_requests_mutex;
extern pthread_mutex_t user_ip_port_mutex;

// Initialize all mutexes
void initialize_mutexes();

// Clean up all mutexes
void destroy_mutexes();

#endif