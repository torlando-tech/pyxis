// Minimal Bytes shim for native pyxis tests.
//
// HDLC.h, BLE fragmenter, ring buffers etc. depend on RNS::Bytes, which in
// turn depends on ArduinoJson and other Arduino-only headers. For native
// unit tests of pyxis-unique logic we only need a small subset of Bytes'
// API — enough to make HDLC/etc. compile and run identically to the device.
//
// This shim covers exactly that subset. It is NOT a drop-in replacement
// for RNS::Bytes; it deliberately omits the parts that need RNS internals
// (msgpack pack/unpack, hex helpers, JSON conversion, etc.).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace RNS {

class Bytes {
public:
    Bytes() = default;
    Bytes(const uint8_t* chunk, size_t size) : _data(chunk, chunk + size) {}
    explicit Bytes(size_t capacity) { _data.reserve(capacity); }

    void reserve(size_t n) { _data.reserve(n); }
    size_t size() const { return _data.size(); }
    const uint8_t* data() const { return _data.data(); }
    uint8_t* data() { return _data.data(); }
    bool empty() const { return _data.empty(); }
    void clear() { _data.clear(); }

    // The HDLC API uses these two append overloads. RNS::Bytes has more
    // overloads; we only need these for HDLC::escape/unescape/frame.
    Bytes& append(uint8_t b) {
        _data.push_back(b);
        return *this;
    }
    Bytes& append(const Bytes& other) {
        _data.insert(_data.end(), other._data.begin(), other._data.end());
        return *this;
    }
    Bytes& append(const uint8_t* chunk, size_t n) {
        _data.insert(_data.end(), chunk, chunk + n);
        return *this;
    }

    // Resize the underlying buffer. Used by BLEFragmenter / BLEReassembler
    // after writable() to set the final size.
    void resize(size_t newsize) { _data.resize(newsize); }

    // Reserve+resize and return a writable pointer to the buffer.
    // Match the real RNS::Bytes semantics: writable(N) gives a pointer to
    // an N-byte buffer the caller can write into, and the size becomes N.
    uint8_t* writable(size_t size) {
        _data.resize(size);
        return _data.data();
    }

    bool operator==(const Bytes& other) const { return _data == other._data; }
    bool operator!=(const Bytes& other) const { return _data != other._data; }

    // Mid-substring — used by the TCP frame extractor. Provide both the
    // (begin, len) and (begin) overloads to match RNS::Bytes.
    Bytes mid(size_t begin, size_t len) const {
        if (begin >= _data.size()) return Bytes();
        size_t take = std::min(len, _data.size() - begin);
        return Bytes(_data.data() + begin, take);
    }
    Bytes mid(size_t begin) const {
        if (begin >= _data.size()) return Bytes();
        return Bytes(_data.data() + begin, _data.size() - begin);
    }

private:
    std::vector<uint8_t> _data;
};

}  // namespace RNS
