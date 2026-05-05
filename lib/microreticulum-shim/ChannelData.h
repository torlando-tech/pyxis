#pragma once

#ifndef RNS_CHANNEL_DATA_H
#define RNS_CHANNEL_DATA_H

#include "Bytes.h"
#include "Link.h"
#include "Packet.h"
#include "MessageBase.h"
#include "Type.h"
#include "Log.h"

#include <map>
#include <vector>
#include <functional>
#include <memory>
#include <utility>  // for std::move

namespace RNS {

// Forward declaration
class Channel;

// Envelope wraps a message with protocol metadata
class Envelope {
public:
    Envelope() = default;
    Envelope(uint16_t msgtype, uint16_t sequence, const Bytes& raw)
        : _msgtype(msgtype), _sequence(sequence), _raw(raw) {}

    // Move semantics (unique_ptr makes class non-copyable)
    Envelope(Envelope&&) = default;
    Envelope& operator=(Envelope&&) = default;

    // Delete copy operations
    Envelope(const Envelope&) = delete;
    Envelope& operator=(const Envelope&) = delete;

    uint16_t msgtype() const { return _msgtype; }
    uint16_t sequence() const { return _sequence; }
    const Bytes& raw() const { return _raw; }

    // For TX tracking
    Packet packet() const { return _packet; }
    void set_packet(const Packet& packet) { _packet = packet; }
    uint8_t tries() const { return _tries; }
    void increment_tries() { _tries++; }
    double timestamp() const { return _timestamp; }
    void set_timestamp(double ts) { _timestamp = ts; }
    bool tracked() const { return _tracked; }
    void set_tracked(bool tracked) { _tracked = tracked; }

    // Message instance (for RX)
    std::unique_ptr<MessageBase>& message() { return _message; }
    void set_message(std::unique_ptr<MessageBase> msg) { _message = std::move(msg); }

    // Pack envelope to wire format (big-endian)
    Bytes pack() const {
        // Wire format: MSGTYPE(2) + SEQUENCE(2) + LENGTH(2) + DATA(N)
        // All values big-endian
        Bytes result;

        // Allocate exact size needed
        size_t data_len = _raw.size();
        result.reserve(6 + data_len);

        // MSGTYPE (2 bytes, big-endian)
        result.append((uint8_t)((_msgtype >> 8) & 0xFF));
        result.append((uint8_t)(_msgtype & 0xFF));

        // SEQUENCE (2 bytes, big-endian)
        result.append((uint8_t)((_sequence >> 8) & 0xFF));
        result.append((uint8_t)(_sequence & 0xFF));

        // LENGTH (2 bytes, big-endian)
        result.append((uint8_t)((data_len >> 8) & 0xFF));
        result.append((uint8_t)(data_len & 0xFF));

        // DATA
        result += _raw;

        return result;
    }

    // Unpack envelope from wire format (big-endian)
    static bool unpack(const Bytes& wire_data, Envelope& out) {
        // Need at least 6 bytes for header
        if (wire_data.size() < 6) {
            return false;
        }

        const uint8_t* data = wire_data.data();

        // MSGTYPE (2 bytes, big-endian)
        uint16_t msgtype = (static_cast<uint16_t>(data[0]) << 8) | data[1];

        // SEQUENCE (2 bytes, big-endian)
        uint16_t sequence = (static_cast<uint16_t>(data[2]) << 8) | data[3];

        // LENGTH (2 bytes, big-endian)
        uint16_t length = (static_cast<uint16_t>(data[4]) << 8) | data[5];

        // Validate length
        if (wire_data.size() < 6 + static_cast<size_t>(length)) {
            return false;
        }

        // Extract data payload
        Bytes raw;
        if (length > 0) {
            raw = wire_data.mid(6, length);
        }

        out = Envelope(msgtype, sequence, raw);
        return true;
    }

private:
    uint16_t _msgtype = 0;
    uint16_t _sequence = 0;
    Bytes _raw;
    Packet _packet = {Type::NONE};
    uint8_t _tries = 0;
    double _timestamp = 0.0;
    bool _tracked = false;
    std::unique_ptr<MessageBase> _message;
};

// Internal Channel data structure
class ChannelData {
public:
    enum class WindowTier { FAST, MEDIUM, SLOW, VERY_SLOW };

    // Fixed ring buffer sizes
    static constexpr size_t RX_RING_SIZE = 16;
    static constexpr size_t TX_RING_SIZE = 16;

