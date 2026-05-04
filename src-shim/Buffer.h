#pragma once

#ifndef RNS_BUFFER_H
#define RNS_BUFFER_H

#include "Bytes.h"
#include "Channel.h"
#include "MessageBase.h"
#include "Type.h"
#include "Log.h"

#include <memory>
#include <functional>
#include <vector>
#include <utility>

namespace RNS {

/**
 * StreamDataMessage - Wire format for Buffer data over Channel
 *
 * Header (2 bytes, big-endian):
 *   Bit 15: EOF flag
 *   Bit 14: Compression flag
 *   Bits 13-0: Stream ID (max 16383)
 *
 * Followed by optional data payload (BZ2 compressed if flag set)
 */
class StreamDataMessage : public MessageBase {
public:
    static constexpr uint16_t MSGTYPE = Type::Channel::SMT_STREAM_DATA;

    // Fields
    uint16_t stream_id = 0;
    Bytes data;
    bool eof = false;
    bool compressed = false;

    // Constructors
    StreamDataMessage() = default;
    StreamDataMessage(uint16_t stream_id, const Bytes& data = Bytes::NONE,
                      bool eof = false, bool compressed = false);

    // MessageBase interface
    uint16_t msgtype() const override { return MSGTYPE; }
    Bytes pack() const override;
    void unpack(const Bytes& raw) override;
};

// Forward declaration for internal data
class RawChannelReaderData;

/**
 * RawChannelReader - Read stream data from a Channel
 *
 * Uses shared_ptr internally so that move operations preserve callback validity.
 */
class RawChannelReader {
public:
    using ReadyCallback = std::function<void(size_t)>;

    RawChannelReader(uint16_t stream_id, Channel& channel);
    RawChannelReader(Type::NoneConstructor none) { MEM("RawChannelReader NONE object created"); }
    ~RawChannelReader();

    // Copy/move - shallow copy of shared_ptr (like other RNS types)
    RawChannelReader(const RawChannelReader& other) : _object(other._object) {
        MEM("RawChannelReader object copy created");
    }
    RawChannelReader(RawChannelReader&& other) noexcept : _object(std::move(other._object)) {
        MEM("RawChannelReader object moved");
    }
    RawChannelReader& operator=(const RawChannelReader& other) {
        _object = other._object;
        return *this;
    }
    RawChannelReader& operator=(RawChannelReader&& other) noexcept {
        _object = std::move(other._object);
        return *this;
    }

    // Validity check
    operator bool() const { return _object.get() != nullptr; }

    // Reading interface
    Bytes read(size_t max_bytes = 0);  // 0 = read all available
    Bytes readline();
    size_t available() const;
    bool eof() const;

    // Callback management
    void add_ready_callback(ReadyCallback callback);
    void remove_ready_callback(ReadyCallback callback);

    // Cleanup
    void close();

private:
    std::shared_ptr<RawChannelReaderData> _object;
};

/**
 * RawChannelWriter - Write stream data to a Channel
 */
class RawChannelWriter {
public:
    RawChannelWriter(uint16_t stream_id, Channel& channel);
    ~RawChannelWriter();

    // Disable copy
    RawChannelWriter(const RawChannelWriter&) = delete;
    RawChannelWriter& operator=(const RawChannelWriter&) = delete;

    // Move support
    RawChannelWriter(RawChannelWriter&& other) noexcept;
    RawChannelWriter& operator=(RawChannelWriter&& other) noexcept;

    // Writing interface
    size_t write(const Bytes& data);
    void flush();
    void close();  // Send EOF

private:
    uint16_t _stream_id;
    Channel* _channel;
    size_t _max_data_len;
    bool _eof_sent = false;
};

/**
 * Buffer namespace - Factory functions for creating readers/writers
 */
namespace Buffer {

    /**
     * Create a reader for receiving stream data
     */
    RawChannelReader create_reader(uint16_t stream_id, Channel& channel,
                                   RawChannelReader::ReadyCallback callback = nullptr);

    /**
     * Create a writer for sending stream data
     */
    RawChannelWriter create_writer(uint16_t stream_id, Channel& channel);

    /**
     * Create bidirectional buffer pair
     * @param rx_stream_id Stream ID for receiving data
     * @param tx_stream_id Stream ID for sending data
     * @param channel The Channel to use
     * @param callback Optional callback when data is ready to read
     * @return Pair of (reader, writer)
     */
    std::pair<RawChannelReader, RawChannelWriter>
    create_bidirectional_buffer(uint16_t rx_stream_id, uint16_t tx_stream_id,
                                Channel& channel,
                                RawChannelReader::ReadyCallback callback = nullptr);

} // namespace Buffer

} // namespace RNS

#endif // RNS_BUFFER_H
