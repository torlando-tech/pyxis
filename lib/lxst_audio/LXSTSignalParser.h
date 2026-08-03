#pragma once

#include <cstddef>
#include <cstdint>
#include <climits>

// Bounded MessagePack parser for LXST {FIELD_SIGNALLING: [signal, ...]} packets.
// Current LXST sends profile and duplex mode together, while older releases sent
// one signal per packet. Accept canonical and valid wider integer containers.
struct LXSTSignalParser {
    static constexpr size_t MAX_SIGNALS = 8;

    static size_t parse(const uint8_t* data, size_t length, int* output,
                        size_t output_capacity) {
        if (!data || !output || output_capacity == 0) return 0;

        size_t position = 0;
        uint32_t map_count = 0;
        if (!readContainerCount(data, length, position, 0x80, 0x8F,
                                0xDE, 0xDF, map_count) ||
            map_count != 1) {
            return 0;
        }

        uint64_t field = 0;
        if (!readNonNegativeInteger(data, length, position, field) || field != 0) {
            return 0;
        }

        uint32_t signal_count = 0;
        if (!readContainerCount(data, length, position, 0x90, 0x9F,
                                0xDC, 0xDD, signal_count) ||
            signal_count == 0 || signal_count > MAX_SIGNALS ||
            signal_count > output_capacity) {
            return 0;
        }

        int parsed[MAX_SIGNALS] = {};
        for (uint32_t index = 0; index < signal_count; ++index) {
            uint64_t value = 0;
            if (!readNonNegativeInteger(data, length, position, value) ||
                value > static_cast<uint64_t>(INT_MAX)) {
                return 0;
            }
            parsed[index] = static_cast<int>(value);
        }

        if (position != length) return 0;
        for (uint32_t index = 0; index < signal_count; ++index) {
            output[index] = parsed[index];
        }
        return signal_count;
    }

private:
    static bool take(const uint8_t* data, size_t length, size_t& position,
                     uint8_t& value) {
        if (position >= length) return false;
        value = data[position++];
        return true;
    }

    static bool readBigEndian(const uint8_t* data, size_t length,
                              size_t& position, size_t bytes,
                              uint64_t& value) {
        if (bytes > length - position) return false;
        value = 0;
        for (size_t index = 0; index < bytes; ++index) {
            value = (value << 8) | data[position++];
        }
        return true;
    }

    static bool readContainerCount(const uint8_t* data, size_t length,
                                   size_t& position, uint8_t fixed_min,
                                   uint8_t fixed_max, uint8_t count16_marker,
                                   uint8_t count32_marker, uint32_t& count) {
        uint8_t marker = 0;
        if (!take(data, length, position, marker)) return false;
        if (marker >= fixed_min && marker <= fixed_max) {
            count = marker & 0x0F;
            return true;
        }

        uint64_t wide_count = 0;
        if (marker == count16_marker) {
            if (!readBigEndian(data, length, position, 2, wide_count)) return false;
        } else if (marker == count32_marker) {
            if (!readBigEndian(data, length, position, 4, wide_count)) return false;
        } else {
            return false;
        }
        if (wide_count > UINT32_MAX) return false;
        count = static_cast<uint32_t>(wide_count);
        return true;
    }

    static bool readNonNegativeInteger(const uint8_t* data, size_t length,
                                       size_t& position, uint64_t& value) {
        uint8_t marker = 0;
        if (!take(data, length, position, marker)) return false;
        if (marker <= 0x7F) {
            value = marker;
            return true;
        }

        size_t bytes = 0;
        bool signed_integer = false;
        switch (marker) {
            case 0xCC: bytes = 1; break;
            case 0xCD: bytes = 2; break;
            case 0xCE: bytes = 4; break;
            case 0xCF: bytes = 8; break;
            case 0xD0: bytes = 1; signed_integer = true; break;
            case 0xD1: bytes = 2; signed_integer = true; break;
            case 0xD2: bytes = 4; signed_integer = true; break;
            case 0xD3: bytes = 8; signed_integer = true; break;
            default: return false;
        }
        if (!readBigEndian(data, length, position, bytes, value)) return false;
        if (signed_integer) {
            const uint64_t sign_bit = bytes == 8
                ? (UINT64_MAX / 2 + 1)
                : (uint64_t{1} << (bytes * 8 - 1));
            if ((value & sign_bit) != 0) return false;
        }
        return true;
    }
};
