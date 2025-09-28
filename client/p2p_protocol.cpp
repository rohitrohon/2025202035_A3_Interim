#include "p2p_protocol.h"
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <cstring>

namespace p2p {

// Protocol string for handshake
const std::string HandshakeMessage::PROTOCOL_STRING = "BitTorrent protocol";

// HandshakeMessage implementation
std::vector<uint8_t> HandshakeMessage::serialize() const {
    if (info_hash.size() != 20 || peer_id.size() != 20) {
        throw std::runtime_error("Invalid handshake message: info_hash and peer_id must be 20 bytes each");
    }
    
    std::vector<uint8_t> data;
    data.reserve(HANDSHAKE_LENGTH);
    
    // Protocol string length (1 byte)
    data.push_back(static_cast<uint8_t>(protocol.length()));
    
    // Protocol string (19 bytes)
    data.insert(data.end(), protocol.begin(), protocol.end());
    
    // Reserved bytes (8 bytes)
    for (int i = 0; i < 8; ++i) {
        data.push_back(0);
    }
    
    // Info hash (20 bytes)
    data.insert(data.end(), info_hash.begin(), info_hash.end());
    
    // Peer ID (20 bytes)
    data.insert(data.end(), peer_id.begin(), peer_id.end());
    
    return data;
}

HandshakeMessage HandshakeMessage::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < HANDSHAKE_LENGTH) {
        throw std::runtime_error("Invalid handshake message: too short");
    }
    
    HandshakeMessage msg;
    
    // Protocol string length
    uint8_t protocol_len = data[0];
    
    // Protocol string
    if (protocol_len != msg.protocol.length() || 
        std::string(data.begin() + 1, data.begin() + 1 + protocol_len) != msg.protocol) {
        throw std::runtime_error("Unsupported protocol");
    }
    
    // Info hash (starts at byte 28: 1 + 19 + 8)
    msg.info_hash = std::string(data.begin() + 28, data.begin() + 48);
    
    // Peer ID (starts at byte 48)
    msg.peer_id = std::string(data.begin() + 48, data.begin() + 68);
    
    return msg;
}

// Message factory
std::unique_ptr<Message> Message::create(const std::vector<uint8_t>& data) {
    if (data.size() < 5) {
        throw std::runtime_error("Message too short");
    }
    
    // First 4 bytes are the message length (big-endian)
    uint32_t length = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
    
    if (length == 0) {
        // Keep-alive message - return a simple message with KEEP_ALIVE type
        // Since we can't instantiate Message directly, we'll use a simple derived class
        struct KeepAliveMessage : public Message {
            KeepAliveMessage() : Message(MessageType::KEEP_ALIVE) {}
            std::vector<uint8_t> serialize() const override { return {}; }
        };
        return std::make_unique<KeepAliveMessage>();
    }
    
    if (data.size() < 5) {
        throw std::runtime_error("Invalid message: incomplete header");
    }
    
    MessageType type = static_cast<MessageType>(data[4]);
    std::vector<uint8_t> payload(data.begin() + 5, data.begin() + 4 + length);
    
    switch (type) {
        case MessageType::CHOKE:
            return std::make_unique<ChokeMessage>();
            
        case MessageType::UNCHOKE:
            return std::make_unique<UnchokeMessage>();
            
        case MessageType::INTERESTED:
            return std::make_unique<InterestedMessage>();
            
        case MessageType::NOT_INTERESTED:
            return std::make_unique<NotInterestedMessage>();
            
        case MessageType::HAVE:
            return HaveMessage::deserialize(payload);
            
        case MessageType::BITFIELD:
            // Note: Need to know the number of pieces to properly deserialize bitfield
            throw std::runtime_error("Bitfield deserialization requires number of pieces");
            
        case MessageType::REQUEST:
            return RequestMessage::deserialize(payload);
            
        case MessageType::PIECE:
            return PieceMessage::deserialize(payload);
            
        case MessageType::CANCEL:
            return CancelMessage::deserialize(payload);
            
        case MessageType::PORT:
            return PortMessage::deserialize(payload);
            
        default:
            throw std::runtime_error("Unknown message type: " + std::to_string(static_cast<int>(type)));
    }
}

// ChokeMessage implementation
std::vector<uint8_t> ChokeMessage::serialize() const {
    return {0, 0, 0, 1, static_cast<uint8_t>(MessageType::CHOKE)};
}

// UnchokeMessage implementation
std::vector<uint8_t> UnchokeMessage::serialize() const {
    return {0, 0, 0, 1, static_cast<uint8_t>(MessageType::UNCHOKE)};
}

// InterestedMessage implementation
std::vector<uint8_t> InterestedMessage::serialize() const {
    return {0, 0, 0, 1, static_cast<uint8_t>(MessageType::INTERESTED)};
}

