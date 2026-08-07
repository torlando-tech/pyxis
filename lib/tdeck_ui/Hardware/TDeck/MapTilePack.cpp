// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#include "Hardware/TDeck/MapTilePack.h"

#include <cstdio>
#include <cstring>

namespace Hardware {
namespace TDeck {

const char MapTilePack::ACTIVE_PACK_PATH[] = "/pyxis-map/active-pack";

MapTilePack::MapTilePack(MapTileStorage& storage)
    : storage_(storage), manifest_(), status_(MapTilePackStatus::UNINITIALIZED),
      stream_open_(false), stream_remaining_(0U), manifest_buffer_() {}

MapTilePack::~MapTilePack() {
    endGet();
}

bool MapTilePack::isValidPackId(const char* pack_id) {
    if (pack_id == NULL) return false;
    std::size_t length = 0U;
    while (length < Pyxis::MapPackManifest::PACK_ID_CAPACITY && pack_id[length] != '\0') {
        const char character = pack_id[length];
        if (!((character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '_' || character == '-')) {
            return false;
        }
        ++length;
    }
    return length > 0U && length < Pyxis::MapPackManifest::PACK_ID_CAPACITY;
}

bool MapTilePack::isValidKey(const TileKey& key) {
    if (key.zoom > Pyxis::MapPackManifest::MAX_ZOOM) return false;
    const std::uint32_t edge = UINT32_C(1) << key.zoom;
    return key.x < edge && key.y < edge;
}

MapTilePackResult MapTilePack::makePath(const char* pack_id, const TileKey* key,
                                        bool manifest, char* output, std::size_t capacity) {
    if (!isValidPackId(pack_id)) return MapTilePackResult::INVALID_PACK_ID;
    if (output == NULL || capacity == 0U) return MapTilePackResult::INVALID_ARGUMENT;
    if (!manifest && (key == NULL || !isValidKey(*key))) return MapTilePackResult::INVALID_KEY;

    char temporary[PATH_CAPACITY];
    int count = 0;
    if (manifest) {
        count = std::snprintf(temporary, sizeof(temporary), "/pyxis-map/packs/%s/manifest.pmp", pack_id);
    } else {
        count = std::snprintf(temporary, sizeof(temporary),
                              "/pyxis-map/packs/%s/tiles/%u/%lu/%lu.png", pack_id,
                              static_cast<unsigned>(key->zoom),
                              static_cast<unsigned long>(key->x),
                              static_cast<unsigned long>(key->y));
    }
    if (count < 0 || static_cast<std::size_t>(count) >= sizeof(temporary) ||
        static_cast<std::size_t>(count) + 1U > capacity) {
        return MapTilePackResult::PATH_TOO_LONG;
    }
    std::memcpy(output, temporary, static_cast<std::size_t>(count) + 1U);
    return MapTilePackResult::OK;
}

MapTilePackResult MapTilePack::manifestPath(const char* pack_id, char* output,
                                            std::size_t capacity) {
    return makePath(pack_id, NULL, true, output, capacity);
}

MapTilePackResult MapTilePack::tilePath(const char* pack_id, const TileKey& key,
                                        char* output, std::size_t capacity) {
    return makePath(pack_id, &key, false, output, capacity);
}

MapTilePackResult MapTilePack::loadFile(const char* path, std::uint8_t* output,
                                        std::size_t capacity, std::size_t& length,
                                        MapTilePackResult missing_result,
                                        MapTilePackResult oversized_result) {
    std::uint32_t declared_size = 0U;
    const TileStoreResult begin = storage_.beginRead(path, declared_size);
    if (begin == TileStoreResult::MISS) return missing_result;
    if (begin == TileStoreResult::STORAGE_UNAVAILABLE) return MapTilePackResult::STORAGE_UNAVAILABLE;
    if (begin != TileStoreResult::OK) return MapTilePackResult::IO_ERROR;

    if (declared_size > capacity) {
        storage_.endRead();
        return oversized_result;
    }

    std::size_t total = 0U;
    while (total < declared_size) {
        std::size_t count = 0U;
        const TileStoreResult read = storage_.readChunk(output + total,
            static_cast<std::size_t>(declared_size) - total, count);
        if (read != TileStoreResult::OK || count == 0U ||
            count > static_cast<std::size_t>(declared_size) - total) {
            storage_.endRead();
            return MapTilePackResult::IO_ERROR;
        }
        total += count;
    }
    std::uint8_t trailing = 0U;
    std::size_t trailing_count = 0U;
    const TileStoreResult eof = storage_.readChunk(&trailing, 1U, trailing_count);
    storage_.endRead();
    if (eof != TileStoreResult::OK || trailing_count != 0U) {
        return MapTilePackResult::IO_ERROR;
    }
    length = total;
    return MapTilePackResult::OK;
}

MapTilePackResult MapTilePack::failInitialize(MapTilePackResult result,
                                              MapTilePackStatus initial_status,
                                              bool had_selection) {
    if (!had_selection) status_ = initial_status;
    return result;
}

MapTilePackResult MapTilePack::initialize() {
    endGet();
    const bool had_selection = hasSelection();
    if (!storage_.isAvailable()) {
        return failInitialize(MapTilePackResult::STORAGE_UNAVAILABLE,
                              MapTilePackStatus::STORAGE_UNAVAILABLE, had_selection);
    }

    std::uint8_t marker[Pyxis::MapPackManifest::PACK_ID_CAPACITY];
    std::size_t marker_length = 0U;
    MapTilePackResult result = loadFile(ACTIVE_PACK_PATH, marker, sizeof(marker) - 1U,
                                        marker_length, MapTilePackResult::NO_SELECTION,
                                        MapTilePackResult::INVALID_PACK_ID);
    if (result == MapTilePackResult::NO_SELECTION) {
        manifest_ = Pyxis::MapPackManifest();
        status_ = MapTilePackStatus::NO_SELECTION;
        return result;
    }
    if (result != MapTilePackResult::OK) {
        const MapTilePackStatus state = result == MapTilePackResult::STORAGE_UNAVAILABLE
            ? MapTilePackStatus::STORAGE_UNAVAILABLE : MapTilePackStatus::INVALID_SELECTION;
        return failInitialize(result, state, had_selection);
    }
    if (marker_length == 0U) {
        manifest_ = Pyxis::MapPackManifest();
        status_ = MapTilePackStatus::NO_SELECTION;
        return MapTilePackResult::NO_SELECTION;
    }
    marker[marker_length] = 0U;
    const char* pack_id = reinterpret_cast<const char*>(marker);
    if (std::memchr(marker, 0, marker_length) != NULL || !isValidPackId(pack_id)) {
        return failInitialize(MapTilePackResult::INVALID_PACK_ID,
                              MapTilePackStatus::INVALID_SELECTION, had_selection);
    }

    char manifest_path[PATH_CAPACITY];
    result = manifestPath(pack_id, manifest_path, sizeof(manifest_path));
    if (result != MapTilePackResult::OK) {
        return failInitialize(result, MapTilePackStatus::INVALID_SELECTION, had_selection);
    }
    std::size_t manifest_length = 0U;
    result = loadFile(manifest_path, manifest_buffer_, sizeof(manifest_buffer_), manifest_length,
                      MapTilePackResult::MANIFEST_MISSING,
                      MapTilePackResult::MANIFEST_TOO_LARGE);
    if (result != MapTilePackResult::OK) {
        const MapTilePackStatus state = result == MapTilePackResult::STORAGE_UNAVAILABLE
            ? MapTilePackStatus::STORAGE_UNAVAILABLE : MapTilePackStatus::INVALID_SELECTION;
        return failInitialize(result, state, had_selection);
    }

    Pyxis::MapPackManifest candidate = {};
    if (Pyxis::MapPackManifest::parse(manifest_buffer_, manifest_length, candidate) !=
        Pyxis::ManifestResult::OK) {
        return failInitialize(MapTilePackResult::INVALID_MANIFEST,
                              MapTilePackStatus::INVALID_SELECTION, had_selection);
    }
    if (std::strcmp(pack_id, candidate.pack_id) != 0) {
        return failInitialize(MapTilePackResult::PACK_ID_MISMATCH,
                              MapTilePackStatus::INVALID_SELECTION, had_selection);
    }

    manifest_ = candidate;
    status_ = MapTilePackStatus::READY;
    return MapTilePackResult::OK;
}

MapTilePackResult MapTilePack::beginGet(const TileKey& key, std::uint32_t& size) {
    if (stream_open_) return MapTilePackResult::BUSY;
    if (!hasSelection()) return MapTilePackResult::NOT_INITIALIZED;
    if (!isValidKey(key)) return MapTilePackResult::INVALID_KEY;
    if (!manifest_.covers(key)) return MapTilePackResult::UNCOVERED;
    if (!storage_.isAvailable()) return MapTilePackResult::STORAGE_UNAVAILABLE;

    char path[PATH_CAPACITY];
    MapTilePackResult result = tilePath(manifest_.pack_id, key, path, sizeof(path));
    if (result != MapTilePackResult::OK) return result;
    std::uint32_t candidate_size = 0U;
    const TileStoreResult begin = storage_.beginRead(path, candidate_size);
    if (begin == TileStoreResult::MISS) return MapTilePackResult::TILE_MISSING;
    if (begin == TileStoreResult::STORAGE_UNAVAILABLE) return MapTilePackResult::STORAGE_UNAVAILABLE;
    if (begin == TileStoreResult::BUSY) return MapTilePackResult::BUSY;
    if (begin != TileStoreResult::OK) return MapTilePackResult::IO_ERROR;
    stream_open_ = true;
    stream_remaining_ = candidate_size;
    size = candidate_size;
    return MapTilePackResult::OK;
}

MapTilePackResult MapTilePack::readGetChunk(std::uint8_t* output, std::size_t capacity,
                                            std::size_t& count) {
    count = 0U;
    if (output == NULL || capacity == 0U) {
        endGet();
        return MapTilePackResult::INVALID_ARGUMENT;
    }
    if (!stream_open_) return MapTilePackResult::NOT_STREAMING;

    if (stream_remaining_ == 0U) {
        std::uint8_t trailing = 0U;
        std::size_t trailing_count = 0U;
        const TileStoreResult eof = storage_.readChunk(&trailing, 1U, trailing_count);
        endGet();
        return (eof == TileStoreResult::OK && trailing_count == 0U)
            ? MapTilePackResult::OK : MapTilePackResult::IO_ERROR;
    }

    std::size_t read_count = 0U;
    const std::size_t allowed = capacity < stream_remaining_ ? capacity : stream_remaining_;
    const TileStoreResult read = storage_.readChunk(output, allowed, read_count);
    if (read != TileStoreResult::OK || read_count > allowed ||
        (read_count == 0U && stream_remaining_ != 0U)) {
        endGet();
        return MapTilePackResult::IO_ERROR;
    }
    if (read_count == 0U) {
        endGet();
        return MapTilePackResult::OK;
    }
    stream_remaining_ -= static_cast<std::uint32_t>(read_count);
    if (stream_remaining_ == 0U) {
        std::uint8_t trailing = 0U;
        std::size_t trailing_count = 0U;
        const TileStoreResult eof = storage_.readChunk(&trailing, 1U, trailing_count);
        if (eof != TileStoreResult::OK || trailing_count != 0U) {
            endGet();
            return MapTilePackResult::IO_ERROR;
        }
        endGet();
    }
    count = read_count;
    return MapTilePackResult::OK;
}

void MapTilePack::endGet() {
    if (stream_open_) storage_.endRead();
    stream_open_ = false;
    stream_remaining_ = 0U;
}

}  // namespace TDeck
}  // namespace Hardware
