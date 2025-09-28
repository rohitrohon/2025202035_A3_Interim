#ifndef DOWNLOAD_MANAGER_H
#define DOWNLOAD_MANAGER_H

#include "peer_connection.h"
#include "piece_selector.h"
#include "file_operations.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <map>
#include <set>
#include <functional>
#include <chrono>

namespace p2p {

class DownloadManager : public std::enable_shared_from_this<DownloadManager> {
public:
    // Callback types
    using ProgressCallback = std::function<void(double progress, size_t downloaded, size_t total)>;
    using CompletionCallback = std::function<void(bool success, const std::string& message)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    
    // Constructor
    DownloadManager(const std::string& peer_id, const std::string& download_dir);
    
    // Destructor
    ~DownloadManager();
    
    // Start downloading a file
    void start_download(const std::string& file_name, uint64_t file_size, 
                       const std::string& file_hash, size_t piece_size,
                       const std::vector<std::string>& trackers);
    
    // Stop the download
    void stop_download();
    
    // Add a peer to the download swarm
    void add_peer(const std::string& peer_id, const std::string& ip, uint16_t port);
    
    // Remove a peer from the download swarm
    void remove_peer(const std::string& peer_id);
    
    // Get the current download progress (0.0 to 1.0)
    double get_progress() const;
    
    // Get the current download speed in bytes per second
    size_t get_download_speed() const;
    
    // Get the number of connected peers
    size_t get_peer_count() const;
    
    // Check if the download is complete
    bool is_complete() const;
    
    // Set callbacks
    void set_progress_callback(ProgressCallback callback);
    void set_completion_callback(CompletionCallback callback);
    void set_error_callback(ErrorCallback callback);
    
    // Get the file metadata
    const FileMetadata& get_metadata() const;
    
private:
    // Represents a piece being downloaded
    struct DownloadingPiece {
        std::vector<bool> blocks_received;
        std::vector<uint8_t> data;
        size_t num_blocks = 0;
        size_t blocks_received_count = 0;
        std::chrono::steady_clock::time_point last_block_time;
        
        DownloadingPiece(size_t piece_size, size_t block_size) 
            : data(piece_size) {
            num_blocks = (piece_size + block_size - 1) / block_size;
            blocks_received.resize(num_blocks, false);
        }
        
        bool is_complete() const {
            return blocks_received_count >= num_blocks;
        }
        
        void mark_block_received(size_t block_offset, const uint8_t* block_data, size_t block_size) {
            size_t block_index = block_offset / block_size;
            if (block_index >= blocks_received.size() || blocks_received[block_index]) {
                return; // Already received this block
            }
            
            // Copy the block data
            std::memcpy(data.data() + block_offset, block_data, block_size);
            blocks_received[block_index] = true;
            blocks_received_count++;
            last_block_time = std::chrono::steady_clock::now();
        }
    };
    
    // Constants
    static constexpr size_t MAX_CONCURRENT_REQUESTS = 5;
    static constexpr size_t MAX_PEERS = 50;
    static constexpr std::chrono::seconds PIECE_TIMEOUT{30};
    static constexpr std::chrono::seconds STATS_INTERVAL{1};
    
    // Member variables
    std::string peer_id_;
    std::string download_dir_;
    
    // File information
    FileMetadata metadata_;
    std::unique_ptr<PieceSelector> piece_selector_;
    
    // Download state
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<size_t> bytes_downloaded_{0};
    std::atomic<size_t> download_speed_{0};
    
    // Threads
    std::thread download_thread_;
    std::thread stats_thread_;
    
    // Peer management
    std::shared_ptr<PeerManager> peer_manager_;
    std::map<std::string, std::shared_ptr<PeerConnection>> active_peers_;
    mutable std::mutex peers_mutex_;
    
    // Download state
    std::map<uint32_t, DownloadingPiece> downloading_pieces_;
    mutable std::mutex download_mutex_;
    std::condition_variable download_cv_;
    
    // Callbacks
    ProgressCallback progress_callback_;
    CompletionCallback completion_callback_;
    ErrorCallback error_callback_;
    
    // Private methods
    void download_loop();
    void stats_loop();
    void process_completed_pieces();
    void verify_and_save_piece(uint32_t piece_index, const std::vector<uint8_t>& data);
    void handle_peer_state_change(const std::string& peer_id, PeerState state);
    void handle_piece_block(uint32_t piece_index, uint32_t begin, const std::vector<uint8_t>& block);
    void handle_error(const std::string& error);
    void update_progress();
    void cleanup();
};

} // namespace p2p

#endif // DOWNLOAD_MANAGER_H
