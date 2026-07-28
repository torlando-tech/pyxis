#include "LocationTelemetryCodec.h"

#include <cstddef>
#include <cstdint>

namespace Telemetry {
namespace {

class Cursor {
public:
    Cursor(const uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool readByte(uint8_t& value) {
        if (position_ >= size_) return false;
        value = data_[position_++];
        return true;
    }

    bool readUnsigned(uint64_t& value) {
        uint8_t marker = 0;
        if (!readByte(marker)) return false;
        if (marker <= 0x7f) {
            value = marker;
            return true;
        }

        std::size_t width = 0;
        switch (marker) {
            case 0xcc: width = 1; break;
            case 0xcd: width = 2; break;
            case 0xce: width = 4; break;
            case 0xcf: width = 8; break;
            default: return false;
        }
        if (size_ - position_ < width) return false;

        value = 0;
        for (std::size_t index = 0; index < width; ++index) {
            value = (value << 8U) | data_[position_++];
        }
        return true;
    }

    bool readMapSize(std::size_t& count) {
        uint8_t marker = 0;
        if (!readByte(marker) || (marker & 0xf0U) != 0x80U) return false;
        count = marker & 0x0fU;
        return true;
    }

    bool readArraySize(std::size_t& count) {
        uint8_t marker = 0;
        if (!readByte(marker) || (marker & 0xf0U) != 0x90U) return false;
        count = marker & 0x0fU;
        return true;
    }

    bool readBinary(uint8_t* destination, std::size_t required) {
        uint8_t marker = 0;
        uint8_t encoded_size = 0;
        if (!readByte(marker) || marker != 0xc4U || !readByte(encoded_size)) return false;
        if (encoded_size != required || size_ - position_ < required) return false;
        for (std::size_t index = 0; index < required; ++index) {
            destination[index] = data_[position_++];
        }
        return true;
    }

    bool atEnd() const { return position_ == size_; }

private:
    const uint8_t* data_;
    std::size_t size_;
    std::size_t position_ = 0;
};

uint32_t decodeU32(const uint8_t bytes[4]) {
    return (static_cast<uint32_t>(bytes[0]) << 24U) |
           (static_cast<uint32_t>(bytes[1]) << 16U) |
           (static_cast<uint32_t>(bytes[2]) << 8U) |
           static_cast<uint32_t>(bytes[3]);
}

int32_t decodeI32(const uint8_t bytes[4]) {
    return static_cast<int32_t>(decodeU32(bytes));
}

bool readLocation(Cursor& cursor, LocationTelemetry& location) {
    std::size_t count = 0;
    if (!cursor.readArraySize(count) || count != 7) return false;

    uint8_t word[4]{};
    uint8_t half[2]{};
    uint64_t timestamp = 0;

    if (!cursor.readBinary(word, sizeof(word))) return false;
    location.latitude_e6 = decodeI32(word);
    if (!cursor.readBinary(word, sizeof(word))) return false;
    location.longitude_e6 = decodeI32(word);
    if (!cursor.readBinary(word, sizeof(word))) return false;
    location.altitude_cm = decodeI32(word);
    if (!cursor.readBinary(word, sizeof(word))) return false;
    location.speed_centi_kmh = decodeU32(word);
    if (!cursor.readBinary(word, sizeof(word))) return false;
    location.bearing_cdeg = decodeI32(word);
    if (!cursor.readBinary(half, sizeof(half))) return false;
    location.accuracy_cm = static_cast<uint16_t>(
        (static_cast<uint16_t>(half[0]) << 8U) | half[1]);
    if (!cursor.readUnsigned(timestamp)) return false;
    location.timestamp_seconds = timestamp;
    return true;
}

}  // namespace

FieldValueResult unwrapLxmfBinaryFieldValue(
    const uint8_t* raw_value,
    std::size_t raw_size,
    BinaryView& output) {
    if (raw_value == nullptr || raw_size == 0) {
        return FieldValueResult::INVALID_ARGUMENT;
    }

    const uint8_t marker = raw_value[0];
    std::size_t header_size = 0;
    std::size_t payload_size = 0;
    if (marker == 0xc4U) {
        if (raw_size < 2) return FieldValueResult::MALFORMED;
        header_size = 2;
        payload_size = raw_value[1];
    } else if (marker == 0xc5U) {
        if (raw_size < 3) return FieldValueResult::MALFORMED;
        header_size = 3;
        payload_size = (static_cast<std::size_t>(raw_value[1]) << 8U) |
                       raw_value[2];
    } else if (marker == 0xc6U) {
        if (raw_size < 5) return FieldValueResult::MALFORMED;
        header_size = 5;
        payload_size = (static_cast<std::size_t>(raw_value[1]) << 24U) |
                       (static_cast<std::size_t>(raw_value[2]) << 16U) |
                       (static_cast<std::size_t>(raw_value[3]) << 8U) |
                       raw_value[4];
    } else {
        return FieldValueResult::NOT_BINARY;
    }

    if (payload_size != raw_size - header_size) {
        return FieldValueResult::MALFORMED;
    }

    BinaryView candidate{raw_value + header_size, payload_size};
    output = candidate;
    return FieldValueResult::OK;
}

DecodeResult decodeLocationTelemetry(
    const uint8_t* data,
    std::size_t size,
    LocationTelemetry& output) {
    if (data == nullptr || size == 0) return DecodeResult::INVALID_ARGUMENT;

    Cursor cursor(data, size);
    std::size_t map_size = 0;
    if (!cursor.readMapSize(map_size)) return DecodeResult::MALFORMED;

    LocationTelemetry candidate{};
    bool has_location = false;
    for (std::size_t index = 0; index < map_size; ++index) {
        uint64_t key = 0;
        if (!cursor.readUnsigned(key)) return DecodeResult::MALFORMED;
        if (key == SID_TIME) {
            if (!cursor.readUnsigned(candidate.sensor_timestamp_seconds)) {
                return DecodeResult::MALFORMED;
            }
        } else if (key == SID_LOCATION) {
            if (has_location || !readLocation(cursor, candidate)) {
                return DecodeResult::MALFORMED;
            }
            has_location = true;
        } else {
            return DecodeResult::MALFORMED;
        }
    }

    if (!cursor.atEnd()) return DecodeResult::MALFORMED;
    if (!has_location) return DecodeResult::MISSING_LOCATION;
    output = candidate;
    return DecodeResult::OK;
}

}  // namespace Telemetry
