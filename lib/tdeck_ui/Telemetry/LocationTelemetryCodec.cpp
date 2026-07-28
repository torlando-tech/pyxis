#include "LocationTelemetryCodec.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace Telemetry {
namespace {

constexpr std::size_t MAX_MAP_ENTRIES = 32;
constexpr std::size_t MAX_LOCATION_ELEMENTS = 16;
constexpr std::size_t MAX_SKIP_DEPTH = 8;
constexpr std::size_t MAX_SKIP_ITEMS = 64;
constexpr std::size_t MAX_ENCODED_TELEMETRY = 96;

class Cursor {
public:
    Cursor(const uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool readByte(uint8_t& value) {
        if (position_ >= size_) return false;
        value = data_[position_++];
        return true;
    }

    bool readBytes(std::size_t count, const uint8_t*& value) {
        if (count > remaining()) return false;
        value = data_ + position_;
        position_ += count;
        return true;
    }

    bool readUnsigned(uint64_t& value) {
        uint8_t marker = 0;
        if (!readByte(marker)) return false;
        if (marker <= 0x7fU) {
            value = marker;
            return true;
        }

        std::size_t width = 0;
        bool signed_value = false;
        switch (marker) {
            case 0xcc: width = 1; break;
            case 0xcd: width = 2; break;
            case 0xce: width = 4; break;
            case 0xcf: width = 8; break;
            case 0xd0: width = 1; signed_value = true; break;
            case 0xd1: width = 2; signed_value = true; break;
            case 0xd2: width = 4; signed_value = true; break;
            case 0xd3: width = 8; signed_value = true; break;
            default: return false;
        }

        const uint8_t* bytes = nullptr;
        if (!readBytes(width, bytes)) return false;
        uint64_t decoded = 0;
        for (std::size_t index = 0; index < width; ++index) {
            decoded = (decoded << 8U) | bytes[index];
        }
        if (signed_value && (bytes[0] & 0x80U) != 0) return false;
        value = decoded;
        return true;
    }

    bool readMapSize(std::size_t& count) {
        uint8_t marker = 0;
        if (!readByte(marker)) return false;
        if ((marker & 0xf0U) == 0x80U) {
            count = marker & 0x0fU;
            return true;
        }
        return readSizedContainer(marker, 0xdeU, 0xdfU, count);
    }

    bool readArraySize(std::size_t& count) {
        uint8_t marker = 0;
        if (!readByte(marker)) return false;
        if ((marker & 0xf0U) == 0x90U) {
            count = marker & 0x0fU;
            return true;
        }
        return readSizedContainer(marker, 0xdcU, 0xddU, count);
    }

    bool readBinary(BinaryView& value) {
        uint8_t marker = 0;
        if (!readByte(marker)) return false;

        std::size_t length = 0;
        if (marker == 0xc4U) {
            uint8_t byte_length = 0;
            if (!readByte(byte_length)) return false;
            length = byte_length;
        } else if (marker == 0xc5U) {
            if (!readBigEndianSize(2, length)) return false;
        } else if (marker == 0xc6U) {
            if (!readBigEndianSize(4, length)) return false;
        } else {
            return false;
        }

        const uint8_t* bytes = nullptr;
        if (!readBytes(length, bytes)) return false;
        value = BinaryView{bytes, length};
        return true;
    }

    bool skipValue(std::size_t depth, std::size_t& budget) {
        if (depth > MAX_SKIP_DEPTH || budget == 0) return false;
        --budget;

        uint8_t marker = 0;
        if (!peekByte(marker)) return false;

        if (marker <= 0x7fU || marker >= 0xe0U || marker == 0xc0U ||
            marker == 0xc2U || marker == 0xc3U) {
            ++position_;
            return true;
        }
        if ((marker & 0xe0U) == 0xa0U) {
            ++position_;
            return skipBytes(marker & 0x1fU);
        }
        if ((marker & 0xf0U) == 0x90U) {
            ++position_;
            return skipChildren(marker & 0x0fU, depth, budget, false);
        }
        if ((marker & 0xf0U) == 0x80U) {
            ++position_;
            return skipChildren(marker & 0x0fU, depth, budget, true);
        }

        ++position_;
        switch (marker) {
            case 0xc4: return skipLengthPrefixed(1, 0);
            case 0xc5: return skipLengthPrefixed(2, 0);
            case 0xc6: return skipLengthPrefixed(4, 0);
            case 0xca: return skipBytes(4);
            case 0xcb: return skipBytes(8);
            case 0xcc: case 0xd0: return skipBytes(1);
            case 0xcd: case 0xd1: return skipBytes(2);
            case 0xce: case 0xd2: return skipBytes(4);
            case 0xcf: case 0xd3: return skipBytes(8);
            case 0xd4: return skipBytes(2);   // type + 1-byte payload
            case 0xd5: return skipBytes(3);   // type + 2-byte payload
            case 0xd6: return skipBytes(5);   // type + 4-byte payload
            case 0xd7: return skipBytes(9);   // type + 8-byte payload
            case 0xd8: return skipBytes(17);  // type + 16-byte payload
            case 0xd9: return skipLengthPrefixed(1, 0);
            case 0xda: return skipLengthPrefixed(2, 0);
            case 0xdb: return skipLengthPrefixed(4, 0);
            case 0xc7: return skipLengthPrefixed(1, 1);
            case 0xc8: return skipLengthPrefixed(2, 1);
            case 0xc9: return skipLengthPrefixed(4, 1);
            case 0xdc:
            case 0xdd: {
                std::size_t count = 0;
                if (!readBigEndianSize(marker == 0xdcU ? 2 : 4, count)) return false;
                return skipChildren(count, depth, budget, false);
            }
            case 0xde:
            case 0xdf: {
                std::size_t count = 0;
                if (!readBigEndianSize(marker == 0xdeU ? 2 : 4, count)) return false;
                return skipChildren(count, depth, budget, true);
            }
            default: return false;
        }
    }

    bool atEnd() const { return position_ == size_; }

private:
    bool peekByte(uint8_t& value) const {
        if (position_ >= size_) return false;
        value = data_[position_];
        return true;
    }

    std::size_t remaining() const { return size_ - position_; }

    bool skipBytes(std::size_t count) {
        if (count > remaining()) return false;
        position_ += count;
        return true;
    }

    bool readBigEndianSize(std::size_t width, std::size_t& value) {
        const uint8_t* bytes = nullptr;
        if (!readBytes(width, bytes)) return false;
        uint64_t decoded = 0;
        for (std::size_t index = 0; index < width; ++index) {
            decoded = (decoded << 8U) | bytes[index];
        }
        if (decoded > std::numeric_limits<std::size_t>::max()) return false;
        value = static_cast<std::size_t>(decoded);
        return true;
    }

    bool readSizedContainer(uint8_t marker, uint8_t marker16, uint8_t marker32,
                            std::size_t& count) {
        if (marker == marker16) return readBigEndianSize(2, count);
        if (marker == marker32) return readBigEndianSize(4, count);
        return false;
    }

    bool skipLengthPrefixed(std::size_t width, std::size_t suffix) {
        std::size_t length = 0;
        if (!readBigEndianSize(width, length)) return false;
        if (length > std::numeric_limits<std::size_t>::max() - suffix) return false;
        return skipBytes(length + suffix);
    }

    bool skipChildren(std::size_t count, std::size_t depth, std::size_t& budget,
                      bool map) {
        if (map) {
            if (count > MAX_SKIP_ITEMS / 2) return false;
            count *= 2;
        }
        if (count > budget) return false;
        for (std::size_t index = 0; index < count; ++index) {
            if (!skipValue(depth + 1, budget)) return false;
        }
        return true;
    }

    const uint8_t* data_;
    std::size_t size_;
    std::size_t position_ = 0;
};

class Writer {
public:
    Writer(uint8_t* data, std::size_t capacity) : data_(data), capacity_(capacity) {}

