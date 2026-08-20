// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_MAP_TILE_STREAM_READER_H
#define UI_LXMF_MAP_TILE_STREAM_READER_H

#include <cstddef>
#include <cstdint>

#include "Hardware/TDeck/MapTileStore.h"

namespace Pyxis {

enum class MapTileStreamResult : std::uint8_t {
    OK = 0,
    MISS,
    STORAGE_UNAVAILABLE,
    TOO_LARGE,
    CANCELLED,
    IO_ERROR
};

class MapTileReadStream {
public:
    virtual ~MapTileReadStream() {}
    virtual MapTileStreamResult begin(const Hardware::TDeck::TileKey& key,
                                      std::uint32_t& size) = 0;
    virtual MapTileStreamResult read(std::uint8_t* output,
                                     std::size_t capacity,
                                     std::size_t& count) = 0;
    virtual void end() = 0;
};

class MapTileStopSource {
public:
    virtual ~MapTileStopSource() {}
    virtual bool stopRequested() const = 0;
};

class MapTileStreamReader {
public:
    static MapTileStreamResult readExact(
        MapTileReadStream& stream,
        MapTileStopSource& stop,
        const Hardware::TDeck::TileKey& key,
        std::uint8_t* output,
        std::size_t maximum_size,
        std::size_t chunk_size,
        std::size_t& total) {
        total = 0U;
        if (output == NULL || maximum_size == 0U || chunk_size == 0U) {
            return MapTileStreamResult::IO_ERROR;
        }
        std::uint32_t declared = 0U;
        const MapTileStreamResult begun = stream.begin(key, declared);
        if (begun != MapTileStreamResult::OK) return begun;
        if (declared > maximum_size) {
            stream.end();
            return MapTileStreamResult::TOO_LARGE;
        }
        while (total < declared) {
            if (stop.stopRequested()) {
                stream.end();
                return MapTileStreamResult::CANCELLED;
            }
            const std::size_t remaining = static_cast<std::size_t>(declared) - total;
            const std::size_t capacity = remaining < chunk_size ? remaining : chunk_size;
            std::size_t count = 0U;
            const MapTileStreamResult read = stream.read(output + total, capacity, count);
            if (read != MapTileStreamResult::OK || count == 0U || count > capacity) {
                stream.end();
                return read == MapTileStreamResult::STORAGE_UNAVAILABLE
                    ? read : MapTileStreamResult::IO_ERROR;
            }
            total += count;
        }
        stream.end();
        if (stop.stopRequested()) return MapTileStreamResult::CANCELLED;
        return MapTileStreamResult::OK;
    }
};

}  // namespace Pyxis

#endif