// NotInterestedMessage implementation
std::vector<uint8_t> NotInterestedMessage::serialize() const {
    return {0, 0, 0, 1, static_cast<uint8_t>(MessageType::NOT_INTERESTED)};
}

// HaveMessage implementation
std::vector<uint8_t> HaveMessage::serialize() const {
    std::vector<uint8_t> data = {
        0, 0, 0, 5,  // Length = 5 (1 + 4)
        static_cast<uint8_t>(MessageType::HAVE),
        static_cast<uint8_t>((piece_index_ >> 24) & 0xFF),
        static_cast<uint8_t>((piece_index_ >> 16) & 0xFF),
        static_cast<uint8_t>((piece_index_ >> 8) & 0xFF),
        static_cast<uint8_t>(piece_index_ & 0xFF)
    };
    return data;
}

std::unique_ptr<HaveMessage> HaveMessage::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() != 4) {
        throw std::runtime_error("Invalid HAVE message: incorrect length");
    }
    
    uint32_t piece_index = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
    return std::make_unique<HaveMessage>(piece_index);
}

// BitfieldMessage implementation
BitfieldMessage::BitfieldMessage(const std::vector<bool>& bitfield) 
    : Message(MessageType::BITFIELD), bitfield_(bitfield) {}

std::vector<uint8_t> BitfieldMessage::serialize() const {
    size_t num_bytes = (bitfield_.size() + 7) / 8;
    std::vector<uint8_t> data(5 + num_bytes);
    
    // Length prefix (1 + num_bytes)
    data[0] = (num_bytes + 1) >> 24;
    data[1] = (num_bytes + 1) >> 16;
    data[2] = (num_bytes + 1) >> 8;
    data[3] = (num_bytes + 1) & 0xFF;
    
    // Message type
    data[4] = static_cast<uint8_t>(MessageType::BITFIELD);
    
    // Bitfield data
    for (size_t i = 0; i < bitfield_.size(); ++i) {
        if (bitfield_[i]) {
            data[5 + i/8] |= (1 << (7 - (i % 8)));
        }
    }
    
    return data;
}

std::unique_ptr<BitfieldMessage> BitfieldMessage::deserialize(
    const std::vector<uint8_t>& data, size_t num_pieces) {
    
    std::vector<bool> bitfield(num_pieces, false);
    
    for (size_t i = 0; i < num_pieces; ++i) {
        size_t byte_idx = i / 8;
        size_t bit_idx = 7 - (i % 8);
        
        if (byte_idx < data.size()) {
            bitfield[i] = (data[byte_idx] >> bit_idx) & 1;
        }
    }
    
    return std::make_unique<BitfieldMessage>(bitfield);
}

bool BitfieldMessage::has_piece(size_t index) const {
    if (index >= bitfield_.size()) {
        return false;
    }
    return bitfield_[index];
}

// RequestMessage implementation
std::vector<uint8_t> RequestMessage::serialize() const {
    std::vector<uint8_t> data(17);  // 4 (length) + 1 (type) + 12 (payload)
    
    // Length prefix (13)
    data[0] = 0; data[1] = 0; data[2] = 0; data[3] = 13;
    
    // Message type
    data[4] = static_cast<uint8_t>(MessageType::REQUEST);
    
    // Piece index (4 bytes)
    data[5] = (index_ >> 24) & 0xFF;
    data[6] = (index_ >> 16) & 0xFF;
    data[7] = (index_ >> 8) & 0xFF;
    data[8] = index_ & 0xFF;
    
    // Begin offset (4 bytes)
    data[9] = (begin_ >> 24) & 0xFF;
    data[10] = (begin_ >> 16) & 0xFF;
    data[11] = (begin_ >> 8) & 0xFF;
    data[12] = begin_ & 0xFF;
    
    // Request length (4 bytes)
    data[13] = (length_ >> 24) & 0xFF;
    data[14] = (length_ >> 16) & 0xFF;
    data[15] = (length_ >> 8) & 0xFF;
    data[16] = length_ & 0xFF;
    
    return data;
}

std::unique_ptr<RequestMessage> RequestMessage::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() != 12) {
        throw std::runtime_error("Invalid REQUEST message: incorrect length");
    }
    
    uint32_t index = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
    uint32_t begin = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
    uint32_t length = (data[8] << 24) | (data[9] << 16) | (data[10] << 8) | data[11];
    
    return std::make_unique<RequestMessage>(index, begin, length);
}