    bool writeByte(uint8_t value) {
        if (size_ >= capacity_) return false;
        data_[size_++] = value;
        return true;
    }

    bool writeBytes(const uint8_t* data, std::size_t size) {
        if (size > capacity_ - size_) return false;
        std::memcpy(data_ + size_, data, size);
        size_ += size;
        return true;
    }

    bool writeUnsigned(uint64_t value) {
        if (value <= 0x7fU) return writeByte(static_cast<uint8_t>(value));
        if (value <= 0xffU) {
            return writeByte(0xccU) && writeBigEndian(value, 1);
        }
        if (value <= 0xffffU) {
            return writeByte(0xcdU) && writeBigEndian(value, 2);
        }
        if (value <= 0xffffffffULL) {
            return writeByte(0xceU) && writeBigEndian(value, 4);
        }
        return writeByte(0xcfU) && writeBigEndian(value, 8);
    }

    bool writeBinary(const uint8_t* data, std::size_t size) {
        if (size > 0xffU) return false;
        return writeByte(0xc4U) && writeByte(static_cast<uint8_t>(size)) &&
               writeBytes(data, size);
    }

    std::size_t size() const { return size_; }

private:
    bool writeBigEndian(uint64_t value, std::size_t width) {
        for (std::size_t index = width; index > 0; --index) {
            if (!writeByte(static_cast<uint8_t>(value >> ((index - 1) * 8U)))) {
                return false;
            }
        }
        return true;
    }

