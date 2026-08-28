// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#include "Hardware/TDeck/ActiveMapSetCodec.h"

#include <cstring>

namespace Hardware {
namespace TDeck {
namespace {

bool readString(const std::uint8_t* input, std::size_t end, std::size_t& position,
                char* output, std::size_t capacity, bool identifier) {
    if (position >= end || capacity < 2U) return false;
    const std::size_t length = input[position++];
    if (length == 0U || length >= capacity || length > end - position) return false;
    for (std::size_t index = 0U; index < length; ++index) {
        const std::uint8_t value = input[position + index];
        if (value < 0x20U || value > 0x7eU) return false;
        if (identifier && !((value >= static_cast<std::uint8_t>('a') &&
                             value <= static_cast<std::uint8_t>('z')) ||
                            (value >= static_cast<std::uint8_t>('0') &&
                             value <= static_cast<std::uint8_t>('9')) ||
                            value == static_cast<std::uint8_t>('_') ||
                            value == static_cast<std::uint8_t>('-'))) return false;
    }
    std::memcpy(output, input + position, length);
    output[length] = '\0';
    position += length;
    return true;
}

}  // namespace

std::uint16_t ActiveMapSetCodec::readU16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(input[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(input[1]) << 8U);
}

std::uint32_t ActiveMapSetCodec::readU32(const std::uint8_t* input) {
    return static_cast<std::uint32_t>(input[0]) |
        (static_cast<std::uint32_t>(input[1]) << 8U) |
        (static_cast<std::uint32_t>(input[2]) << 16U) |
        (static_cast<std::uint32_t>(input[3]) << 24U);
}

void ActiveMapSetCodec::writeU32(std::uint8_t* output, std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
    output[2] = static_cast<std::uint8_t>(value >> 16U);
    output[3] = static_cast<std::uint8_t>(value >> 24U);
}

std::uint32_t ActiveMapSetCodec::crc32(const std::uint8_t* input, std::size_t length) {
    std::uint32_t crc = UINT32_C(0xffffffff);
    for (std::size_t index = 0U; index < length; ++index) {
        crc ^= input[index];
        for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? UINT32_C(0xedb88320) : 0U);
        }
    }
    return ~crc;
}

bool ActiveMapSetCodec::decode(const std::uint8_t* input, std::size_t length,
                               ActiveMapSetView& output) {
    static const std::uint8_t magic[4] = {'P', 'M', 'A', 'S'};
    if (input == NULL || length < 16U || length > MAX_SERIALIZED_SIZE ||
        std::memcmp(input, magic, sizeof(magic)) != 0 ||
        (input[4] != SPAN_FORMAT_VERSION && input[4] != INDEXLESS_FORMAT_VERSION) ||
        input[5] != 0U ||
        readU16(input + 6U) != length || readU32(input + length - 4U) != crc32(input, length - 4U)) {
        return false;
    }

    ActiveMapSetView candidate = {};
    candidate.format_version = input[4];
    candidate.generation = readU32(input + 8U);
    if (candidate.generation == 0U) return false;
    const std::size_t end = length - 4U;
    std::size_t position = 12U;
    if (!readString(input, end, position, candidate.map_set_id,
                    sizeof(candidate.map_set_id), true) ||
        !readString(input, end, position, candidate.attribution,
                    sizeof(candidate.attribution), false) || position >= end) return false;
    candidate.pack_count = input[position++];
    if (candidate.pack_count == 0U || candidate.pack_count > MAX_PACKS) return false;

    std::uint16_t total_spans = 0U;
    for (std::uint8_t pack_index = 0U; pack_index < candidate.pack_count; ++pack_index) {
        ActiveMapSetPackView& pack = candidate.packs[pack_index];
        if (!readString(input, end, position, pack.pack_id, sizeof(pack.pack_id), true)) return false;
        for (std::uint8_t previous = 0U; previous < pack_index; ++previous) {
            if (std::strcmp(pack.pack_id, candidate.packs[previous].pack_id) == 0) return false;
        }
        if (candidate.format_version == INDEXLESS_FORMAT_VERSION) {
            pack.span_count = 0U;
            pack.span_bytes = NULL;
            continue;
        }
        if (position > end || end - position < 2U) return false;
        pack.span_count = readU16(input + position); position += 2U;
        if (pack.span_count == 0U || pack.span_count > MAX_ROW_SPANS ||
            total_spans > MAX_ROW_SPANS - pack.span_count) return false;
        const std::size_t span_bytes = static_cast<std::size_t>(pack.span_count) * ROW_SPAN_SIZE;
        if (position > end || span_bytes > end - position) return false;
        pack.span_bytes = input + position;

        bool have_previous = false;
        std::uint8_t previous_zoom = 0U;
        std::uint32_t previous_y = 0U;
        std::uint32_t previous_x_maximum = 0U;
        for (std::uint16_t span_index = 0U; span_index < pack.span_count; ++span_index) {
            const std::uint8_t* span = input + position;
            const std::uint8_t zoom = span[0];
            const std::uint32_t y = readU32(span + 1U);
            const std::uint32_t x_minimum = readU32(span + 5U);
            const std::uint32_t x_maximum = readU32(span + 9U);
            if (zoom > Pyxis::MapPackManifest::MAX_ZOOM) return false;
            const std::uint32_t edge = UINT32_C(1) << zoom;
            if (y >= edge || x_minimum > x_maximum || x_maximum >= edge ||
                (have_previous && (zoom < previous_zoom ||
                 (zoom == previous_zoom && y < previous_y) ||
                 (zoom == previous_zoom && y == previous_y &&
                  x_minimum <= previous_x_maximum + 1U)))) return false;
            previous_zoom = zoom; previous_y = y; previous_x_maximum = x_maximum;
            have_previous = true; position += ROW_SPAN_SIZE;
        }
        total_spans = static_cast<std::uint16_t>(total_spans + pack.span_count);
    }
    if (position != end) return false;
    candidate.total_span_count = total_spans;
    output = candidate;
    return true;
}

bool ActiveMapSetCodec::resequence(const std::uint8_t* input, std::size_t length,
                                   std::uint32_t generation, std::uint8_t* output,
                                   std::size_t capacity, std::size_t& written) {
    written = 0U;
    ActiveMapSetView view = {};
    if (generation == 0U || output == NULL || capacity < length ||
        !decode(input, length, view)) return false;
    if (output != input) std::memcpy(output, input, length);
    writeU32(output + 8U, generation);
    writeU32(output + length - 4U, crc32(output, length - 4U));
    written = length;
    return true;
}

}  // namespace TDeck
}  // namespace Hardware
