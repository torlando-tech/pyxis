// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_MAP_STYLE_CATALOG_H
#define HARDWARE_TDECK_MAP_STYLE_CATALOG_H

#include <cstddef>
#include <cstdint>

#include "Hardware/TDeck/ActiveMapSetCodec.h"
#include "Hardware/TDeck/MapTileStore.h"

namespace Hardware {
namespace TDeck {

enum class MapStyleCatalogResult : std::uint8_t {
    OK = 0,
    STORAGE_UNAVAILABLE,
    INSUFFICIENT_MEMORY,
    INVALID_STYLE_RECORD,
    ACTIVE_IO_INDETERMINATE,
    ACTIVE_CONFLICT,
    STALE_CATALOG,
    UNKNOWN_STYLE,
    GENERATION_EXHAUSTED,
    WRITE_FAILED,
    READBACK_MISMATCH
};

struct MapStyleSummary {
    char id[Pyxis::MapPackManifest::PACK_ID_CAPACITY];
    char attribution[Pyxis::MapPackManifest::ATTRIBUTION_CAPACITY];
    std::uint8_t pack_count;
    std::uint16_t span_count;
    bool active;
    bool synthesized;
};

/** Fixed-allowlist PMAS discovery and redundant-slot transactional activation. */
class MapStyleCatalog {
public:
    static const std::size_t MAX_STYLES = 4U;
    static const char ACTIVE_SLOT_0_PATH[];
    static const char ACTIVE_SLOT_1_PATH[];

    explicit MapStyleCatalog(MapTileStorage& storage);
    ~MapStyleCatalog();

    MapStyleCatalogResult discover();
    MapStyleCatalogResult activate(std::uint32_t expected_catalog_generation,
                                   const char* style_id);

    std::size_t count() const { return count_; }
    const MapStyleSummary* style(std::size_t index) const {
        return index < count_ ? &styles_[index] : NULL;
    }
    std::uint32_t generation() const { return catalog_generation_; }

private:
    enum class FileState : std::uint8_t { PRESENT, MISSING, INVALID, INDETERMINATE };
    struct SlotState {
        FileState state;
        std::size_t length;
        ActiveMapSetView view;
    };

    MapTileStorage& storage_;
    MapStyleSummary styles_[MAX_STYLES];
    std::size_t count_;
    std::uint32_t catalog_generation_;
#if defined(ARDUINO_ARCH_ESP32)
    std::uint8_t* buffers_;
#else
    std::uint8_t buffers_[3][ActiveMapSetCodec::MAX_SERIALIZED_SIZE];
#endif

    MapStyleCatalog(const MapStyleCatalog&);
    MapStyleCatalog& operator=(const MapStyleCatalog&);

    std::uint8_t* buffer(std::size_t index);
    FileState readRecord(const char* path, std::uint8_t* output,
                         std::size_t& length, ActiveMapSetView& view);
    MapStyleCatalogResult readActiveSlots(SlotState (&slots)[2]);
    static bool allowedIndex(const char* style_id, std::size_t& index);
    static void stylePath(std::size_t index, char* output, std::size_t capacity);
    static bool sameRecord(const SlotState& first, const std::uint8_t* first_bytes,
                           const SlotState& second, const std::uint8_t* second_bytes);
    static void copySummary(MapStyleSummary& output, const ActiveMapSetView& view,
                            bool active, bool synthesized);
    void advanceCatalogGeneration();
};

}  // namespace TDeck
}  // namespace Hardware

#endif
