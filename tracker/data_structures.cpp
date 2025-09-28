#include "data_structures.h"
#include <pthread.h>
#include <chrono>
using namespace std;
using namespace std::chrono;

// Define all data structures
unordered_map<string, string> user_data;
unordered_map<string, bool> is_logged_in;
vector<string> all_groups;
unordered_map<string, string> group_admin;
unordered_map<string, set<string>> group_members;
// Maps group_id to a map of join_time to user_id (for finding earliest joiner)
unordered_map<string, map<long long, string>> group_join_times;
unordered_map<string, set<string>> pending_requests;
unordered_map<string, pair<int, string>> user_ip_port;

// Define all mutexes
pthread_mutex_t user_data_mutex;
pthread_mutex_t login_mutex;
pthread_mutex_t all_groups_mutex;
pthread_mutex_t group_admin_mutex;
pthread_mutex_t group_members_mutex;
pthread_mutex_t group_join_times_mutex;
pthread_mutex_t pending_requests_mutex;
pthread_mutex_t user_ip_port_mutex;

void initialize_mutexes() {
    pthread_mutex_init(&user_data_mutex, NULL);
    pthread_mutex_init(&login_mutex, NULL);
    pthread_mutex_init(&all_groups_mutex, NULL);
    pthread_mutex_init(&group_admin_mutex, NULL);
    pthread_mutex_init(&group_members_mutex, NULL);
    pthread_mutex_init(&group_join_times_mutex, NULL);
    pthread_mutex_init(&pending_requests_mutex, NULL);
    pthread_mutex_init(&user_ip_port_mutex, NULL);
}

void destroy_mutexes() {
    pthread_mutex_destroy(&user_data_mutex);
    pthread_mutex_destroy(&login_mutex);
    pthread_mutex_destroy(&all_groups_mutex);
    pthread_mutex_destroy(&group_admin_mutex);
    pthread_mutex_destroy(&group_members_mutex);
    pthread_mutex_destroy(&group_join_times_mutex);
    pthread_mutex_destroy(&pending_requests_mutex);
    pthread_mutex_destroy(&user_ip_port_mutex);
}