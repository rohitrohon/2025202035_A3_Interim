#ifndef FILE_SHARE_MANAGER_H
#define FILE_SHARE_MANAGER_H

#include <string>
#include <cstdint>  // for uint64_t
#include <openssl/sha.h>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional> // for std::function

// Progress callback type definition
using ProgressCallback = std::function<void(int percent, const std::string& status)>;

#include "file_operations.h"

// --------------------------------------------
// File Operation Declarations
// --------------------------------------------
std::string calculate_sha1(const std::string& data);
std::string calculate_file_hash(const std::string& file_path);
bool verify_chunk(const std::string& chunk_path, const std::string& expected_hash);
FileMetadata split_file_into_chunks(const std::string& file_path);
bool combine_chunks(const std::string& output_path, const std::string& temp_dir, int total_chunks);
bool verify_file_integrity(const std::string& file_path, const std::string& expected_hash);
bool save_metadata(const FileMetadata& metadata, const std::string& output_path);
FileMetadata load_metadata(const std::string& metadata_path);
std::string get_chunk_path(const std::string& base_dir, const std::string& file_name, int chunk_number);
std::string get_file_name_from_path(const std::string& file_path);

// --------------------------------------------
// File Sharing Management
// --------------------------------------------
class FileShareManager {
public:
    // Constructor
    FileShareManager(const std::string& client_id, 
                    const std::string& tracker_ip, 
                    int tracker_port);
                    
    // Destructor
    ~FileShareManager();

    // File sharing operations
    bool upload_file(const std::string& group_id, const std::string& file_path, ProgressCallback callback = nullptr);
    std::vector<std::string> list_files(const std::string& group_id) const;
    bool download_file(const std::string& group_id, const std::string& file_name, 
                      const std::string& dest_path);
    void show_downloads() const;
    bool stop_share(const std::string& group_id, const std::string& file_name);

    // Allow caller to provide an already-open control socket (e.g., interactive client)
    void set_control_socket(int sock);
    // Set client id (user id) for messages that require authentication
    void set_client_id(const std::string& id);
    // Announce that this client now has a file (after download) so tracker can add it as a peer
    bool announce_share(const std::string& group_id, const std::string& file_hash, const std::string& file_path) const;

    // Getters
    const FileMetadata* get_file_metadata(const std::string& file_name) const;
    bool is_file_shared(const std::string& group_id, const std::string& file_name) const;

private:
    std::string client_id;
    std::string tracker_ip;
    int tracker_port;
    
    // Map of group_id to set of file names being shared
    std::unordered_map<std::string, std::unordered_set<std::string>> shared_files;
    
    // Map of file_name to its metadata
    std::unordered_map<std::string, FileMetadata> file_metadata;
    
    // Map of download_id to download info
    struct DownloadInfo {
        std::string file_name;
        std::string dest_path;
        std::string group_id;
        int total_chunks;
        int downloaded_chunks;
        std::chrono::system_clock::time_point start_time;
    };
    std::unordered_map<std::string, DownloadInfo> active_downloads;
    // Completed downloads history
    std::unordered_map<std::string, DownloadInfo> completed_downloads;
    
    // Thread safety
    mutable std::mutex shared_files_mutex;
    mutable std::mutex downloads_mutex;
    std::condition_variable cv_downloads;
    
    // Network settings
    static const int MAX_RETRIES = 3;
    static const int SOCKET_TIMEOUT_SEC = 10;
    
    // Network helpers
    int connect_to_tracker() const;
    // Get a tracker socket; out_owned is true if caller must close the socket
    int connect_to_tracker(bool &out_owned) const;
    bool send_with_retry(int sock, const void* buf, size_t len, int flags = 0) const;
    bool recv_with_retry(int sock, void* buf, size_t len, int flags = 0) const;
    bool send_to_tracker(const std::string& message) const;
    bool register_share_with_tracker(const std::string& group_id, const std::string& file_name);
    bool unregister_share_with_tracker(const std::string& group_id, const std::string& file_name);
    
    // File upload helpers
    bool register_file_with_tracker(const std::string& group_id, const std::string& file_path, std::string& temp_file_id);
    bool update_file_metadata(const std::string& temp_file_id, const FileMetadata& metadata);
    bool send_chunks_to_peers(const std::string& file_hash, const std::string& file_path, int total_chunks);
    bool send_message(int sock, const std::string& message) const;
    std::string receive_message(int sock) const;
};

#endif // FILE_SHARE_MANAGER_H
