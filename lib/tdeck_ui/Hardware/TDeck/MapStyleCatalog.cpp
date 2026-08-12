// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#include "Hardware/TDeck/MapStyleCatalog.h"
#include "Hardware/TDeck/MapTilePack.h"

#include <cstdio>
#include <cstring>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

namespace Hardware {
namespace TDeck {
namespace {

const char* const STYLE_IDS[MapStyleCatalog::MAX_STYLES] = {
    "osm-bright", "dark-matter", "positron", "toner"
};

}  // namespace

const char MapStyleCatalog::ACTIVE_SLOT_0_PATH[] = "/pyxis-map/active-pack.0";
const char MapStyleCatalog::ACTIVE_SLOT_1_PATH[] = "/pyxis-map/active-pack.1";

MapStyleCatalog::MapStyleCatalog(MapTileStorage& storage)
    : storage_(storage), styles_(), count_(0U), catalog_generation_(0U), buffers_() {
#if defined(ARDUINO_ARCH_ESP32)
    buffers_ = static_cast<std::uint8_t*>(heap_caps_malloc(
        3U * ActiveMapSetCodec::MAX_SERIALIZED_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
}

MapStyleCatalog::~MapStyleCatalog() {
#if defined(ARDUINO_ARCH_ESP32)
    if (buffers_ != NULL) heap_caps_free(buffers_);
    buffers_ = NULL;
#endif
}

std::uint8_t* MapStyleCatalog::buffer(std::size_t index) {
    if (index >= 3U) return NULL;
#if defined(ARDUINO_ARCH_ESP32)
    return buffers_ == NULL ? NULL : buffers_ + index * ActiveMapSetCodec::MAX_SERIALIZED_SIZE;
#else
    return buffers_[index];
#endif
}

bool MapStyleCatalog::allowedIndex(const char* style_id, std::size_t& index) {
    if (style_id == NULL) return false;
    for (std::size_t candidate = 0U; candidate < MAX_STYLES; ++candidate) {
        if (std::strcmp(style_id, STYLE_IDS[candidate]) == 0) {
            index = candidate; return true;
        }
    }
    return false;
}

void MapStyleCatalog::stylePath(std::size_t index, char* output, std::size_t capacity) {
    if (output == NULL || capacity == 0U || index >= MAX_STYLES) return;
    const int result = std::snprintf(output, capacity, "/pyxis-map/map-sets/%s.pmas", STYLE_IDS[index]);
    if (result < 0 || static_cast<std::size_t>(result) >= capacity) output[0] = '\0';
}

MapStyleCatalog::FileState MapStyleCatalog::readRecord(const char* path, std::uint8_t* output,
                                                        std::size_t& length,
                                                        ActiveMapSetView& view) {
    length = 0U;
    if (output == NULL) return FileState::INDETERMINATE;
    std::uint32_t declared_size = 0U;
    const TileStoreResult begin = storage_.beginRead(path, declared_size);
    if (begin == TileStoreResult::MISS) return FileState::MISSING;
    if (begin != TileStoreResult::OK) return FileState::INDETERMINATE;
    if (declared_size > ActiveMapSetCodec::MAX_SERIALIZED_SIZE) {
        storage_.endRead(); return FileState::INVALID;
    }
    std::size_t total = 0U;
    while (total < declared_size) {
        std::size_t count = 0U;
        const TileStoreResult result = storage_.readChunk(
            output + total, static_cast<std::size_t>(declared_size) - total, count);
        if (result != TileStoreResult::OK || count == 0U ||
            count > static_cast<std::size_t>(declared_size) - total) {
            storage_.endRead(); return FileState::INDETERMINATE;
        }
        total += count;
    }
    std::uint8_t trailing = 0U;
    std::size_t trailing_count = 0U;
    const TileStoreResult eof = storage_.readChunk(&trailing, 1U, trailing_count);
    storage_.endRead();
    if (eof != TileStoreResult::OK || trailing_count != 0U) return FileState::INDETERMINATE;
    length = total;
    return ActiveMapSetCodec::decode(output, total, view) ? FileState::PRESENT : FileState::INVALID;
}

bool MapStyleCatalog::sameRecord(const SlotState& first, const std::uint8_t* first_bytes,
                                 const SlotState& second, const std::uint8_t* second_bytes) {
    return first.length == second.length &&
        std::memcmp(first_bytes, second_bytes, first.length) == 0;
}

MapStyleCatalogResult MapStyleCatalog::readActiveSlots(SlotState (&slots)[2]) {
    const char* paths[2] = {ACTIVE_SLOT_0_PATH, ACTIVE_SLOT_1_PATH};
    for (std::size_t index = 0U; index < 2U; ++index) {
        slots[index].length = 0U; slots[index].view = ActiveMapSetView();
        slots[index].state = readRecord(paths[index], buffer(index + 1U),
                                        slots[index].length, slots[index].view);
        if (slots[index].state == FileState::INDETERMINATE) {
            return MapStyleCatalogResult::ACTIVE_IO_INDETERMINATE;
        }
    }
    if (slots[0].state == FileState::PRESENT && slots[1].state == FileState::PRESENT &&
        slots[0].view.generation == slots[1].view.generation &&
        !sameRecord(slots[0], buffer(1U), slots[1], buffer(2U))) {
        return MapStyleCatalogResult::ACTIVE_CONFLICT;
    }
    return MapStyleCatalogResult::OK;
}

void MapStyleCatalog::copySummary(MapStyleSummary& output, const ActiveMapSetView& view,
                                  bool active, bool synthesized) {
    output = MapStyleSummary();
    std::strcpy(output.id, view.map_set_id);
    std::strcpy(output.attribution, view.attribution);
    output.pack_count = view.pack_count;
    output.span_count = view.total_span_count;
    output.active = active;
    output.synthesized = synthesized;
}

void MapStyleCatalog::advanceCatalogGeneration() {
    ++catalog_generation_;
    if (catalog_generation_ == 0U) ++catalog_generation_;
}

MapStyleCatalogResult MapStyleCatalog::discover() {
    if (!storage_.isAvailable()) return MapStyleCatalogResult::STORAGE_UNAVAILABLE;
    if (buffer(0U) == NULL) return MapStyleCatalogResult::INSUFFICIENT_MEMORY;

    MapStyleSummary candidates[MAX_STYLES] = {};
    std::size_t candidate_count = 0U;
    for (std::size_t index = 0U; index < MAX_STYLES; ++index) {
        char path[80] = {};
        stylePath(index, path, sizeof(path));
        std::size_t length = 0U; ActiveMapSetView view = {};
        const FileState state = readRecord(path, buffer(0U), length, view);
        if (state == FileState::INDETERMINATE) return MapStyleCatalogResult::ACTIVE_IO_INDETERMINATE;
        if (state == FileState::INVALID ||
            (state == FileState::PRESENT && std::strcmp(view.map_set_id, STYLE_IDS[index]) != 0)) {
            return MapStyleCatalogResult::INVALID_STYLE_RECORD;
        }
        if (state == FileState::PRESENT) copySummary(candidates[candidate_count++], view, false, false);
    }

    SlotState slots[2] = {};
    const MapStyleCatalogResult active_result = readActiveSlots(slots);
    if (active_result != MapStyleCatalogResult::OK) return active_result;
    const SlotState* current = NULL;
    if (slots[0].state == FileState::PRESENT || slots[1].state == FileState::PRESENT) {
        current = slots[1].state != FileState::PRESENT ||
            (slots[0].state == FileState::PRESENT &&
             slots[0].view.generation >= slots[1].view.generation) ? &slots[0] : &slots[1];
    }
    if (current != NULL) {
        std::size_t allowlist_index = 0U;
        if (allowedIndex(current->view.map_set_id, allowlist_index)) {
            bool found = false;
            for (std::size_t index = 0U; index < candidate_count; ++index) {
                if (std::strcmp(candidates[index].id, current->view.map_set_id) == 0) {
                    candidates[index].active = true; found = true; break;
                }
            }
            if (!found && candidate_count < MAX_STYLES) {
                copySummary(candidates[candidate_count++], current->view, true, true);
            }
        }
    }

    std::memcpy(styles_, candidates, candidate_count * sizeof(MapStyleSummary));
    count_ = candidate_count;
    advanceCatalogGeneration();
    return MapStyleCatalogResult::OK;
}

MapStyleCatalogResult MapStyleCatalog::activate(std::uint32_t expected_catalog_generation,
                                                const char* style_id,
                                                BeginCommitCallback begin_commit,
                                                void* commit_context) {
    if (expected_catalog_generation != catalog_generation_) return MapStyleCatalogResult::STALE_CATALOG;
    std::size_t style_index = 0U;
    if (!allowedIndex(style_id, style_index)) return MapStyleCatalogResult::UNKNOWN_STYLE;
    bool catalogued = false;
    for (std::size_t index = 0U; index < count_; ++index) {
        if (std::strcmp(styles_[index].id, style_id) == 0) { catalogued = true; break; }
    }
    if (!catalogued) return MapStyleCatalogResult::UNKNOWN_STYLE;
    if (!storage_.isAvailable()) return MapStyleCatalogResult::STORAGE_UNAVAILABLE;
    if (buffer(0U) == NULL) return MapStyleCatalogResult::INSUFFICIENT_MEMORY;

    char selected_path[80] = {};
    stylePath(style_index, selected_path, sizeof(selected_path));
    std::size_t selected_length = 0U; ActiveMapSetView selected_view = {};
    const FileState selected_state = readRecord(selected_path, buffer(0U), selected_length, selected_view);
    if (selected_state != FileState::PRESENT || std::strcmp(selected_view.map_set_id, style_id) != 0) {
        return MapStyleCatalogResult::INVALID_STYLE_RECORD;
    }

    const MapTilePackResult semantic = MapTilePack::validateMapSet(
        storage_, selected_view, buffer(1U), ActiveMapSetCodec::MAX_SERIALIZED_SIZE);
    if (semantic != MapTilePackResult::OK) {
        if (semantic == MapTilePackResult::STORAGE_UNAVAILABLE)
            return MapStyleCatalogResult::STORAGE_UNAVAILABLE;
        if (semantic == MapTilePackResult::INSUFFICIENT_MEMORY)
            return MapStyleCatalogResult::INSUFFICIENT_MEMORY;
        if (semantic == MapTilePackResult::IO_ERROR || semantic == MapTilePackResult::BUSY)
            return MapStyleCatalogResult::ACTIVE_IO_INDETERMINATE;
        return MapStyleCatalogResult::INVALID_STYLE_RECORD;
    }

    SlotState slots[2] = {};
    const MapStyleCatalogResult active_result = readActiveSlots(slots);
    if (active_result != MapStyleCatalogResult::OK) return active_result;
    std::uint32_t highest = 0U;
    for (std::size_t index = 0U; index < 2U; ++index) {
        if (slots[index].state == FileState::PRESENT && slots[index].view.generation > highest) {
            highest = slots[index].view.generation;
        }
    }
    if (highest == UINT32_MAX) return MapStyleCatalogResult::GENERATION_EXHAUSTED;

    std::size_t target = 0U;
    if (slots[0].state != FileState::PRESENT) target = 0U;
    else if (slots[1].state != FileState::PRESENT) target = 1U;
    else target = slots[0].view.generation <= slots[1].view.generation ? 0U : 1U;
    const std::uint32_t next_generation = highest + 1U;
    std::size_t write_length = 0U;
    if (!ActiveMapSetCodec::resequence(buffer(0U), selected_length, next_generation,
                                       buffer(0U), ActiveMapSetCodec::MAX_SERIALIZED_SIZE,
                                       write_length)) return MapStyleCatalogResult::INVALID_STYLE_RECORD;

    const char* target_path = target == 0U ? ACTIVE_SLOT_0_PATH : ACTIVE_SLOT_1_PATH;
    if (storage_.beginWrite(target_path) != TileStoreResult::OK) {
        storage_.abortWrite(); return MapStyleCatalogResult::WRITE_FAILED;
    }
    std::size_t offset = 0U;
    while (offset < write_length) {
        std::size_t written = 0U;
        const TileStoreResult result = storage_.writeChunk(buffer(0U) + offset,
                                                            write_length - offset, written);
        if (result != TileStoreResult::OK || written == 0U || written > write_length - offset) {
            storage_.abortWrite(); return MapStyleCatalogResult::WRITE_FAILED;
        }
        offset += written;
    }
    if (begin_commit != NULL && !begin_commit(commit_context)) {
        storage_.abortWrite(); return MapStyleCatalogResult::CANCELLED;
    }
    if (storage_.commitWrite() != TileStoreResult::OK) {
        storage_.abortWrite(); return MapStyleCatalogResult::WRITE_FAILED;
    }

    std::size_t readback_length = 0U; ActiveMapSetView readback_view = {};
    const FileState readback_state = readRecord(target_path, buffer(target + 1U),
                                                 readback_length, readback_view);
    if (readback_state != FileState::PRESENT || readback_length != write_length ||
        std::memcmp(buffer(target + 1U), buffer(0U), write_length) != 0 ||
        readback_view.generation != next_generation ||
        std::strcmp(readback_view.map_set_id, style_id) != 0) {
        return MapStyleCatalogResult::READBACK_MISMATCH;
    }

    for (std::size_t index = 0U; index < count_; ++index) {
        styles_[index].active = std::strcmp(styles_[index].id, style_id) == 0;
    }
    advanceCatalogGeneration();
    return MapStyleCatalogResult::OK;
}

}  // namespace TDeck
}  // namespace Hardware
