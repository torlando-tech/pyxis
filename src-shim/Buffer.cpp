#include "Buffer.h"
#include "ChannelData.h"  // Required for Channel template instantiation
#include "Cryptography/BZ2.h"

#include <algorithm>

namespace RNS {

//==============================================================================
// RawChannelReaderData - Internal data for RawChannelReader
//==============================================================================

class RawChannelReaderData {
public:
    RawChannelReaderData(uint16_t stream_id, Channel& channel)
        : _stream_id(stream_id), _channel(&channel), _eof(false), _closed(false) {
        MEM("RawChannelReaderData object created");
    }

    ~RawChannelReaderData() {
        if (!_closed) {
            close();
        }
        MEM("RawChannelReaderData object destroyed");
    }

    bool handle_message(MessageBase& msg) {
        if (_closed) return false;

        // Only handle StreamDataMessage
        if (msg.msgtype() != StreamDataMessage::MSGTYPE) {
            return false;
        }

        StreamDataMessage& stream_msg = static_cast<StreamDataMessage&>(msg);

        // Filter by stream_id
        if (stream_msg.stream_id != _stream_id) {
            return false;
        }

        DEBUGF("RawChannelReader: Received %zu bytes on stream %u (eof=%d)",
               stream_msg.data.size(), _stream_id, stream_msg.eof);

        // Accumulate data
        if (stream_msg.data) {
            _buffer += stream_msg.data;
        }

        // Set EOF flag
        if (stream_msg.eof) {
            _eof = true;
            DEBUG("RawChannelReader: EOF received");
        }

        // Notify callbacks
        notify_ready();

        return true;  // Message consumed
    }

    void notify_ready() {
        size_t avail = _buffer.size();
        if (avail == 0 && !_eof) return;

        // Make a copy of callbacks in case they modify the list
        auto callbacks_copy = _ready_callbacks;
        for (auto& callback : callbacks_copy) {
            try {
                callback(avail);
            } catch (...) {
                ERROR("RawChannelReader: Callback threw exception");
            }
        }
    }

    void close() {
        _closed = true;
        _ready_callbacks.clear();
        DEBUG("RawChannelReader: Closed");
    }

