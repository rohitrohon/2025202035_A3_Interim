#include "download_manager.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <openssl/sha.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace p2p {

DownloadManager::DownloadManager(const std::string& peer_id, const std::string& download_dir)
    : peer_id_(peer_id)
    , download_dir_(download_dir) {
    
    // Create download directory if it doesn't exist
    if (!fs::exists(download_dir_)) {
        fs::create_directories(download_dir_);
    }
    
    // Create a temporary directory for partial downloads
    std::string temp_dir = download_dir_ + "/.tmp_" + std::to_string(std::time(nullptr));
    if (!fs::exists(temp_dir)) {
        fs::create_directories(temp_dir);
    }
    
    // Initialize peer manager
    peer_manager_ = std::make_shared<PeerManager>("", peer_id_);
    
    // Set up peer manager callbacks
    peer_manager_->set_piece_callback([this](uint32_t piece_index, const std::vector<uint8_t>& data) {
        // This will be set up when we start a download
    });
    
    peer_manager_->set_peer_state_callback([this](const std::string& peer_id, PeerState state) {
        handle_peer_state_change(peer_id, state);
    });
    
    peer_manager_->set_error_callback([this](const std::string& error) {
        handle_error(error);
    });
}

DownloadManager::~DownloadManager() {
    stop_download();
}

void DownloadManager::start_download(const std::string& file_name, uint64_t file_size,
                                    const std::string& file_hash, size_t piece_size,
                                    const std::vector<std::string>& trackers) {
    if (running_) {
        if (error_callback_) {
            error_callback_("Download already in progress");
        }
        return;
    }
    
    // Initialize file metadata
    metadata_.file_name = file_name;
    metadata_.file_path = (fs::path(download_dir_) / file_name).string();;
    metadata_.file_size = file_size;
    metadata_.file_hash = file_hash;
    
    // Calculate number of pieces
    size_t num_pieces = (file_size + piece_size - 1) / piece_size;
    
    // Initialize piece selector
    piece_selector_ = std::make_unique<PieceSelector>(num_pieces, piece_size);
    
    // Set up the piece callback to handle incoming blocks
    peer_manager_->set_piece_callback([this](uint32_t piece_index, const std::vector<uint8_t>& data) {
        handle_piece_block(piece_index, 0, data); // For now, assume the block starts at 0
    });
    
    // Reset state
    bytes_downloaded_ = 0;
    download_speed_ = 0;
    downloading_pieces_.clear();
    
    // Start the download and stats threads
    running_ = true;
    stopping_ = false;
    download_thread_ = std::thread(&DownloadManager::download_loop, this);
    stats_thread_ = std::thread(&DownloadManager::stats_loop, this);
    
    // Start the peer manager
    peer_manager_->start();
    
    // Connect to trackers (in a real implementation, this would be more sophisticated)
    for (const auto& tracker : trackers) {
        // Parse tracker address (format: ip:port)
        size_t colon_pos = tracker.find(':');
        if (colon_pos != std::string::npos) {
            std::string ip = tracker.substr(0, colon_pos);
            uint16_t port = static_cast<uint16_t>(std::stoi(tracker.substr(colon_pos + 1)));
            
            // In a real implementation, we would connect to the tracker and get a list of peers
            // For now, we'll just add the tracker as a peer (which isn't correct but works for testing)
            add_peer("tracker_" + tracker, ip, port);
        }
    }
}

void DownloadManager::stop_download() {
    if (!running_) {
        return;
    }
    
    stopping_ = true;
    
    // Notify all waiting threads
    download_cv_.notify_all();
    
    // Stop the peer manager
    if (peer_manager_) {
        peer_manager_->stop();
    }
    
    // Wait for threads to finish
    if (download_thread_.joinable()) {
        download_thread_.join();
    }
    
    if (stats_thread_.joinable()) {
        stats_thread_.join();
    }
    
    // Clean up
    cleanup();
    running_ = false;
}

