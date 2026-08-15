#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "NomadNetMemory.h"

namespace UI::LXMF::NomadNet {

inline std::vector<uint8_t> no_form_request_data() {
    // Link::request splices already encoded msgpack data into its envelope.
    // NomadNet's no-form value is protocol-level nil, exactly one byte 0xc0.
    return {0xc0};
}

inline void append_msgpack_string(std::vector<uint8_t>& output, const std::string& value) {
    const std::size_t size = value.size();
    if (size <= 31) {
        output.push_back(static_cast<uint8_t>(0xa0 | size));
    } else if (size <= 0xff) {
        output.push_back(0xd9);
        output.push_back(static_cast<uint8_t>(size));
    } else {
        output.push_back(0xda);
        output.push_back(static_cast<uint8_t>(size >> 8));
        output.push_back(static_cast<uint8_t>(size));
    }
    output.insert(output.end(), value.begin(), value.end());
}

inline std::vector<uint8_t> request_data(const std::string& fields) {
    if (fields.empty()) return no_form_request_data();

    std::size_t variable_count = 0;
    for (std::size_t start = 0; start <= fields.size();) {
        const std::size_t end = fields.find('|', start);
        const std::size_t length = (end == std::string::npos ? fields.size() : end) - start;
        const std::string field = fields.substr(start, length);
        const std::size_t equals = field.find('=');
        if (equals != std::string::npos && field.find('=', equals + 1) == std::string::npos) {
            ++variable_count;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }

    std::vector<uint8_t> output;
    output.reserve(fields.size() + variable_count * 5 + 3);
    if (variable_count <= 15) {
        output.push_back(static_cast<uint8_t>(0x80 | variable_count));
    } else {
        output.push_back(0xde);
        output.push_back(static_cast<uint8_t>(variable_count >> 8));
        output.push_back(static_cast<uint8_t>(variable_count));
    }

    for (std::size_t start = 0; start <= fields.size();) {
        const std::size_t end = fields.find('|', start);
        const std::size_t length = (end == std::string::npos ? fields.size() : end) - start;
        const std::string field = fields.substr(start, length);
        const std::size_t equals = field.find('=');
        if (equals != std::string::npos && field.find('=', equals + 1) == std::string::npos) {
            append_msgpack_string(output, "var_" + field.substr(0, equals));
            append_msgpack_string(output, field.substr(equals + 1));
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return output;
}

class ResponseBuffer {
public:
    static constexpr std::size_t MAX_BYTES = 64 * 1024;

    bool assign(const uint8_t* data, std::size_t size) {
        clear();
        if ((!data && size != 0) || size > MAX_BYTES) return false;
        if (size != 0) _bytes.assign(data, data + size);
        return true;
    }
    const ExternalVector<uint8_t>& bytes() const { return _bytes; }
    std::size_t size() const { return _bytes.size(); }
    std::size_t capacity() const { return _bytes.capacity(); }
    bool truncated() const { return false; }
    void clear() { _bytes.clear(); }
    void release() { ExternalVector<uint8_t>().swap(_bytes); }

private:
    ExternalVector<uint8_t> _bytes;
};

inline bool normalize_response(const uint8_t* data, std::size_t size, ResponseBuffer& output) {
    output.clear();
    if (!data || size == 0) return false;

    std::size_t offset = 0;
    uint64_t payload = 0;
    if (data[0] == 0xc4) {
        if (size < 2) return false;
        offset = 2;
        payload = data[1];
    } else if (data[0] == 0xc5) {
        if (size < 3) return false;
        offset = 3;
        payload = (static_cast<uint64_t>(data[1]) << 8) | data[2];
    } else if (data[0] == 0xc6) {
        if (size < 5) return false;
        offset = 5;
        payload = (static_cast<uint64_t>(data[1]) << 24) |
                  (static_cast<uint64_t>(data[2]) << 16) |
                  (static_cast<uint64_t>(data[3]) << 8) | data[4];
    } else {
        // Pinned microReticulum 6ac0d32 exposes packet responses as decoded
        // raw bytes, while Resource responses retain the encoded MessagePack
        // value. Valid UTF-8 Micron cannot begin with the bin markers above,
        // so the representations are unambiguous here.
        return output.assign(data, size);
    }

    if (payload > ResponseBuffer::MAX_BYTES) return false;
    if (payload != static_cast<uint64_t>(size - offset)) return false;
    return output.assign(data + offset, static_cast<std::size_t>(payload));
}

} // namespace UI::LXMF::NomadNet