    uint16_t _stream_id;
    Channel* _channel;
    Bytes _buffer;
    bool _eof;
    bool _closed;
    std::vector<RawChannelReader::ReadyCallback> _ready_callbacks;
};

//==============================================================================
// StreamDataMessage
//==============================================================================

StreamDataMessage::StreamDataMessage(uint16_t stream_id, const Bytes& data,
                                     bool eof, bool compressed)
    : stream_id(stream_id), data(data), eof(eof), compressed(compressed) {
}

Bytes StreamDataMessage::pack() const {
    // Build 2-byte header: EOF(bit15) | COMP(bit14) | STREAM_ID(bits13-0)
    uint16_t header = (stream_id & Type::Buffer::STREAM_ID_MASK);
    if (eof) header |= Type::Buffer::FLAG_EOF;
    if (compressed) header |= Type::Buffer::FLAG_COMPRESSED;

    Bytes result;
    result.reserve(Type::Buffer::STREAM_OVERHEAD + data.size());

    // Big-endian header (matching Python struct.pack(">H", header))
    result.append(static_cast<uint8_t>((header >> 8) & 0xFF));
    result.append(static_cast<uint8_t>(header & 0xFF));

    // Data payload (already compressed if compressed flag is set)
    if (data) {
        result += data;
    }

    return result;
}

void StreamDataMessage::unpack(const Bytes& raw) {
    if (raw.size() < Type::Buffer::STREAM_OVERHEAD) {
        ERROR("StreamDataMessage::unpack: Data too short");
        return;
    }

    // Read big-endian header
    uint16_t header = (static_cast<uint16_t>(raw[0]) << 8) | raw[1];

    eof = (header & Type::Buffer::FLAG_EOF) != 0;
    compressed = (header & Type::Buffer::FLAG_COMPRESSED) != 0;
    stream_id = header & Type::Buffer::STREAM_ID_MASK;

    // Extract data payload
    if (raw.size() > Type::Buffer::STREAM_OVERHEAD) {
        data = raw.mid(Type::Buffer::STREAM_OVERHEAD);

        // Decompress if needed
        if (compressed && data.size() > 0) {
            data = Cryptography::bz2_decompress(data);
        }
    } else {
        data = Bytes::NONE;
    }
}

//==============================================================================
// RawChannelReader - Uses shared_ptr to RawChannelReaderData
//==============================================================================

RawChannelReader::RawChannelReader(uint16_t stream_id, Channel& channel)
    : _object(std::make_shared<RawChannelReaderData>(stream_id, channel)) {

    // Register StreamDataMessage type (as system message)
    _object->_channel->register_message_type<StreamDataMessage>(true);

    // Add message handler - capture shared_ptr to keep data alive
    // and use weak_ptr to avoid preventing destruction
    std::weak_ptr<RawChannelReaderData> weak_obj = _object;
    _object->_channel->add_message_handler([weak_obj](MessageBase& msg) -> bool {
        if (auto obj = weak_obj.lock()) {
            return obj->handle_message(msg);
        }
        return false;  // Object destroyed, don't handle
    });

    DEBUGF("RawChannelReader: Created for stream_id=%u", stream_id);
}

RawChannelReader::~RawChannelReader() {
    // shared_ptr handles cleanup automatically
    MEM("RawChannelReader object destroyed");
}

Bytes RawChannelReader::read(size_t max_bytes) {
    if (!_object || _object->_buffer.size() == 0) {
        return Bytes::NONE;
    }

    size_t to_read = (max_bytes == 0) ? _object->_buffer.size()
                                      : std::min(max_bytes, _object->_buffer.size());
    Bytes result = _object->_buffer.left(to_read);
    _object->_buffer = _object->_buffer.mid(to_read);

    DEBUGF("RawChannelReader: Read %zu bytes, %zu remaining", to_read, _object->_buffer.size());
    return result;
}

Bytes RawChannelReader::readline() {
    if (!_object) return Bytes::NONE;

    // Search for newline
    for (size_t i = 0; i < _object->_buffer.size(); i++) {
        if (_object->_buffer[i] == '\n') {
            // Return including the newline
            Bytes line = _object->_buffer.left(i + 1);
            _object->_buffer = _object->_buffer.mid(i + 1);
            return line;
        }
    }

    // No complete line available
    // If EOF, return remaining data as final line (no newline)
    if (_object->_eof && _object->_buffer.size() > 0) {
        Bytes line = _object->_buffer;
        _object->_buffer = Bytes::NONE;
        return line;
    }

    return Bytes::NONE;
}

size_t RawChannelReader::available() const {
    if (!_object) return 0;
    return _object->_buffer.size();
}

bool RawChannelReader::eof() const {
    if (!_object) return true;
    return _object->_eof && _object->_buffer.size() == 0;
}

void RawChannelReader::add_ready_callback(ReadyCallback callback) {
    if (!_object) return;
    _object->_ready_callbacks.push_back(callback);
}

void RawChannelReader::remove_ready_callback(ReadyCallback callback) {
    // Note: Function comparison is tricky in C++
    // This is a simplified implementation
    // For production, consider using callback IDs
}

void RawChannelReader::close() {
    if (!_object) return;
    _object->close();
}

//==============================================================================
// RawChannelWriter
//==============================================================================

RawChannelWriter::RawChannelWriter(uint16_t stream_id, Channel& channel)
    : _stream_id(stream_id), _channel(&channel), _eof_sent(false) {

    // Calculate max data length (Channel MDU minus stream header)
    _max_data_len = _channel->mdu() - Type::Buffer::STREAM_OVERHEAD;

    // Register StreamDataMessage type (as system message)
    // This may already be registered by a reader, but that's OK
    _channel->register_message_type<StreamDataMessage>(true);

    DEBUGF("RawChannelWriter: Created for stream_id=%u, max_data_len=%zu",
           _stream_id, _max_data_len);
}

RawChannelWriter::~RawChannelWriter() {
    if (!_eof_sent && _channel) {
        close();
    }
}

RawChannelWriter::RawChannelWriter(RawChannelWriter&& other) noexcept
    : _stream_id(other._stream_id),
      _channel(other._channel),
      _max_data_len(other._max_data_len),
      _eof_sent(other._eof_sent) {
    other._channel = nullptr;
    other._eof_sent = true;
}

RawChannelWriter& RawChannelWriter::operator=(RawChannelWriter&& other) noexcept {
    if (this != &other) {
        if (!_eof_sent && _channel) {
            close();
        }
        _stream_id = other._stream_id;
        _channel = other._channel;
        _max_data_len = other._max_data_len;
        _eof_sent = other._eof_sent;
        other._channel = nullptr;
        other._eof_sent = true;
    }
    return *this;
}

size_t RawChannelWriter::write(const Bytes& data) {
    if (!_channel || _eof_sent) {
        ERROR("RawChannelWriter: Cannot write after close");
        return 0;
    }

    if (!data || data.size() == 0) {
        return 0;
    }

    size_t chunk_len = std::min(data.size(), Type::Buffer::MAX_CHUNK_LEN);
    Bytes chunk = data.left(chunk_len);

    bool use_compression = false;
    Bytes send_data;
    size_t processed = 0;

    // Try compression at decreasing chunk sizes (matching Python Buffer.py)
    if (chunk_len > Type::Buffer::COMPRESSION_MIN_SIZE) {
        for (size_t try_num = 1; try_num <= Type::Buffer::COMPRESSION_TRIES; try_num++) {
            size_t segment_len = chunk_len / try_num;
            if (segment_len == 0) break;

            Bytes segment = chunk.left(segment_len);
            Bytes compressed = Cryptography::bz2_compress(segment);

            if (compressed.size() < _max_data_len &&
                compressed.size() < segment_len) {
                use_compression = true;
                send_data = compressed;
                processed = segment_len;
                DEBUGF("RawChannelWriter: Compression succeeded: %zu -> %zu bytes",
                       segment_len, compressed.size());
                break;
            }
        }
    }

    // If compression didn't help, send uncompressed
    if (!use_compression) {
        send_data = chunk.left(_max_data_len);
        processed = send_data.size();
    }

    // Build and send message
    StreamDataMessage msg;
    msg.stream_id = _stream_id;
    msg.data = send_data;
    msg.eof = false;
    msg.compressed = use_compression;

    _channel->send(msg);

    DEBUGF("RawChannelWriter: Sent %zu bytes on stream %u (compressed=%d)",
           processed, _stream_id, use_compression);

    return processed;
}

void RawChannelWriter::flush() {
    // In the current implementation, write() sends immediately
    // This is a no-op but provided for API compatibility
    DEBUG("RawChannelWriter: Flush called (no-op)");
}

void RawChannelWriter::close() {
    if (_eof_sent) return;

    if (_channel) {
        // Send EOF message
        StreamDataMessage msg;
        msg.stream_id = _stream_id;
        msg.data = Bytes::NONE;
        msg.eof = true;
        msg.compressed = false;

        _channel->send(msg);

        DEBUGF("RawChannelWriter: Sent EOF on stream %u", _stream_id);
    }

    _eof_sent = true;
}

//==============================================================================
// Buffer namespace factory functions
//==============================================================================

namespace Buffer {

RawChannelReader create_reader(uint16_t stream_id, Channel& channel,
                               RawChannelReader::ReadyCallback callback) {
    RawChannelReader reader(stream_id, channel);
    if (callback) {
        reader.add_ready_callback(callback);
    }
    return reader;
}

RawChannelWriter create_writer(uint16_t stream_id, Channel& channel) {
    return RawChannelWriter(stream_id, channel);
}

std::pair<RawChannelReader, RawChannelWriter>
create_bidirectional_buffer(uint16_t rx_stream_id, uint16_t tx_stream_id,
                            Channel& channel,
                            RawChannelReader::ReadyCallback callback) {
    RawChannelReader reader(rx_stream_id, channel);
    if (callback) {
        reader.add_ready_callback(callback);
    }
    RawChannelWriter writer(tx_stream_id, channel);
    return std::make_pair(std::move(reader), std::move(writer));
}

} // namespace Buffer

} // namespace RNS