    uint8_t* data_;
    std::size_t capacity_;
    std::size_t size_ = 0;
};

uint32_t decodeU32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24U) |
           (static_cast<uint32_t>(bytes[1]) << 16U) |
           (static_cast<uint32_t>(bytes[2]) << 8U) |
           static_cast<uint32_t>(bytes[3]);
}

int32_t decodeI32(const uint8_t* bytes) {
    const uint32_t raw = decodeU32(bytes);
    if (raw <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return static_cast<int32_t>(raw);
    }
    // Convert two's-complement wire bits without relying on an
    // implementation-defined uint32_t -> int32_t narrowing conversion.
    const uint32_t distance_from_minus_one =
        std::numeric_limits<uint32_t>::max() - raw;
    return -1 - static_cast<int32_t>(distance_from_minus_one);
}

void encodeU32(uint32_t value, uint8_t bytes[4]) {
    bytes[0] = static_cast<uint8_t>(value >> 24U);
    bytes[1] = static_cast<uint8_t>(value >> 16U);
    bytes[2] = static_cast<uint8_t>(value >> 8U);
    bytes[3] = static_cast<uint8_t>(value);
}

bool readFixedBinary(Cursor& cursor, std::size_t expected, BinaryView& value) {
    return cursor.readBinary(value) && value.size == expected;
}

bool readLocation(Cursor& cursor, LocationTelemetry& location) {
    std::size_t count = 0;
    if (!cursor.readArraySize(count) || count < 7 ||
        count > MAX_LOCATION_ELEMENTS) {
        return false;
    }

    BinaryView value{};
    uint64_t timestamp = 0;

    if (!readFixedBinary(cursor, 4, value)) return false;
    location.latitude_e6 = decodeI32(value.data);
    if (!readFixedBinary(cursor, 4, value)) return false;
    location.longitude_e6 = decodeI32(value.data);
    if (!readFixedBinary(cursor, 4, value)) return false;
    location.altitude_cm = decodeI32(value.data);
    if (!readFixedBinary(cursor, 4, value)) return false;
    location.speed_centi_kmh = decodeU32(value.data);
    if (!readFixedBinary(cursor, 4, value)) return false;
    location.bearing_cdeg = decodeI32(value.data);
    if (!readFixedBinary(cursor, 2, value)) return false;
    location.accuracy_cm = static_cast<uint16_t>(
        (static_cast<uint16_t>(value.data[0]) << 8U) | value.data[1]);
    if (!cursor.readUnsigned(timestamp)) return false;
    location.timestamp_seconds = timestamp;

    std::size_t budget = MAX_SKIP_ITEMS;
    for (std::size_t index = 7; index < count; ++index) {
        if (!cursor.skipValue(0, budget)) return false;
    }
    return true;
}

bool locationInRange(const LocationTelemetry& location) {
    return location.latitude_e6 >= -90000000 &&
           location.latitude_e6 <= 90000000 &&
           location.longitude_e6 >= -180000000 &&
           location.longitude_e6 <= 180000000;
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
    if (marker != 0xc4U && marker != 0xc5U && marker != 0xc6U) {
        return FieldValueResult::NOT_BINARY;
    }

    Cursor cursor(raw_value, raw_size);
    BinaryView candidate{};
    if (!cursor.readBinary(candidate)) return FieldValueResult::MALFORMED;
    if (!cursor.atEnd()) return FieldValueResult::MALFORMED;
    output = candidate;
    return FieldValueResult::OK;
}

