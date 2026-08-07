// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#include "Hardware/TDeck/MapTilePack.h"

#include <cstdio>
#include <cstring>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

namespace Hardware {
namespace TDeck {
namespace {

std::uint16_t selectionU16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(input[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(input[1]) << 8U);
}

std::uint32_t selectionU32(const std::uint8_t* input) {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

std::uint32_t selectionCrc32(const std::uint8_t* input, std::size_t length) {
    std::uint32_t crc = UINT32_C(0xffffffff);
    for (std::size_t index = 0U; index < length; ++index) {
        crc ^= input[index];
        for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? UINT32_C(0xedb88320) : 0U);
        }
    }
    return ~crc;
}

bool decodeSelection(const std::uint8_t* input, std::size_t length,
                     char* pack_id, std::uint32_t& generation) {
    static const std::uint8_t magic[4] = {'P', 'M', 'A', 'S'};
    if (input == NULL || pack_id == NULL || length != MapTilePack::LEGACY_ACTIVE_SELECTION_SIZE ||
        std::memcmp(input, magic, sizeof(magic)) != 0 || input[4] != 1U || input[5] != 0U ||
        selectionU16(input + 6U) != MapTilePack::LEGACY_ACTIVE_SELECTION_SIZE ||
        selectionU32(input + 44U) != selectionCrc32(input, 44U)) {
        return false;
    }
    generation = selectionU32(input + 8U);
    const std::size_t id_length = input[12];
    if (generation == 0U || id_length == 0U ||
        id_length >= Pyxis::MapPackManifest::PACK_ID_CAPACITY) return false;
    for (std::size_t index = 13U + id_length; index < 44U; ++index) {
        if (input[index] != 0U) return false;
    }
    std::memcpy(pack_id, input + 13U, id_length);
    pack_id[id_length] = '\0';
    return MapTilePack::isValidPackId(pack_id);
}

bool readSelectionString(const std::uint8_t* input, std::size_t end, std::size_t& position,
                         char* output, std::size_t capacity, bool identifier) {
    if (input == NULL || output == NULL || capacity < 2U || position >= end) return false;
    const std::size_t length = input[position++];
    if (length == 0U || length >= capacity || position + length > end) return false;
    for (std::size_t index = 0U; index < length; ++index) {
        const std::uint8_t value = input[position + index];
        if (value < 0x20U || value > 0x7eU) return false;
        if (identifier && !((value >= 'a' && value <= 'z') ||
                            (value >= '0' && value <= '9') || value == '_' || value == '-')) {
            return false;
        }
    }
    std::memcpy(output, input + position, length);
    output[length] = '\0';
    position += length;
    return true;
}

}  // namespace

const char MapTilePack::ACTIVE_PACK_PATH[] = "/pyxis-map/active-pack";
const char MapTilePack::ACTIVE_PACK_SLOT_0_PATH[] = "/pyxis-map/active-pack.0";
const char MapTilePack::ACTIVE_PACK_SLOT_1_PATH[] = "/pyxis-map/active-pack.1";

MapTilePack::MapTilePack(MapTileStorage& storage)
    : storage_(storage), manifest_(), active_packs_(), active_pack_count_(0U),
      map_set_active_(false), selection_generation_(0U),
      status_(MapTilePackStatus::UNINITIALIZED),
      stream_open_(false), stream_remaining_(0U), selection_buffers_(),
      active_selection_buffer_(0U) {
#if defined(ARDUINO_ARCH_ESP32)
    selection_buffers_ = static_cast<std::uint8_t*>(heap_caps_malloc(
        2U * ACTIVE_SELECTION_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
}

MapTilePack::~MapTilePack() {
    endGet();
#if defined(ARDUINO_ARCH_ESP32)
    if (selection_buffers_ != NULL) heap_caps_free(selection_buffers_);
    selection_buffers_ = NULL;
#endif
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

std::uint8_t* MapTilePack::selectionBuffer(std::uint8_t index) {
    if (index > 1U) return NULL;
#if defined(ARDUINO_ARCH_ESP32)
    return selection_buffers_ == NULL ? NULL : selection_buffers_ +
        static_cast<std::size_t>(index) * ACTIVE_SELECTION_SIZE;
#else
    return selection_buffers_[index];
#endif
}

bool MapTilePack::parseMapSetSelection(const std::uint8_t* input, std::size_t length,
                                       std::uint32_t& generation,
                                       Pyxis::MapPackManifest& metadata,
                                       ActivePackView* packs, std::uint8_t& pack_count) {
    static const std::uint8_t magic[4] = {'P', 'M', 'A', 'S'};
    if (input == NULL || packs == NULL || length < 16U || length > ACTIVE_SELECTION_SIZE ||
        std::memcmp(input, magic, sizeof(magic)) != 0 || input[4] != 2U || input[5] != 0U ||
        selectionU16(input + 6U) != length ||
        selectionU32(input + length - 4U) != selectionCrc32(input, length - 4U)) return false;
    generation = selectionU32(input + 8U);
    if (generation == 0U) return false;
    metadata = Pyxis::MapPackManifest();
    std::size_t position = 12U;
    if (!readSelectionString(input, length - 4U, position, metadata.pack_id,
                             sizeof(metadata.pack_id), true)) return false;
    std::strncpy(metadata.name, metadata.pack_id, sizeof(metadata.name) - 1U);
    if (!readSelectionString(input, length - 4U, position, metadata.attribution,
                             sizeof(metadata.attribution), false) || position >= length - 4U) return false;
    std::strncpy(metadata.source, "active map set", sizeof(metadata.source) - 1U);
    pack_count = input[position++];
    if (pack_count == 0U || pack_count > MAX_ACTIVE_PACKS) return false;
    std::uint16_t total_spans = 0U;
    for (std::uint8_t pack_index = 0U; pack_index < pack_count; ++pack_index) {
        ActivePackView& pack = packs[pack_index];
        if (!readSelectionString(input, length - 4U, position, pack.pack_id,
                                 sizeof(pack.pack_id), true)) return false;
        for (std::uint8_t previous_pack = 0U; previous_pack < pack_index; ++previous_pack) {
            if (std::strcmp(pack.pack_id, packs[previous_pack].pack_id) == 0) return false;
        }
        if (position + 2U > length - 4U) return false;
        pack.span_count = selectionU16(input + position);
        position += 2U;
        if (pack.span_count == 0U || pack.span_count > MAX_ACTIVE_ROW_SPANS ||
            total_spans > MAX_ACTIVE_ROW_SPANS - pack.span_count ||
            position + static_cast<std::size_t>(pack.span_count) * 13U > length - 4U) return false;
        pack.span_bytes = input + position;
        std::uint8_t previous_zoom = 0U;
        std::uint32_t previous_y = 0U;
        std::uint32_t previous_x_maximum = 0U;
        bool have_previous = false;
        for (std::uint16_t span_index = 0U; span_index < pack.span_count; ++span_index) {
            const std::uint8_t* span = input + position;
            const std::uint8_t zoom = span[0];
            const std::uint32_t y = selectionU32(span + 1U);
            const std::uint32_t x_minimum = selectionU32(span + 5U);
            const std::uint32_t x_maximum = selectionU32(span + 9U);
            if (zoom > Pyxis::MapPackManifest::MAX_ZOOM) return false;
            const std::uint32_t edge = UINT32_C(1) << zoom;
            if (y >= edge || x_minimum > x_maximum || x_maximum >= edge ||
                (have_previous && (zoom < previous_zoom ||
                 (zoom == previous_zoom && y < previous_y) ||
                 (zoom == previous_zoom && y == previous_y &&
                  x_minimum <= previous_x_maximum + 1U)))) return false;
            previous_zoom = zoom;
            previous_y = y;
            previous_x_maximum = x_maximum;
            have_previous = true;
            position += 13U;
        }
        total_spans = static_cast<std::uint16_t>(total_spans + pack.span_count);
    }
    return position == length - 4U;
}

bool MapTilePack::spanCovers(const ActivePackView& pack, const TileKey& key) {
    for (std::uint16_t index = 0U; index < pack.span_count; ++index) {
        const std::uint8_t* span = pack.span_bytes + static_cast<std::size_t>(index) * 13U;
        const std::uint8_t zoom = span[0];
        if (zoom > key.zoom) return false;
        if (zoom != key.zoom) continue;
        const std::uint32_t y = selectionU32(span + 1U);
        if (y > key.y) return false;
        if (y == key.y && key.x >= selectionU32(span + 5U) && key.x <= selectionU32(span + 9U)) {
            return true;
        }
    }
    return false;
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
    if (selectionBuffer(0U) == NULL) {
        return failInitialize(MapTilePackResult::INSUFFICIENT_MEMORY,
                              MapTilePackStatus::INSUFFICIENT_MEMORY, had_selection);
    }
    if (!storage_.isAvailable()) {
        return failInitialize(MapTilePackResult::STORAGE_UNAVAILABLE,
                              MapTilePackStatus::STORAGE_UNAVAILABLE, had_selection);
    }

    const std::uint8_t candidate_buffer = static_cast<std::uint8_t>(active_selection_buffer_ ^ 1U);
    const char* slot_paths[2] = {ACTIVE_PACK_SLOT_0_PATH, ACTIVE_PACK_SLOT_1_PATH};
    std::uint32_t slot_generations[2] = {0U, 0U};
    std::uint32_t slot_checksums[2] = {0U, 0U};
    bool slot_valid[2] = {false, false};
    MapTilePackResult result = MapTilePackResult::NO_SELECTION;
    for (std::size_t slot = 0U; slot < 2U; ++slot) {
        std::size_t record_length = 0U;
        result = loadFile(slot_paths[slot], selectionBuffer(candidate_buffer), ACTIVE_SELECTION_SIZE,
                          record_length, MapTilePackResult::NO_SELECTION,
                          MapTilePackResult::INVALID_PACK_ID);
        if (result == MapTilePackResult::OK) {
            char legacy_id[Pyxis::MapPackManifest::PACK_ID_CAPACITY] = {};
            Pyxis::MapPackManifest map_set_metadata = {};
            ActivePackView map_set_packs[MAX_ACTIVE_PACKS] = {};
            std::uint8_t map_set_pack_count = 0U;
            if (decodeSelection(selectionBuffer(candidate_buffer), record_length,
                                legacy_id, slot_generations[slot]) ||
                parseMapSetSelection(selectionBuffer(candidate_buffer), record_length,
                                     slot_generations[slot], map_set_metadata,
                                     map_set_packs, map_set_pack_count)) {
                slot_valid[slot] = true;
                slot_checksums[slot] = selectionU32(selectionBuffer(candidate_buffer) + record_length - 4U);
            }
        } else if (result != MapTilePackResult::NO_SELECTION &&
                   result != MapTilePackResult::INVALID_PACK_ID) {
            const MapTilePackStatus state = result == MapTilePackResult::STORAGE_UNAVAILABLE
                ? MapTilePackStatus::STORAGE_UNAVAILABLE : MapTilePackStatus::INVALID_SELECTION;
            return failInitialize(result, state, had_selection);
        }
    }
    if (slot_valid[0] && slot_valid[1] && slot_generations[0] == slot_generations[1] &&
        slot_checksums[0] != slot_checksums[1]) {
        return failInitialize(MapTilePackResult::INVALID_PACK_ID,
                              MapTilePackStatus::INVALID_SELECTION, had_selection);
    }

    char selected_pack_id[Pyxis::MapPackManifest::PACK_ID_CAPACITY] = {};
    std::uint32_t selected_generation = 0U;
    if (slot_valid[0] || slot_valid[1]) {
        const std::size_t chosen = !slot_valid[1] ||
            (slot_valid[0] && slot_generations[0] >= slot_generations[1]) ? 0U : 1U;
        std::size_t record_length = 0U;
        result = loadFile(slot_paths[chosen], selectionBuffer(candidate_buffer), ACTIVE_SELECTION_SIZE,
                          record_length, MapTilePackResult::NO_SELECTION,
                          MapTilePackResult::INVALID_PACK_ID);
        if (result != MapTilePackResult::OK) {
            return failInitialize(result, MapTilePackStatus::INVALID_SELECTION, had_selection);
        }
        std::uint32_t generation = 0U;
        Pyxis::MapPackManifest map_set_metadata = {};
        ActivePackView map_set_packs[MAX_ACTIVE_PACKS] = {};
        std::uint8_t map_set_pack_count = 0U;
        if (parseMapSetSelection(selectionBuffer(candidate_buffer), record_length, generation,
                                 map_set_metadata, map_set_packs, map_set_pack_count)) {
            manifest_ = map_set_metadata;
            std::memcpy(active_packs_, map_set_packs,
                        static_cast<std::size_t>(map_set_pack_count) * sizeof(ActivePackView));
            active_pack_count_ = map_set_pack_count;
            map_set_active_ = true;
            selection_generation_ = generation;
            active_selection_buffer_ = candidate_buffer;
            status_ = MapTilePackStatus::READY;
            return MapTilePackResult::OK;
        }
        if (!decodeSelection(selectionBuffer(candidate_buffer), record_length,
                             selected_pack_id, selected_generation)) {
            return failInitialize(MapTilePackResult::INVALID_PACK_ID,
                                  MapTilePackStatus::INVALID_SELECTION, had_selection);
        }
    } else {
        std::uint8_t marker[Pyxis::MapPackManifest::PACK_ID_CAPACITY];
        std::size_t marker_length = 0U;
        result = loadFile(ACTIVE_PACK_PATH, marker, sizeof(marker) - 1U,
                          marker_length, MapTilePackResult::NO_SELECTION,
                          MapTilePackResult::INVALID_PACK_ID);
        if (result == MapTilePackResult::NO_SELECTION ||
            (result == MapTilePackResult::OK && marker_length == 0U)) {
            manifest_ = Pyxis::MapPackManifest();
            active_pack_count_ = 0U;
            map_set_active_ = false;
            selection_generation_ = 0U;
            status_ = MapTilePackStatus::NO_SELECTION;
            return MapTilePackResult::NO_SELECTION;
        }
        if (result != MapTilePackResult::OK) {
            const MapTilePackStatus state = result == MapTilePackResult::STORAGE_UNAVAILABLE
                ? MapTilePackStatus::STORAGE_UNAVAILABLE : MapTilePackStatus::INVALID_SELECTION;
            return failInitialize(result, state, had_selection);
        }
        marker[marker_length] = 0U;
        if (std::memchr(marker, 0, marker_length) != NULL ||
            !isValidPackId(reinterpret_cast<const char*>(marker))) {
            return failInitialize(MapTilePackResult::INVALID_PACK_ID,
                                  MapTilePackStatus::INVALID_SELECTION, had_selection);
        }
        std::strcpy(selected_pack_id, reinterpret_cast<const char*>(marker));
    }

    char manifest_path[PATH_CAPACITY];
    result = manifestPath(selected_pack_id, manifest_path, sizeof(manifest_path));
    if (result != MapTilePackResult::OK) {
        return failInitialize(result, MapTilePackStatus::INVALID_SELECTION, had_selection);
    }
    std::size_t manifest_length = 0U;
    result = loadFile(manifest_path, selectionBuffer(candidate_buffer), MANIFEST_BUFFER_CAPACITY,
                      manifest_length, MapTilePackResult::MANIFEST_MISSING,
                      MapTilePackResult::MANIFEST_TOO_LARGE);
    if (result != MapTilePackResult::OK) {
        const MapTilePackStatus state = result == MapTilePackResult::STORAGE_UNAVAILABLE
            ? MapTilePackStatus::STORAGE_UNAVAILABLE : MapTilePackStatus::INVALID_SELECTION;
        return failInitialize(result, state, had_selection);
    }

    Pyxis::MapPackManifest candidate = {};
    if (Pyxis::MapPackManifest::parse(selectionBuffer(candidate_buffer), manifest_length, candidate) !=
        Pyxis::ManifestResult::OK) {
        return failInitialize(MapTilePackResult::INVALID_MANIFEST,
                              MapTilePackStatus::INVALID_SELECTION, had_selection);
    }
    if (std::strcmp(selected_pack_id, candidate.pack_id) != 0) {
        return failInitialize(MapTilePackResult::PACK_ID_MISMATCH,
                              MapTilePackStatus::INVALID_SELECTION, had_selection);
    }

    manifest_ = candidate;
    active_pack_count_ = 0U;
    map_set_active_ = false;
    selection_generation_ = selected_generation;
    active_selection_buffer_ = candidate_buffer;
    status_ = MapTilePackStatus::READY;
    return MapTilePackResult::OK;
}

MapTilePackResult MapTilePack::beginGet(const TileKey& key, std::uint32_t& size) {
    if (stream_open_) return MapTilePackResult::BUSY;
    if (!hasSelection()) return MapTilePackResult::NOT_INITIALIZED;
    if (!isValidKey(key)) return MapTilePackResult::INVALID_KEY;
    if (!storage_.isAvailable()) return MapTilePackResult::STORAGE_UNAVAILABLE;

    if (map_set_active_) {
        bool covered = false;
        for (std::uint8_t index = 0U; index < active_pack_count_; ++index) {
            if (!spanCovers(active_packs_[index], key)) continue;
            covered = true;
            char path[PATH_CAPACITY];
            MapTilePackResult result = tilePath(active_packs_[index].pack_id, key, path, sizeof(path));
            if (result != MapTilePackResult::OK) return result;
            std::uint32_t candidate_size = 0U;
            const TileStoreResult begin = storage_.beginRead(path, candidate_size);
            if (begin == TileStoreResult::MISS) continue;
            if (begin == TileStoreResult::STORAGE_UNAVAILABLE) return MapTilePackResult::STORAGE_UNAVAILABLE;
            if (begin == TileStoreResult::BUSY) return MapTilePackResult::BUSY;
            if (begin != TileStoreResult::OK) return MapTilePackResult::IO_ERROR;
            stream_open_ = true;
            stream_remaining_ = candidate_size;
            size = candidate_size;
            return MapTilePackResult::OK;
        }
        return covered ? MapTilePackResult::TILE_MISSING : MapTilePackResult::UNCOVERED;
    }

    if (!manifest_.covers(key)) return MapTilePackResult::UNCOVERED;
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