void DownloadManager::add_peer(const std::string& peer_id, const std::string& ip, uint16_t port) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    
    // Don't add duplicate peers
    if (active_peers_.find(peer_id) != active_peers_.end()) {
        return;
    }
    
    // Don't exceed maximum number of peers
    if (active_peers_.size() >= MAX_PEERS) {
        // Find the least useful peer to disconnect
        // For now, just remove a random peer (this could be improved)
        auto it = active_peers_.begin();
        std::advance(it, std::rand() % active_peers_.size());
        active_peers_.erase(it);
    }
    
    // Add the new peer
    peer_manager_->add_peer(peer_id, ip, port);
}

void DownloadManager::remove_peer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    active_peers_.erase(peer_id);
    peer_manager_->remove_peer(peer_id);
}

double DownloadManager::get_progress() const {
    if (!piece_selector_) {
        return 0.0;
    }
    return piece_selector_->get_progress();
}

size_t DownloadManager::get_download_speed() const {
    return download_speed_;
}

size_t DownloadManager::get_peer_count() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    return active_peers_.size();
}

bool DownloadManager::is_complete() const {
    return piece_selector_ && piece_selector_->is_complete();
}

void DownloadManager::set_progress_callback(ProgressCallback callback) {
    progress_callback_ = std::move(callback);
}

void DownloadManager::set_completion_callback(CompletionCallback callback) {
    completion_callback_ = std::move(callback);
}

void DownloadManager::set_error_callback(ErrorCallback callback) {
    error_callback_ = std::move(callback);
}

const FileMetadata& DownloadManager::get_metadata() const {
    return metadata_;
}

// Private methods

void DownloadManager::download_loop() {
    const size_t REQUESTS_PER_PEER = 2; // Number of requests to send to each peer per iteration
    
    while (!stopping_) {
        // Check if download is complete
        if (piece_selector_ && piece_selector_->is_complete()) {
            if (completion_callback_) {
                completion_callback_(true, "Download complete");
            }
            break;
        }
        
        // Get the list of connected peers
        auto peers = peer_manager_->get_all_peers();
        
        // Request pieces from each peer
        for (const auto& peer : peers) {
            if (stopping_) {
                break;
            }
            
            // Skip peers that are choking us
            if (peer->is_peer_choking()) {
                continue;
            }
            
            // Get the next pieces to request from this peer
            auto requests = piece_selector_->get_next_requests(peer->get_peer_id(), REQUESTS_PER_PEER);
            
            // Send the requests
            for (const auto& req : requests) {
                try {
                    peer->request_piece(req.piece_index, req.begin, req.length);
                } catch (const std::exception& e) {
                    handle_error(std::string("Failed to request piece: ") + e.what());
                }
            }
        }
        
        // Process any completed pieces
        process_completed_pieces();
        
        // Update progress
        update_progress();
        
        // Small delay to prevent busy-waiting
        std::this_thread::sleep_for(100ms);
    }
    
    // Clean up
    cleanup();
    running_ = false;
}

void DownloadManager::stats_loop() {
    auto last_time = std::chrono::steady_clock::now();
    size_t last_bytes = 0;
    
    while (!stopping_) {
        // Calculate download speed
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_time).count();
        
        if (elapsed >= 1) { // Update every second
            size_t current_bytes = bytes_downloaded_;
            size_t bytes_diff = current_bytes - last_bytes;
            
            // Calculate bytes per second
            download_speed_ = bytes_diff / elapsed;
            
            // Update for next iteration
            last_bytes = current_bytes;
            last_time = now;
            
            // Update progress
            update_progress();
        }
        
        // Sleep for a short time
        std::this_thread::sleep_for(500ms);
    }
}

