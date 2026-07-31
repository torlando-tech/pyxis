// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_DECODED_TILE_CACHE_H
#define UI_LXMF_DECODED_TILE_CACHE_H

#include <cstddef>
#include <cstdint>

#include "Hardware/TDeck/MapTileStore.h"

namespace Pyxis {

/** Fixed-capacity LRU over caller-owned decoded RGB565 buffers. */
class DecodedTileCache {
public:
    static constexpr std::size_t CAPACITY = 12U;

    explicit DecodedTileCache(std::size_t pixel_count);

    bool attach(std::size_t index, std::uint16_t* pixels);
    bool get(const Hardware::TDeck::TileKey& key,
             std::uint16_t* output, std::size_t pixel_count);
    bool put(const Hardware::TDeck::TileKey& key,
             const std::uint16_t* input, std::size_t pixel_count);
    void clear();

    std::size_t validCount() const;
    std::size_t attachedCount() const;

private:
    struct Entry {
        Hardware::TDeck::TileKey key;
        std::uint16_t* pixels;
        std::uint8_t rank;
        bool valid;
    };

    std::size_t pixel_count_;
    Entry entries_[CAPACITY];

    static bool sameKey(const Hardware::TDeck::TileKey& left,
                        const Hardware::TDeck::TileKey& right);
    int find(const Hardware::TDeck::TileKey& key) const;
    int selectTarget() const;
    void touch(std::size_t index);
};

}  // namespace Pyxis

#endif