// PieceMessage implementation
std::vector<uint8_t> PieceMessage::serialize() const {
    std::vector<uint8_t> data(9 + block_.size());  // 4 (length) + 1 (type) + 8 (index+begin) + block
    
    // Length prefix (9 + block size)
    uint32_t length = 9 + block_.size();
    data[0] = (length >> 24) & 0xFF;
    data[1] = (length >> 16) & 0xFF;
    data[2] = (length >> 8) & 0xFF;
    data[3] = length & 0xFF;
    
    // Message type
    data[4] = static_cast<uint8_t>(MessageType::PIECE);
    
    // Piece index (4 bytes)
    data[5] = (index_ >> 24) & 0xFF;
    data[6] = (index_ >> 16) & 0xFF;
    data[7] = (index_ >> 8) & 0xFF;
    data[8] = index_ & 0xFF;
    
    // Begin offset (4 bytes)
    data[9] = (begin_ >> 24) & 0xFF;
    data[10] = (begin_ >> 16) & 0xFF;
    data[11] = (begin_ >> 8) & 0xFF;
    data[12] = begin_ & 0xFF;
    
    // Block data
    std::copy(block_.begin(), block_.end(), data.begin() + 13);
    
    return data;
}

std::unique_ptr<PieceMessage> PieceMessage::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 8) {
        throw std::runtime_error("Invalid PIECE message: too short");
    }
    
    uint32_t index = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
    uint32_t begin = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
    
    std::vector<uint8_t> block(data.begin() + 8, data.end());
    
    return std::make_unique<PieceMessage>(index, begin, block);
}

// CancelMessage implementation
std::vector<uint8_t> CancelMessage::serialize() const {
    std::vector<uint8_t> data(17);  // 4 (length) + 1 (type) + 12 (payload)
    
    // Length prefix (13)
    data[0] = 0; data[1] = 0; data[2] = 0; data[3] = 13;
    
    // Message type
    data[4] = static_cast<uint8_t>(MessageType::CANCEL);
    
    // Piece index (4 bytes)
    data[5] = (index_ >> 24) & 0xFF;
    data[6] = (index_ >> 16) & 0xFF;
    data[7] = (index_ >> 8) & 0xFF;
    data[8] = index_ & 0xFF;
    
    // Begin offset (4 bytes)
    data[9] = (begin_ >> 24) & 0xFF;
    data[10] = (begin_ >> 16) & 0xFF;
    data[11] = (begin_ >> 8) & 0xFF;
    data[12] = begin_ & 0xFF;
    
    // Request length (4 bytes)
    data[13] = (length_ >> 24) & 0xFF;
    data[14] = (length_ >> 16) & 0xFF;
    data[15] = (length_ >> 8) & 0xFF;
    data[16] = length_ & 0xFF;
    
    return data;
}

std::unique_ptr<CancelMessage> CancelMessage::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() != 12) {
        throw std::runtime_error("Invalid CANCEL message: incorrect length");
    }
    
    uint32_t index = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
    uint32_t begin = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
    uint32_t length = (data[8] << 24) | (data[9] << 16) | (data[10] << 8) | data[11];
    
    return std::make_unique<CancelMessage>(index, begin, length);
}

// PortMessage implementation
std::vector<uint8_t> PortMessage::serialize() const {
    return {
        0, 0, 0, 3,  // Length = 3 (1 + 2)
        static_cast<uint8_t>(MessageType::PORT),
        static_cast<uint8_t>((port_ >> 8) & 0xFF),
        static_cast<uint8_t>(port_ & 0xFF)
    };
}

std::unique_ptr<PortMessage> PortMessage::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() != 2) {
        throw std::runtime_error("Invalid PORT message: incorrect length");
    }
    
    uint16_t port = (data[0] << 8) | data[1];
    return std::make_unique<PortMessage>(port);
}

// Utility functions
std::string message_type_to_string(MessageType type) {
    switch (type) {
        case MessageType::CHOKE: return "CHOKE";
        case MessageType::UNCHOKE: return "UNCHOKE";
        case MessageType::INTERESTED: return "INTERESTED";
        case MessageType::NOT_INTERESTED: return "NOT_INTERESTED";
        case MessageType::HAVE: return "HAVE";
        case MessageType::BITFIELD: return "BITFIELD";
        case MessageType::REQUEST: return "REQUEST";
        case MessageType::PIECE: return "PIECE";
        case MessageType::CANCEL: return "CANCEL";
        case MessageType::PORT: return "PORT";
        case MessageType::KEEP_ALIVE: return "KEEP_ALIVE";
        default: return "UNKNOWN";
    }
}

std::string bytes_to_hex(const std::vector<uint8_t>& bytes) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    
    for (uint8_t b : bytes) {
        ss << std::setw(2) << static_cast<int>(b);
    }
    
    return ss.str();
}

std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    if (hex.length() % 2 != 0) {
        throw std::runtime_error("Hex string must have even length");
    }
    
    std::vector<uint8_t> bytes;
    
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byteString, nullptr, 16));
        bytes.push_back(byte);
    }
    
    return bytes;
}

} // namespace p2p
