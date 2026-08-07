// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_MAP_PACK_MANIFEST_H
#define UI_LXMF_MAP_PACK_MANIFEST_H

#include <cstddef>
#include <cstdint>

#include "Hardware/TDeck/MapTileStore.h"

namespace Pyxis {

enum class ManifestResult : std::uint8_t {
    OK = 0,
    BAD_MAGIC,
    UNSUPPORTED_VERSION,
    BAD_HEADER,
    BAD_LENGTH,
    BAD_CRC,
    INVALID_STRING,
    INVALID_ZOOM,
    INVALID_EXTENT,
    INVALID_TILE_COUNT,
    INSUFFICIENT_CAPACITY
};

struct XInterval {
    std::uint32_t minimum;
    std::uint32_t maximum;
};

struct ZoomExtent {
    std::uint8_t zoom;
    std::uint8_t interval_count;
    XInterval x[2];
    std::uint32_t y_minimum;
    std::uint32_t y_maximum;
};

struct RowSpan {
    std::uint8_t zoom;
    std::uint32_t y;
    std::uint32_t x_minimum;
    std::uint32_t x_maximum;
};

/**
 * Portable, allocation-free description of one complete rectangular extent at
 * every declared XYZ zoom. Two canonical X intervals describe a rectangle that
 * crosses the antimeridian: the first starts at X=0 and the second ends at the
 * world's maximum X.
 *
 * The wire format uses little-endian integers, a 16-byte versioned header, and
 * an IEEE CRC-32 stored in the final four bytes. It never serializes compiler
 * padding or native enum representations.
 */
struct MapPackManifest {
    static const std::uint8_t LEGACY_FORMAT_VERSION = 1U;
    static const std::uint8_t FORMAT_VERSION = 2U;
    static const std::uint8_t MAX_ZOOM = 22U;
    static const std::size_t MAX_ZOOM_LEVELS = 23U;
    static const std::size_t PACK_ID_CAPACITY = 32U;
    static const std::size_t NAME_CAPACITY = 64U;
    static const std::size_t ATTRIBUTION_CAPACITY = 128U;
    static const std::size_t SOURCE_CAPACITY = 128U;
    static const std::size_t LICENSE_CAPACITY = 64U;
    static const std::size_t MAX_ROW_SPANS = 512U;
    static const std::size_t MAX_SERIALIZED_SIZE = 7100U;

    char pack_id[PACK_ID_CAPACITY];
    char name[NAME_CAPACITY];
    char attribution[ATTRIBUTION_CAPACITY];
    char source[SOURCE_CAPACITY];
    char license[LICENSE_CAPACITY];
    std::uint8_t min_zoom;
    std::uint8_t max_zoom;
    std::uint8_t extent_count;
    std::uint32_t tile_count;
    ZoomExtent extents[MAX_ZOOM_LEVELS];
    std::uint8_t format_version;
    std::uint16_t row_span_count;
    // Version-two manifests keep a validated view into the parse input. The
    // input buffer must outlive this object (MapTilePack owns that buffer).
    const std::uint8_t* row_span_bytes;

    static ManifestResult serialize(const MapPackManifest& manifest,
                                    std::uint8_t* output,
                                    std::size_t capacity,
                                    std::size_t& written);
    static ManifestResult serializeSparse(const MapPackManifest& manifest,
                                          const RowSpan* spans,
                                          std::size_t span_count,
                                          std::uint8_t* output,
                                          std::size_t capacity,
                                          std::size_t& written);
    static ManifestResult parse(const std::uint8_t* input,
                                std::size_t length,
                                MapPackManifest& output);
    bool covers(const Hardware::TDeck::TileKey& key) const;
};

}  // namespace Pyxis

#endif
