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

// Structure to store file information
struct FileInfo {
    string file_name;
    string file_path;
    uint64_t file_size;
    string file_hash;
    string owner_id;
    int total_chunks;
    map<int, set<string>> chunk_owners;  // chunk_num -> set of client_ids
    std::vector<std::string> chunk_hashes; // per-chunk SHA1 hashes
};

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

// File sharing data structures
extern unordered_map<string, set<string>> group_files;  // group_id -> set of file_hashes
extern unordered_map<string, FileInfo> file_metadata;  // file_hash -> FileInfo
extern unordered_map<string, set<string>> user_files;  // user_id -> set of file_hashes

// Download tracking per user
struct DownloadEntry {
    string group_id;
    string file_name;
    char status; // 'D' or 'C'
};
extern unordered_map<string, vector<DownloadEntry>> user_downloads; // user_id -> list of downloads

// Mutexes for thread safety
extern pthread_mutex_t user_data_mutex;
extern pthread_mutex_t login_mutex;
extern pthread_mutex_t all_groups_mutex;
extern pthread_mutex_t group_admin_mutex;
extern pthread_mutex_t group_members_mutex;
extern pthread_mutex_t group_join_times_mutex;
extern pthread_mutex_t pending_requests_mutex;
extern pthread_mutex_t user_ip_port_mutex;

// File sharing mutexes
extern pthread_mutex_t group_files_mutex;
extern pthread_mutex_t file_metadata_mutex;
extern pthread_mutex_t user_files_mutex;
extern pthread_mutex_t user_downloads_mutex;

// Temp upload tracking (to avoid leaking temp_* into user/group maps)
extern unordered_map<string, string> temp_to_group; // temp_id -> group_id
extern unordered_map<string, string> temp_to_owner; // temp_id -> user_id
extern pthread_mutex_t temp_maps_mutex;

// Initialize all mutexes
void initialize_mutexes();

// Clean up all mutexes
void destroy_mutexes();

#endif