    ChannelData() { MEM("ChannelData object created"); }
    ChannelData(const Link& link) : _link(link) { MEM("ChannelData object created with link"); }
    virtual ~ChannelData() { MEM("ChannelData object destroyed"); }

    // RX Ring buffer operations (ordered by sequence)
    bool rx_ring_empty() const { return _rx_ring_count == 0; }
    size_t rx_ring_size() const { return _rx_ring_count; }
    bool rx_ring_full() const { return _rx_ring_count >= RX_RING_SIZE; }

    Envelope& rx_ring_front() {
        return _rx_ring_pool[_rx_ring_head];
    }

    void rx_ring_pop_front() {
        if (_rx_ring_count > 0) {
            // Reset the envelope being removed
            _rx_ring_pool[_rx_ring_head] = Envelope();
            _rx_ring_head = (_rx_ring_head + 1) % RX_RING_SIZE;
            _rx_ring_count--;
        }
    }

    void rx_ring_clear() {
        // Reset all envelopes
        for (size_t i = 0; i < RX_RING_SIZE; i++) {
            _rx_ring_pool[i] = Envelope();
        }
        _rx_ring_head = 0;
        _rx_ring_tail = 0;
        _rx_ring_count = 0;
    }

    // Insert envelope in sequence order (for reordering)
    // Returns false if ring is full
    bool rx_ring_insert_ordered(Envelope&& envelope) {
        if (_rx_ring_count >= RX_RING_SIZE) {
            return false;
        }

        uint16_t new_seq = envelope.sequence();

        // If empty, just add at head
        if (_rx_ring_count == 0) {
            _rx_ring_pool[_rx_ring_head] = std::move(envelope);
            _rx_ring_tail = (_rx_ring_head + 1) % RX_RING_SIZE;
            _rx_ring_count = 1;
            return true;
        }

        // Find insertion position by iterating through valid entries
        // We need to shift elements to make room
        size_t insert_pos = _rx_ring_count;  // Default: insert at end

        for (size_t i = 0; i < _rx_ring_count; i++) {
            size_t idx = (_rx_ring_head + i) % RX_RING_SIZE;
            uint16_t existing_seq = _rx_ring_pool[idx].sequence();

            // Calculate relative position (handling wraparound)
            int32_t diff = static_cast<int32_t>(new_seq) - static_cast<int32_t>(existing_seq);
            if (diff >= static_cast<int32_t>(Type::Channel::SEQ_MODULUS / 2)) {
                diff -= Type::Channel::SEQ_MODULUS;
            } else if (diff < -static_cast<int32_t>(Type::Channel::SEQ_MODULUS / 2)) {
                diff += Type::Channel::SEQ_MODULUS;
            }

            if (diff < 0) {
                // Insert before this position
                insert_pos = i;
                break;
            }
        }

        // Shift elements from insert_pos to end to make room
        // We shift by moving tail back and shifting elements
        for (size_t i = _rx_ring_count; i > insert_pos; i--) {
            size_t dst_idx = (_rx_ring_head + i) % RX_RING_SIZE;
            size_t src_idx = (_rx_ring_head + i - 1) % RX_RING_SIZE;
            _rx_ring_pool[dst_idx] = std::move(_rx_ring_pool[src_idx]);
        }

        // Insert the new envelope
        size_t actual_idx = (_rx_ring_head + insert_pos) % RX_RING_SIZE;
        _rx_ring_pool[actual_idx] = std::move(envelope);
        _rx_ring_tail = (_rx_ring_tail + 1) % RX_RING_SIZE;
        _rx_ring_count++;

        return true;
    }

    // Check if sequence exists in RX ring
    bool rx_ring_contains_sequence(uint16_t sequence) const {
        for (size_t i = 0; i < _rx_ring_count; i++) {
            size_t idx = (_rx_ring_head + i) % RX_RING_SIZE;
            if (_rx_ring_pool[idx].sequence() == sequence) {
                return true;
            }
        }
        return false;
    }

    // TX Ring buffer operations (simple FIFO with removal by packet match)
    bool tx_ring_empty() const { return _tx_ring_count == 0; }
    size_t tx_ring_size() const { return _tx_ring_count; }
    bool tx_ring_full() const { return _tx_ring_count >= TX_RING_SIZE; }

    bool tx_ring_push_back(Envelope&& envelope) {
        if (_tx_ring_count >= TX_RING_SIZE) {
            return false;
        }
        _tx_ring_pool[_tx_ring_tail] = std::move(envelope);
        _tx_ring_tail = (_tx_ring_tail + 1) % TX_RING_SIZE;
        _tx_ring_count++;
        return true;
    }

