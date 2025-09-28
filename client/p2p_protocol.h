#ifndef P2P_PROTOCOL_H
#define P2P_PROTOCOL_H

#include <string>
#include <vector>
#include <cstdint>
#include <map>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace p2p {

// Message types
enum class MessageType : uint8_t {
    HANDSHAKE = 0,
    CHOKE = 1,
    UNCHOKE = 2,
    INTERESTED = 3,
    NOT_INTERESTED = 4,
    HAVE = 5,
    BITFIELD = 6,
    REQUEST = 7,
    PIECE = 8,
    CANCEL = 9,
    PORT = 10,
    KEEP_ALIVE = 11
};

// Handshake message format
struct HandshakeMessage {
    static const size_t HANDSHAKE_LENGTH = 68; // 1 + 19 + 8 + 20 + 20
    static const std::string PROTOCOL_STRING;
    
    std::string protocol = "BitTorrent protocol";
    std::string info_hash;  // 20 bytes
    std::string peer_id;    // 20 bytes
    
    // Serialize to bytes
    std::vector<uint8_t> serialize() const;
    
    // Deserialize from bytes
    static HandshakeMessage deserialize(const std::vector<uint8_t>& data);
};

// Base message class
class Message {
public:
    explicit Message(MessageType type) : type_(type) {}
    virtual ~Message() = default;
    
    // Serialize message to bytes
    virtual std::vector<uint8_t> serialize() const = 0;
    
    // Get message type
    MessageType get_type() const { return type_; }
    
    // Create message from raw data
    static std::unique_ptr<Message> create(const std::vector<uint8_t>& data);
    
protected:
    MessageType type_;
};

// Choke message
class ChokeMessage : public Message {
public:
    ChokeMessage() : Message(MessageType::CHOKE) {}
    std::vector<uint8_t> serialize() const override;
};

// Unchoke message
class UnchokeMessage : public Message {
public:
    UnchokeMessage() : Message(MessageType::UNCHOKE) {}
    std::vector<uint8_t> serialize() const override;
};

// Interested message
class InterestedMessage : public Message {
public:
    InterestedMessage() : Message(MessageType::INTERESTED) {}
    std::vector<uint8_t> serialize() const override;
};

// Not interested message
class NotInterestedMessage : public Message {
public:
    NotInterestedMessage() : Message(MessageType::NOT_INTERESTED) {}
    std::vector<uint8_t> serialize() const override;
};

// Have message
class HaveMessage : public Message {
public:
    explicit HaveMessage(uint32_t piece_index) 
        : Message(MessageType::HAVE), piece_index_(piece_index) {}
    
    std::vector<uint8_t> serialize() const override;
    static std::unique_ptr<HaveMessage> deserialize(const std::vector<uint8_t>& data);
    
    uint32_t get_piece_index() const { return piece_index_; }
    
private:
    uint32_t piece_index_;
};

// Bitfield message
class BitfieldMessage : public Message {
public:
    explicit BitfieldMessage(const std::vector<bool>& bitfield);
    
    std::vector<uint8_t> serialize() const override;
    static std::unique_ptr<BitfieldMessage> deserialize(const std::vector<uint8_t>& data, size_t num_pieces);
    
    const std::vector<bool>& get_bitfield() const { return bitfield_; }
    bool has_piece(size_t index) const;
    
private:
    std::vector<bool> bitfield_;
};

// Request message
class RequestMessage : public Message {
public:
    RequestMessage(uint32_t index, uint32_t begin, uint32_t length)
        : Message(MessageType::REQUEST), index_(index), begin_(begin), length_(length) {}
    
    std::vector<uint8_t> serialize() const override;
    static std::unique_ptr<RequestMessage> deserialize(const std::vector<uint8_t>& data);
    
    uint32_t get_index() const { return index_; }
    uint32_t get_begin() const { return begin_; }
    uint32_t get_length() const { return length_; }
    
private:
    uint32_t index_;
    uint32_t begin_;
    uint32_t length_;
};

// Piece message
class PieceMessage : public Message {
public:
    PieceMessage(uint32_t index, uint32_t begin, const std::vector<uint8_t>& block)
        : Message(MessageType::PIECE), index_(index), begin_(begin), block_(block) {}
    
    std::vector<uint8_t> serialize() const override;
    static std::unique_ptr<PieceMessage> deserialize(const std::vector<uint8_t>& data);
    
    uint32_t get_index() const { return index_; }
    uint32_t get_begin() const { return begin_; }
    const std::vector<uint8_t>& get_block() const { return block_; }
    
private:
    uint32_t index_;
    uint32_t begin_;
    std::vector<uint8_t> block_;
};

// Cancel message
class CancelMessage : public Message {
public:
    CancelMessage(uint32_t index, uint32_t begin, uint32_t length)
        : Message(MessageType::CANCEL), index_(index), begin_(begin), length_(length) {}
    
    std::vector<uint8_t> serialize() const override;
    static std::unique_ptr<CancelMessage> deserialize(const std::vector<uint8_t>& data);
    
    uint32_t get_index() const { return index_; }
    uint32_t get_begin() const { return begin_; }
    uint32_t get_length() const { return length_; }
    
private:
    uint32_t index_;
    uint32_t begin_;
    uint32_t length_;
};

// Port message (for DHT)
class PortMessage : public Message {
public:
    explicit PortMessage(uint16_t port) : Message(MessageType::PORT), port_(port) {}
    
    std::vector<uint8_t> serialize() const override;
    static std::unique_ptr<PortMessage> deserialize(const std::vector<uint8_t>& data);
    
    uint16_t get_port() const { return port_; }
    
private:
    uint16_t port_;
};

// Peer connection state
enum class PeerState {
    DISCONNECTED,
    CONNECTING,
    HANDSHAKING,
    CONNECTED,
    CLOSING
};

// Peer information
struct PeerInfo {
    std::string ip;
    uint16_t port;
    std::string peer_id;
    std::vector<bool> bitfield;
    bool is_choked;
    bool am_choked;
    bool is_interested;
    bool am_interested;
    
    PeerInfo() : port(0), is_choked(true), am_choked(true), 
                is_interested(false), am_interested(false) {}
};

// Utility functions
std::string message_type_to_string(MessageType type);
std::string bytes_to_hex(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> hex_to_bytes(const std::string& hex);

} // namespace p2p

#endif // P2P_PROTOCOL_H