FieldValueResult wrapLxmfBinaryFieldValue(
    const uint8_t* payload,
    std::size_t payload_size,
    uint8_t* output,
    std::size_t capacity,
    std::size_t& written) {
    if ((payload == nullptr && payload_size != 0) || output == nullptr) {
        return FieldValueResult::INVALID_ARGUMENT;
    }

    std::size_t header_size = 0;
    if (payload_size <= 0xffU) {
        header_size = 2;
    } else if (payload_size <= 0xffffU) {
        header_size = 3;
    } else if (payload_size <= 0xffffffffULL) {
        header_size = 5;
    } else {
        return FieldValueResult::INVALID_ARGUMENT;
    }
    if (payload_size > std::numeric_limits<std::size_t>::max() - header_size ||
        capacity < header_size + payload_size) {
        return FieldValueResult::BUFFER_TOO_SMALL;
    }

    if (header_size == 2) {
        output[0] = 0xc4U;
        output[1] = static_cast<uint8_t>(payload_size);
    } else if (header_size == 3) {
        output[0] = 0xc5U;
        output[1] = static_cast<uint8_t>(payload_size >> 8U);
        output[2] = static_cast<uint8_t>(payload_size);
    } else {
        output[0] = 0xc6U;
        output[1] = static_cast<uint8_t>(payload_size >> 24U);
        output[2] = static_cast<uint8_t>(payload_size >> 16U);
        output[3] = static_cast<uint8_t>(payload_size >> 8U);
        output[4] = static_cast<uint8_t>(payload_size);
    }
    if (payload_size != 0) std::memcpy(output + header_size, payload, payload_size);
    written = header_size + payload_size;
    return FieldValueResult::OK;
}

DecodeResult decodeLocationTelemetry(
    const uint8_t* data,
    std::size_t size,
    LocationTelemetry& output) {
    if (data == nullptr || size == 0) return DecodeResult::INVALID_ARGUMENT;

    Cursor cursor(data, size);
    std::size_t map_size = 0;
    if (!cursor.readMapSize(map_size) || map_size > MAX_MAP_ENTRIES) {
        return DecodeResult::MALFORMED;
    }

    LocationTelemetry candidate{};
    bool has_location = false;
    std::size_t skip_budget = MAX_SKIP_ITEMS;
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
        } else if (!cursor.skipValue(0, skip_budget)) {
            return DecodeResult::MALFORMED;
        }
    }

    if (!cursor.atEnd()) return DecodeResult::MALFORMED;
    if (!has_location) return DecodeResult::MISSING_LOCATION;
    if (!locationInRange(candidate)) return DecodeResult::OUT_OF_RANGE;
    output = candidate;
    return DecodeResult::OK;
}

EncodeResult encodeLocationTelemetry(
    const LocationTelemetry& input,
    uint8_t* output,
    std::size_t capacity,
    std::size_t& written) {
    if (output == nullptr) return EncodeResult::INVALID_ARGUMENT;
    if (!locationInRange(input)) return EncodeResult::OUT_OF_RANGE;

    uint8_t temporary[MAX_ENCODED_TELEMETRY]{};
    Writer writer(temporary, sizeof(temporary));
    uint8_t word[4]{};
    uint8_t half[2]{};
    const uint64_t sensor_timestamp = input.sensor_timestamp_seconds == 0
                                          ? input.timestamp_seconds
                                          : input.sensor_timestamp_seconds;

    bool ok = writer.writeByte(0x82U) &&
              writer.writeUnsigned(SID_TIME) &&
              writer.writeUnsigned(sensor_timestamp) &&
              writer.writeUnsigned(SID_LOCATION) &&
              writer.writeByte(0x97U);

    encodeU32(static_cast<uint32_t>(input.latitude_e6), word);
    ok = ok && writer.writeBinary(word, sizeof(word));
    encodeU32(static_cast<uint32_t>(input.longitude_e6), word);
    ok = ok && writer.writeBinary(word, sizeof(word));
    encodeU32(static_cast<uint32_t>(input.altitude_cm), word);
    ok = ok && writer.writeBinary(word, sizeof(word));
    encodeU32(input.speed_centi_kmh, word);
    ok = ok && writer.writeBinary(word, sizeof(word));
    encodeU32(static_cast<uint32_t>(input.bearing_cdeg), word);
    ok = ok && writer.writeBinary(word, sizeof(word));
    half[0] = static_cast<uint8_t>(input.accuracy_cm >> 8U);
    half[1] = static_cast<uint8_t>(input.accuracy_cm);
    ok = ok && writer.writeBinary(half, sizeof(half)) &&
         writer.writeUnsigned(input.timestamp_seconds);

    if (!ok) return EncodeResult::INVALID_ARGUMENT;
    if (capacity < writer.size()) return EncodeResult::BUFFER_TOO_SMALL;
    std::memcpy(output, temporary, writer.size());
    written = writer.size();
    return EncodeResult::OK;
}

}  // namespace Telemetry
