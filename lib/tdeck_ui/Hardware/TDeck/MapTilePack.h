// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_MAP_TILE_PACK_H
#define HARDWARE_TDECK_MAP_TILE_PACK_H

#include <cstddef>
#include <cstdint>

#include "Hardware/TDeck/MapTileStore.h"
#include "UI/LXMF/MapPackManifest.h"

namespace Hardware {
namespace TDeck {

enum class MapTilePackResult : std::uint8_t {
    OK = 0,
    NO_SELECTION,
    INVALID_PACK_ID,
    STORAGE_UNAVAILABLE,
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
    STORAGE_UNAVAILABLE
};

/**
 * Allocation-free, read-only access to one SD-selected map pack.
 *
 * initialize() reads the bounded active marker and manifest transactionally:
 * an already usable selection remains active if a replacement is malformed or
 * unreadable. Only one tile stream may be open. Every read failure, explicit
 * end, reinitialize, and destruction closes that stream.
 */
class MapTilePack {
public:
    static const char ACTIVE_PACK_PATH[];
    static const std::size_t PATH_CAPACITY = 80U;
    static const std::size_t MANIFEST_BUFFER_CAPACITY = Pyxis::MapPackManifest::MAX_SERIALIZED_SIZE;

    explicit MapTilePack(MapTileStorage& storage);
    ~MapTilePack();

    MapTilePackResult initialize();

    MapTilePackStatus status() const { return status_; }
    bool hasSelection() const { return status_ == MapTilePackStatus::READY; }
    const Pyxis::MapPackManifest& metadata() const { return manifest_; }

    static bool isValidPackId(const char* pack_id);
    static MapTilePackResult manifestPath(const char* pack_id, char* output, std::size_t capacity);
    static MapTilePackResult tilePath(const char* pack_id, const TileKey& key,
                                      char* output, std::size_t capacity);

    MapTilePackResult beginGet(const TileKey& key, std::uint32_t& size);
    MapTilePackResult readGetChunk(std::uint8_t* output, std::size_t capacity,
                                   std::size_t& count);
    void endGet();

    static std::size_t ramBytes() { return sizeof(MapTilePack); }

private:
    MapTileStorage& storage_;
    Pyxis::MapPackManifest manifest_;
    MapTilePackStatus status_;
    bool stream_open_;
    std::uint32_t stream_remaining_;
    std::uint8_t manifest_buffer_[MANIFEST_BUFFER_CAPACITY];

    MapTilePack(const MapTilePack&);
    MapTilePack& operator=(const MapTilePack&);

    MapTilePackResult failInitialize(MapTilePackResult result, MapTilePackStatus initial_status,
                                     bool had_selection);
    MapTilePackResult loadFile(const char* path, std::uint8_t* output, std::size_t capacity,
                               std::size_t& length, MapTilePackResult missing_result,
                               MapTilePackResult oversized_result);
    static bool isValidKey(const TileKey& key);
    static MapTilePackResult makePath(const char* pack_id, const TileKey* key,
                                      bool manifest, char* output, std::size_t capacity);
};

}  // namespace TDeck
}  // namespace Hardware

#endif
