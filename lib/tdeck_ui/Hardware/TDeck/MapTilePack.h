// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_MAP_TILE_PACK_H
#define HARDWARE_TDECK_MAP_TILE_PACK_H

#include <cstddef>
#include <cstdint>

#include "Hardware/TDeck/ActiveMapSetCodec.h"
#include "Hardware/TDeck/MapTileStore.h"
#include "UI/LXMF/MapPackManifest.h"

namespace Hardware {
namespace TDeck {

enum class MapTilePackResult : std::uint8_t {
    OK = 0,
    NO_SELECTION,
    INVALID_PACK_ID,
    STORAGE_UNAVAILABLE,
    INSUFFICIENT_MEMORY,
    MANIFEST_MISSING,
    MANIFEST_TOO_LARGE,
    INVALID_MANIFEST,
    PACK_ID_MISMATCH,
    NOT_INITIALIZED,
    INVALID_KEY,
    UNCOVERED,
    TILE_MISSING,
    PATH_TOO_LONG,
    INVALID_ARGUMENT,
    IO_ERROR,
    BUSY,
    NOT_STREAMING
};

enum class MapTilePackStatus : std::uint8_t {
    UNINITIALIZED = 0,
    NO_SELECTION,
    READY,
    INVALID_SELECTION,
    STORAGE_UNAVAILABLE,
    INSUFFICIENT_MEMORY
};

/**
 * Bounded, read-only access to an enabled map set. Production selection
 * buffers are allocated once in PSRAM; tile lookup itself is allocation-free.
 *
 * initialize() reads the bounded active marker and manifest transactionally:
 * an already usable selection remains active if a replacement is malformed or
 * unreadable. Only one tile stream may be open. Every read failure, explicit
 * end, reinitialize, and destruction closes that stream.
 */
class MapTilePack {
public:
    static const char ACTIVE_PACK_PATH[];
    static const char ACTIVE_PACK_SLOT_0_PATH[];
    static const char ACTIVE_PACK_SLOT_1_PATH[];
    static const std::size_t LEGACY_ACTIVE_SELECTION_SIZE = 48U;
    static const std::size_t ACTIVE_SELECTION_SIZE = ActiveMapSetCodec::MAX_SERIALIZED_SIZE;
    static const std::size_t MAX_ACTIVE_PACKS = ActiveMapSetCodec::MAX_PACKS;
    static const std::size_t MAX_ACTIVE_ROW_SPANS = ActiveMapSetCodec::MAX_ROW_SPANS;
    static const std::size_t PATH_CAPACITY = 80U;
    static const std::size_t MANIFEST_BUFFER_CAPACITY = Pyxis::MapPackManifest::MAX_SERIALIZED_SIZE;

    explicit MapTilePack(MapTileStorage& storage);
    ~MapTilePack();

    MapTilePackResult initialize();

    MapTilePackStatus status() const { return status_; }
    bool hasSelection() const { return status_ == MapTilePackStatus::READY; }
    const Pyxis::MapPackManifest& metadata() const { return manifest_; }
    std::uint32_t selectionGeneration() const { return selection_generation_; }

    static bool isValidPackId(const char* pack_id);
    static bool selectionRecordsEqual(const std::uint8_t* first, std::size_t first_length,
                                      const std::uint8_t* second, std::size_t second_length);
    static MapTilePackResult manifestPath(const char* pack_id, char* output, std::size_t capacity);
    static MapTilePackResult tilePath(const char* pack_id, const TileKey& key,
                                      char* output, std::size_t capacity);
    static MapTilePackResult validateMapSet(MapTileStorage& storage,
                                            const ActiveMapSetView& view,
                                            std::uint8_t* scratch,
                                            std::size_t scratch_capacity);

    MapTilePackResult beginGet(const TileKey& key, std::uint32_t& size);
    MapTilePackResult readGetChunk(std::uint8_t* output, std::size_t capacity,
                                   std::size_t& count);
    void endGet();

    static std::size_t ramBytes() {
#if defined(ARDUINO_ARCH_ESP32)
        return sizeof(MapTilePack) + 2U * ACTIVE_SELECTION_SIZE + MANIFEST_BUFFER_CAPACITY;
#else
        return sizeof(MapTilePack);
#endif
    }
    static std::size_t internalRamBytes() { return sizeof(MapTilePack); }
    static std::size_t psramBytes() {
#if defined(ARDUINO_ARCH_ESP32)
        return 2U * ACTIVE_SELECTION_SIZE + MANIFEST_BUFFER_CAPACITY;
#else
        return 0U;
#endif
    }

private:
    struct ActivePackView {
        char pack_id[Pyxis::MapPackManifest::PACK_ID_CAPACITY];
        std::uint16_t span_count;
        const std::uint8_t* span_bytes;
    };

    MapTileStorage& storage_;
    Pyxis::MapPackManifest manifest_;
    ActivePackView active_packs_[MAX_ACTIVE_PACKS];
    std::uint8_t active_pack_count_;
    bool map_set_active_;
    std::uint32_t selection_generation_;
    MapTilePackStatus status_;
    bool stream_open_;
    std::uint32_t stream_remaining_;
#if defined(ARDUINO_ARCH_ESP32)
    std::uint8_t* selection_buffers_;
#else
    std::uint8_t selection_buffers_[2][ACTIVE_SELECTION_SIZE];
    std::uint8_t manifest_buffer_[MANIFEST_BUFFER_CAPACITY];
#endif
    std::uint8_t active_selection_buffer_;

    MapTilePack(const MapTilePack&);
    MapTilePack& operator=(const MapTilePack&);

    MapTilePackResult failInitialize(MapTilePackResult result, MapTilePackStatus initial_status,
                                     bool had_selection);
    MapTilePackResult loadFile(const char* path, std::uint8_t* output, std::size_t capacity,
                               std::size_t& length, MapTilePackResult missing_result,
                               MapTilePackResult oversized_result);
    static bool isValidKey(const TileKey& key);
    std::uint8_t* selectionBuffer(std::uint8_t index);
    std::uint8_t* manifestBuffer();
    bool fileMatches(const char* path, const std::uint8_t* expected, std::size_t length);

    static bool parseMapSetSelection(const std::uint8_t* input, std::size_t length,
                                     std::uint32_t& generation,
                                     Pyxis::MapPackManifest& metadata,
                                     ActivePackView* packs, std::uint8_t& pack_count);
    static bool spanCovers(const ActivePackView& pack, const TileKey& key);
    static MapTilePackResult makePath(const char* pack_id, const TileKey* key,
                                      bool manifest, char* output, std::size_t capacity);
};

}  // namespace TDeck
}  // namespace Hardware

#endif