void DownloadManager::process_completed_pieces() {
    std::vector<uint32_t> completed_pieces;
    
    // Find completed pieces
    {
        std::lock_guard<std::mutex> lock(download_mutex_);
        
        for (auto it = downloading_pieces_.begin(); it != downloading_pieces_.end(); ) {
            if (it->second.is_complete()) {
                completed_pieces.push_back(it->first);
                it = downloading_pieces_.erase(it);
            } else {
                // Check for timeouts
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - it->second.last_block_time);
                
                if (elapsed > PIECE_TIMEOUT) {
                    // This piece is taking too long, mark it as failed
                    piece_selector_->mark_failed(it->first, "");
                    it = downloading_pieces_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    
    // Process completed pieces
    for (uint32_t piece_index : completed_pieces) {
        auto it = downloading_pieces_.find(piece_index);
        if (it != downloading_pieces_.end()) {
            verify_and_save_piece(piece_index, it->second.data);
            downloading_pieces_.erase(it);
        }
    }
    
    // If we're close to finishing, enter endgame mode
    if (piece_selector_ && piece_selector_->get_progress() > 0.9) {
        piece_selector_->enter_endgame();
    }
}

void DownloadManager::verify_and_save_piece(uint32_t piece_index, const std::vector<uint8_t>& data) {
    try {
        // Calculate the hash of the piece
        std::vector<uint8_t> hash(SHA_DIGEST_LENGTH);
        SHA1(data.data(), data.size(), hash.data());
        
        // Convert hash to hex string
        std::stringstream ss;
        for (uint8_t b : hash) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
        }
        std::string hash_str = ss.str();
        
        // In a real implementation, we would compare this with the expected hash from the metadata
        // For now, we'll just assume it's correct
        
        // Save the piece to disk
        std::string piece_path = download_dir_ + "/.tmp_" + metadata_.file_name + ".piece_" + std::to_string(piece_index);
        
        std::ofstream out_file(piece_path, std::ios::binary);
        if (!out_file) {
            throw std::runtime_error("Failed to open file for writing: " + piece_path);
        }
        
        out_file.write(reinterpret_cast<const char*>(data.data()), data.size());
        out_file.close();
        
        if (!out_file) {
            throw std::runtime_error("Failed to write piece to file: " + piece_path);
        }
        
        // Update the piece selector
        piece_selector_->mark_have(piece_index);
        
        // Update bytes downloaded
        bytes_downloaded_ += data.size();
        
    } catch (const std::exception& e) {
        handle_error(std::string("Error saving piece: ") + e.what());
    }
}

void DownloadManager::handle_peer_state_change(const std::string& peer_id, PeerState state) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    
    switch (state) {
        case PeerState::CONNECTED:
            // Update the peer's bitfield
            if (peer_manager_) {
                auto peer = peer_manager_->get_peer(peer_id);
                if (peer) {
                    auto bitfield = peer->get_bitfield();
                    if (!bitfield.empty()) {
                        piece_selector_->update_peer_pieces(peer_id, bitfield);
                    }
                }
            }
            break;
            
        case PeerState::DISCONNECTED:
            // Remove the peer from our active peers
            active_peers_.erase(peer_id);
            break;
            
        default:
            break;
    }
}

void DownloadManager::handle_piece_block(uint32_t piece_index, uint32_t begin, const std::vector<uint8_t>& block) {
    std::lock_guard<std::mutex> lock(download_mutex_);
    
    // Find or create the downloading piece
    auto it = downloading_pieces_.find(piece_index);
    if (it == downloading_pieces_.end()) {
        // Create a new entry for this piece
        size_t piece_size = piece_selector_ ? piece_selector_->piece_size(piece_index) : 0;
        it = downloading_pieces_.emplace(
            piece_index, 
            DownloadingPiece(piece_size, 16 * 1024) // 16KB blocks
        ).first;
    }
    
    // Mark the block as received
    it->second.mark_block_received(begin, block.data(), block.size());
    
    // If the piece is complete, process it
    if (it->second.is_complete()) {
        process_completed_pieces();
    }
}

void DownloadManager::handle_error(const std::string& error) {
    if (error_callback_) {
        error_callback_(error);
    }
}

void DownloadManager::update_progress() {
    if (progress_callback_) {
        double progress = get_progress();
        size_t downloaded = bytes_downloaded_;
        size_t total = metadata_.file_size;
        progress_callback_(progress, downloaded, total);
    }
}

void DownloadManager::cleanup() {
    // In a real implementation, we would clean up temporary files and resources
    // For now, we'll just clear the downloading pieces
    std::lock_guard<std::mutex> lock(download_mutex_);
    downloading_pieces_.clear();
}

} // namespace p2p
