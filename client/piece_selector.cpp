#include "piece_selector.h"
#include <algorithm>
#include <random>
#include <limits>

namespace p2p {

PieceSelector::PieceSelector(size_t num_pieces, size_t piece_size, size_t max_concurrent_requests)
    : num_pieces_(num_pieces)
    , piece_size_(piece_size)
    , max_concurrent_requests_(max_concurrent_requests)
    , pieces_(num_pieces) {
    
    if (num_pieces == 0) {
        throw std::invalid_argument("Number of pieces must be greater than 0");
    }
    
    if (piece_size == 0) {
        throw std::invalid_argument("Piece size must be greater than 0");
    }
    
    if (max_concurrent_requests == 0) {
        throw std::invalid_argument("Max concurrent requests must be greater than 0");
    }
}

void PieceSelector::update_peer_pieces(const std::string& peer_id, const std::vector<bool>& bitfield) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Store the peer's bitfield
    peer_bitfields_[peer_id] = bitfield;
    
    // Update which pieces this peer has
    for (size_t i = 0; i < std::min(bitfield.size(), num_pieces_); ++i) {
        if (bitfield[i]) {
            pieces_[i].peers_have.insert(peer_id);
        } else {
            pieces_[i].peers_have.erase(peer_id);
            pieces_[i].peers_requested_from.erase(peer_id);
        }
    }
}

void PieceSelector::mark_requested(uint32_t piece_index, uint32_t begin, uint32_t length, 
                                 const std::string& peer_id) {
    if (piece_index >= num_pieces_) {
        throw std::out_of_range("Piece index out of range");
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto& piece = pieces_[piece_index];
    if (piece.have) {
        return; // Already have this piece
    }
    
    piece.requested = true;
    piece.peers_requested_from.insert(peer_id);
    piece.last_attempt = std::chrono::steady_clock::now();
}

void PieceSelector::mark_have(uint32_t piece_index) {
    if (piece_index >= num_pieces_) {
        throw std::out_of_range("Piece index out of range");
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto& piece = pieces_[piece_index];
    if (!piece.have) {
        piece.have = true;
        piece.requested = false;
        piece.peers_requested_from.clear();
        num_have_++;
    }
}

void PieceSelector::mark_failed(uint32_t piece_index, const std::string& peer_id) {
    if (piece_index >= num_pieces_) {
        throw std::out_of_range("Piece index out of range");
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto& piece = pieces_[piece_index];
    piece.peers_requested_from.erase(peer_id);
    
    // If no more peers have this piece, mark it as not requested
    // so we can try again later if a new peer appears
    if (piece.peers_requested_from.empty()) {
        piece.requested = false;
    }
}

std::vector<PieceSelector::PieceRequest> PieceSelector::get_next_requests(
    const std::string& peer_id, size_t max_requests) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Check if we already have all pieces
    if (num_have_ >= num_pieces_) {
        return {};
    }
    
    // Get the peer's bitfield
    auto peer_it = peer_bitfields_.find(peer_id);
    if (peer_it == peer_bitfields_.end() || peer_it->second.empty()) {
        return {}; // No bitfield for this peer yet
    }
    
    const auto& peer_bitfield = peer_it->second;
    std::vector<PieceRequest> requests;
    
    // In endgame mode, request from all peers that have the piece
    if (endgame_) {
        for (uint32_t i = 0; i < num_pieces_ && requests.size() < max_requests; ++i) {
            if (!pieces_[i].have && !pieces_[i].requested && 
                i < peer_bitfield.size() && peer_bitfield[i] &&
                can_request_from_peer(i, peer_id)) {
                
                auto block_requests = get_block_requests(i, peer_id, max_requests - requests.size());
                requests.insert(requests.end(), block_requests.begin(), block_requests.end());
                
                if (!block_requests.empty()) {
                    mark_requested(i, block_requests[0].begin, block_requests[0].length, peer_id);
                }
            }
        }
        return requests;
    }
    
    // In normal mode, use rarest-first strategy
    auto rarest_pieces = get_rarest_pieces(peer_id);
    
    for (uint32_t piece_index : rarest_pieces) {
        if (requests.size() >= max_requests) {
            break;
        }
        
        auto block_requests = get_block_requests(piece_index, peer_id, max_requests - requests.size());
        if (!block_requests.empty()) {
            requests.insert(requests.end(), block_requests.begin(), block_requests.end());
            mark_requested(piece_index, block_requests[0].begin, block_requests[0].length, peer_id);
        }
    }
    
    return requests;
}

bool PieceSelector::is_complete() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return num_have_ >= num_pieces_;
}

double PieceSelector::get_progress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<double>(num_have_) / num_pieces_;
}

size_t PieceSelector::num_pieces_have() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return num_have_;
}

