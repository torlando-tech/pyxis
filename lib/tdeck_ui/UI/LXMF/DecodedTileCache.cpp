// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#include "DecodedTileCache.h"

#include <cstring>

namespace Pyxis {

DecodedTileCache::DecodedTileCache(std::size_t pixel_count)
    : pixel_count_(pixel_count), entries_{} {}

bool DecodedTileCache::sameKey(const Hardware::TDeck::TileKey& left,
                               const Hardware::TDeck::TileKey& right) {
    return left.zoom == right.zoom && left.x == right.x && left.y == right.y;
}

bool DecodedTileCache::attach(std::size_t index, std::uint16_t* pixels) {
    if (index >= CAPACITY || pixels == NULL) return false;
    entries_[index].pixels = pixels;
    entries_[index].valid = false;
    entries_[index].rank = 0U;
    return true;
}

int DecodedTileCache::find(const Hardware::TDeck::TileKey& key) const {
    for (std::size_t index = 0U; index < CAPACITY; ++index) {
        if (entries_[index].pixels != NULL && entries_[index].valid &&
            sameKey(entries_[index].key, key)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int DecodedTileCache::selectTarget() const {
    int oldest = -1;
    std::uint8_t oldest_rank = 0U;
    for (std::size_t index = 0U; index < CAPACITY; ++index) {
        if (entries_[index].pixels == NULL) continue;
        if (!entries_[index].valid) return static_cast<int>(index);
        if (oldest < 0 || entries_[index].rank > oldest_rank) {
            oldest = static_cast<int>(index);
            oldest_rank = entries_[index].rank;
        }
    }
    return oldest;
}

void DecodedTileCache::touch(std::size_t index) {
    const std::uint8_t previous = entries_[index].valid
        ? entries_[index].rank
        : static_cast<std::uint8_t>(CAPACITY);
    for (std::size_t other = 0U; other < CAPACITY; ++other) {
        if (other == index || !entries_[other].valid ||
            entries_[other].pixels == NULL) continue;
        if (entries_[other].rank < previous) ++entries_[other].rank;
    }
    entries_[index].rank = 0U;
}

bool DecodedTileCache::get(const Hardware::TDeck::TileKey& key,
                           std::uint16_t* output,
                           std::size_t pixel_count) {
    if (output == NULL || pixel_count != pixel_count_) return false;
    const int found = find(key);
    if (found < 0) return false;
    const std::size_t index = static_cast<std::size_t>(found);
    std::memcpy(output, entries_[index].pixels,
                pixel_count_ * sizeof(std::uint16_t));
    touch(index);
    return true;
}

bool DecodedTileCache::put(const Hardware::TDeck::TileKey& key,
                           const std::uint16_t* input,
                           std::size_t pixel_count) {
    if (input == NULL || pixel_count != pixel_count_) return false;
    int target = find(key);
    if (target < 0) target = selectTarget();
    if (target < 0) return false;
    const std::size_t index = static_cast<std::size_t>(target);
    std::memcpy(entries_[index].pixels, input,
                pixel_count_ * sizeof(std::uint16_t));
    entries_[index].key = key;
    touch(index);
    entries_[index].valid = true;
    return true;
}

void DecodedTileCache::clear() {
    for (std::size_t index = 0U; index < CAPACITY; ++index) {
        entries_[index].valid = false;
        entries_[index].rank = 0U;
    }
}

std::size_t DecodedTileCache::validCount() const {
    std::size_t count = 0U;
    for (std::size_t index = 0U; index < CAPACITY; ++index) {
        if (entries_[index].valid && entries_[index].pixels != NULL) ++count;
    }
    return count;
}

std::size_t DecodedTileCache::attachedCount() const {
    std::size_t count = 0U;
    for (std::size_t index = 0U; index < CAPACITY; ++index) {
        if (entries_[index].pixels != NULL) ++count;
    }
    return count;
}

}  // namespace Pyxis
