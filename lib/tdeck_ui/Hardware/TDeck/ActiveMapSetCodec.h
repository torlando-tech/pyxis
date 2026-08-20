// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_ACTIVE_MAP_SET_CODEC_H
#define HARDWARE_TDECK_ACTIVE_MAP_SET_CODEC_H

#include <cstddef>
#include <cstdint>

#include "UI/LXMF/MapPackManifest.h"

namespace Hardware {
namespace TDeck {

struct ActiveMapSetPackView {
    char pack_id[Pyxis::MapPackManifest::PACK_ID_CAPACITY];
    std::uint16_t span_count;
    const std::uint8_t* span_bytes;
};

struct ActiveMapSetView {
    std::uint8_t format_version;
    std::uint32_t generation;
    char map_set_id[Pyxis::MapPackManifest::PACK_ID_CAPACITY];
    char attribution[Pyxis::MapPackManifest::ATTRIBUTION_CAPACITY];
    std::uint8_t pack_count;
    std::uint16_t total_span_count;
    ActiveMapSetPackView packs[8];
};

/** Strict, allocation-free codec for span-indexed v2 and indexless v3 PMAS records. */
class ActiveMapSetCodec {
public:
    static const std::uint8_t SPAN_FORMAT_VERSION = 2U;
    static const std::uint8_t INDEXLESS_FORMAT_VERSION = 3U;
    static const std::size_t MAX_SERIALIZED_SIZE = 7105U;
    static const std::size_t MAX_PACKS = 8U;
    static const std::size_t MAX_ROW_SPANS = 512U;
    static const std::size_t ROW_SPAN_SIZE = 13U;

    static bool decode(const std::uint8_t* input, std::size_t length, ActiveMapSetView& output);
    static bool resequence(const std::uint8_t* input, std::size_t length,
                           std::uint32_t generation, std::uint8_t* output,
                           std::size_t capacity, std::size_t& written);
    static std::uint32_t crc32(const std::uint8_t* input, std::size_t length);
    static std::uint16_t readU16(const std::uint8_t* input);
    static std::uint32_t readU32(const std::uint8_t* input);

private:
    static void writeU32(std::uint8_t* output, std::uint32_t value);
};

}  // namespace TDeck
}  // namespace Hardware

#endif