size_t PieceSelector::num_pieces() const {
    return num_pieces_;
}

size_t PieceSelector::piece_size() const {
    return piece_size_;
}

size_t PieceSelector::piece_size(uint32_t piece_index) const {
    if (piece_index >= num_pieces_) {
        throw std::out_of_range("Piece index out of range");
    }
    
    // Last piece may be smaller
    if (piece_index == num_pieces_ - 1) {
        size_t total_size = num_pieces_ * piece_size_;
        size_t last_piece_size = total_size % piece_size_;
        return last_piece_size == 0 ? piece_size_ : last_piece_size;
    }
    
    return piece_size_;
}

std::vector<bool> PieceSelector::get_bitfield() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<bool> bitfield(num_pieces_);
    for (size_t i = 0; i < num_pieces_; ++i) {
        bitfield[i] = pieces_[i].have;
    }
    
    return bitfield;
}

void PieceSelector::enter_endgame() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Only enter endgame if we're close to finishing
    if (num_have_ >= num_pieces_ * 0.9) {
        endgame_ = true;
        
        // Reset requested state for all pieces we don't have
        for (auto& piece : pieces_) {
            if (!piece.have) {
                piece.requested = false;
                piece.peers_requested_from.clear();
            }
        }
    }
}

bool PieceSelector::is_endgame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return endgame_;
}

// Private methods

std::vector<uint32_t> PieceSelector::get_rarest_pieces(const std::string& peer_id) const {
    std::vector<std::pair<uint32_t, size_t>> piece_rarities;
    const auto& peer_bitfield = peer_bitfields_.at(peer_id);
    
    // Find all pieces that the peer has and we don't
    for (uint32_t i = 0; i < num_pieces_; ++i) {
        if (i < peer_bitfield.size() && peer_bitfield[i] && !pieces_[i].have && 
            !pieces_[i].requested && can_request_from_peer(i, peer_id)) {
            
            size_t rarity = get_piece_rarity(i);
            piece_rarities.emplace_back(i, rarity);
        }
    }
    
    // Sort by rarity (rarest first)
    std::sort(piece_rarities.begin(), piece_rarities.end(), 
        [](const auto& a, const auto& b) {
            if (a.second != b.second) {
                return a.second < b.second; // Lower count is rarer
            }
            return a.first < b.first; // For ties, use piece index
        });
    
    // Extract just the piece indices
    std::vector<uint32_t> result;
    result.reserve(piece_rarities.size());
    for (const auto& pair : piece_rarities) {
        result.push_back(pair.first);
    }
    
    return result;
}

size_t PieceSelector::get_piece_rarity(uint32_t piece_index) const {
    if (piece_index >= num_pieces_) {
        return std::numeric_limits<size_t>::max();
    }
    
    return pieces_[piece_index].peers_have.size();
}

bool PieceSelector::can_request_from_peer(uint32_t piece_index, const std::string& peer_id) const {
    if (piece_index >= num_pieces_) {
        return false;
    }
    
    const auto& piece = pieces_[piece_index];
    
    // Don't request if we already have it
    if (piece.have) {
        return false;
    }
    
    // In endgame mode, request from any peer that has it
    if (endgame_) {
        return piece.peers_have.find(peer_id) != piece.peers_have.end();
    }
    
    // In normal mode, only request if we haven't requested it from too many peers
    return piece.peers_requested_from.size() < max_concurrent_requests_ &&
           piece.peers_requested_from.find(peer_id) == piece.peers_requested_from.end() &&
           piece.peers_have.find(peer_id) != piece.peers_have.end();
}

std::vector<PieceSelector::PieceRequest> PieceSelector::get_block_requests(
    uint32_t piece_index, const std::string& peer_id, size_t max_requests) const {
    
    if (piece_index >= num_pieces_ || max_requests == 0) {
        return {};
    }
    
    const size_t psize = piece_size(piece_index);
    const size_t num_blocks = (psize + block_size_ - 1) / block_size_;
    
    // Try to find blocks that haven't been requested yet
    std::vector<PieceRequest> requests;
    
    // Simple strategy: request blocks in order
    for (size_t block = 0; block < num_blocks && requests.size() < max_requests; ++block) {
        uint32_t begin = block * block_size_;
        uint32_t length = static_cast<uint32_t>(std::min(block_size_, psize - begin));
        
        // Check if this block is already being downloaded
        bool already_requested = false;
        for (const auto& peer : pieces_[piece_index].peers_requested_from) {
            // For simplicity, assume that if we've requested any part of the piece from a peer,
            // we've requested all blocks. This could be made more precise by tracking individual blocks.
            already_requested = true;
            break;
        }
        
        if (!already_requested) {
            requests.push_back({piece_index, begin, length, peer_id});
        }
    }
    
    return requests;
}

} // namespace p2p