    void tx_ring_clear() {
        for (size_t i = 0; i < TX_RING_SIZE; i++) {
            _tx_ring_pool[i] = Envelope();
        }
        _tx_ring_head = 0;
        _tx_ring_tail = 0;
        _tx_ring_count = 0;
    }

    // Remove envelope matching packet, returns true if found and removed
    bool tx_ring_remove_by_packet(const Packet& packet) {
        for (size_t i = 0; i < _tx_ring_count; i++) {
            size_t idx = (_tx_ring_head + i) % TX_RING_SIZE;
            if (_tx_ring_pool[idx].packet() == packet) {
                // Shift remaining elements forward
                for (size_t j = i; j < _tx_ring_count - 1; j++) {
                    size_t dst_idx = (_tx_ring_head + j) % TX_RING_SIZE;
                    size_t src_idx = (_tx_ring_head + j + 1) % TX_RING_SIZE;
                    _tx_ring_pool[dst_idx] = std::move(_tx_ring_pool[src_idx]);
                }
                // Clear the last slot
                size_t last_idx = (_tx_ring_head + _tx_ring_count - 1) % TX_RING_SIZE;
                _tx_ring_pool[last_idx] = Envelope();
                _tx_ring_count--;
                return true;
            }
        }
        return false;
    }

    // Find envelope by packet (returns nullptr if not found)
    Envelope* tx_ring_find_by_packet(const Packet& packet) {
        for (size_t i = 0; i < _tx_ring_count; i++) {
            size_t idx = (_tx_ring_head + i) % TX_RING_SIZE;
            if (_tx_ring_pool[idx].packet() == packet) {
                return &_tx_ring_pool[idx];
            }
        }
        return nullptr;
    }

    // Find envelope by sequence (returns nullptr if not found)
    Envelope* tx_ring_find_by_sequence(uint16_t sequence) {
        for (size_t i = 0; i < _tx_ring_count; i++) {
            size_t idx = (_tx_ring_head + i) % TX_RING_SIZE;
            if (_tx_ring_pool[idx].sequence() == sequence) {
                return &_tx_ring_pool[idx];
            }
        }
        return nullptr;
    }

    // Iterate over TX ring (for counting outstanding, checking timeouts)
    template<typename Func>
    void tx_ring_foreach(Func&& func) {
        for (size_t i = 0; i < _tx_ring_count; i++) {
            size_t idx = (_tx_ring_head + i) % TX_RING_SIZE;
            func(_tx_ring_pool[idx]);
        }
    }

    template<typename Func>
    void tx_ring_foreach(Func&& func) const {
        for (size_t i = 0; i < _tx_ring_count; i++) {
            size_t idx = (_tx_ring_head + i) % TX_RING_SIZE;
            func(_tx_ring_pool[idx]);
        }
    }

private:
    friend class Channel;

    // Link reference
    Link _link = {Type::NONE};

    // Sequencing
    uint16_t _next_sequence = 0;
    uint16_t _next_rx_sequence = 0;

    // RX Ring buffer (fixed-size circular buffer, ordered by sequence)
    Envelope _rx_ring_pool[RX_RING_SIZE];
    size_t _rx_ring_head = 0;
    size_t _rx_ring_tail = 0;
    size_t _rx_ring_count = 0;

    // TX Ring buffer (fixed-size circular buffer)
    Envelope _tx_ring_pool[TX_RING_SIZE];
    size_t _tx_ring_head = 0;
    size_t _tx_ring_tail = 0;
    size_t _tx_ring_count = 0;

    // Message dispatch
    // Factory: msgtype -> function that creates a new message instance
    // Note: These are only set up once at initialization, so map is acceptable
    std::map<uint16_t, std::function<std::unique_ptr<MessageBase>()>> _message_factories;
    // Handlers: list of callbacks, first returning true stops dispatch
    // Note: These are only set up once at initialization, so vector is acceptable
    std::vector<std::function<bool(MessageBase&)>> _message_callbacks;

    // Window management
    uint16_t _window = Type::Channel::WINDOW_INITIAL;
    uint16_t _window_min = Type::Channel::WINDOW_MIN;
    uint16_t _window_max = Type::Channel::WINDOW_MAX;
    uint16_t _fast_rate_rounds = 0;

    // Timing/RTT
    double _rtt = 0.0;
    uint8_t _max_tries = Type::Channel::MAX_TRIES;
    WindowTier _current_tier = WindowTier::MEDIUM;

    // State
    bool _ready = false;
};

} // namespace RNS

#endif // RNS_CHANNEL_DATA_H
