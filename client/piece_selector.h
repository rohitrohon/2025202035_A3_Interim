#ifndef PIECE_SELECTOR_H
#define PIECE_SELECTOR_H

#include <vector>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <random>
#include <chrono>
#include <algorithm>
#include <stdexcept>
#include "p2p_protocol.h"

namespace p2p {

class PieceSelector {
public:
    // Represents a piece request
    struct PieceRequest {
        uint32_t piece_index;
        uint32_t begin;
        uint32_t length;
        std::string peer_id;
        
        bool operator<(const PieceRequest& other) const {
            if (piece_index != other.piece_index) return piece_index < other.piece_index;
            if (begin != other.begin) return begin < other.begin;
            return length < other.length;
        }
    };
    
    // Represents a piece that we have or are downloading
    struct PieceInfo {
        bool have = false;                    // Do we have this piece?
        bool requested = false;               // Is this piece currently being downloaded?
        std::set<std::string> peers_have;     // Which peers have this piece?
        std::set<std::string> peers_requested_from; // Which peers have we requested this from?
        std::chrono::steady_clock::time_point last_attempt; // When did we last try to download this?
    };
    
    // Constructor
    PieceSelector(size_t num_pieces, size_t piece_size, size_t max_concurrent_requests = 5);
    
    // Update the availability of a piece from a peer
    void update_peer_pieces(const std::string& peer_id, const std::vector<bool>& bitfield);
    
    // Mark a piece as being requested from a peer
    void mark_requested(uint32_t piece_index, uint32_t begin, uint32_t length, const std::string& peer_id);
    
    // Mark a piece as received
    void mark_have(uint32_t piece_index);
    
    // Mark a piece as failed (so we can try again later)
    void mark_failed(uint32_t piece_index, const std::string& peer_id);
    
    // Get the next piece to download from a peer
    std::vector<PieceRequest> get_next_requests(const std::string& peer_id, size_t max_requests = 1);
    
    // Check if we have all pieces
    bool is_complete() const;
    
    // Get the current progress (0.0 to 1.0)
    double get_progress() const;
    
    // Get the number of pieces we have
    size_t num_pieces_have() const;
    
    // Get the total number of pieces
    size_t num_pieces() const;
    
    // Get the size of a piece
    size_t piece_size() const;
    
    // Get the size of a specific piece (last piece may be smaller)
    size_t piece_size(uint32_t piece_index) const;
    
    // Get the bitfield of pieces we have
    std::vector<bool> get_bitfield() const;
    
    // Enter endgame mode (request pieces from all peers to finish faster)
    void enter_endgame();
    
    // Check if we're in endgame mode
    bool is_endgame() const;
    
private:
    // Get the rarest pieces that a peer has that we don't have and haven't requested
    std::vector<uint32_t> get_rarest_pieces(const std::string& peer_id) const;
    
    // Get the count of how many peers have a piece
    size_t get_piece_rarity(uint32_t piece_index) const;
    
    // Check if we can request a piece from a peer
    bool can_request_from_peer(uint32_t piece_index, const std::string& peer_id) const;
    
    // Get the next block to request for a piece
    std::vector<PieceRequest> get_block_requests(uint32_t piece_index, const std::string& peer_id, size_t max_requests) const;
    
    // Constants
    const size_t num_pieces_;
    const size_t piece_size_;
    const size_t block_size_ = 16 * 1024; // 16KB blocks
    const size_t max_concurrent_requests_;
    
    // State
    mutable std::mutex mutex_;
    std::vector<PieceInfo> pieces_;
    std::map<std::string, std::vector<bool>> peer_bitfields_;
    size_t num_have_ = 0;
    bool endgame_ = false;
    
    // Random number generator for tie-breaking
    mutable std::mt19937 rng_{static_cast<unsigned int>(std::chrono::system_clock::now().time_since_epoch().count())};
};

} // namespace p2p

#endif // PIECE_SELECTOR_H
