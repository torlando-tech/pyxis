// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#include "MapTileStoreSD.h"

#ifdef ARDUINO
#include <cstring>

namespace Hardware {
namespace TDeck {

MapTileStoreSD::MapTileStoreSD()
    : stream_(), list_root_(), list_zoom_(), list_x_(), writing_(false), healthy_(true) {}
MapTileStoreSD::~MapTileStoreSD() { abortWrite(); endRead(); endList(); }

bool MapTileStoreSD::cardPresentLocked() { return SD.cardType() != CARD_NONE; }

bool MapTileStoreSD::isAvailable() const {
    if (!healthy_) return false;
    if (!SDAccess::is_ready() || !SDAccess::acquire_bus(100U)) return false;
    const bool present = cardPresentLocked();
    SDAccess::release_bus();
    return present;
}

TileStoreResult MapTileStoreSD::copyName(const char* source, char* output, std::size_t capacity) {
    if ((source == NULL) || (output == NULL)) return TileStoreResult::INVALID_ARGUMENT;
    const std::size_t length = std::strlen(source);
    if ((length + 1U) > capacity) return TileStoreResult::INDEX_MISMATCH;
    std::memcpy(output, source, length + 1U);
    return TileStoreResult::OK;
}

bool MapTileStoreSD::makeParentDirectoriesLocked(const char* name) {
    char part[MapTileStore::PATH_CAPACITY] = {};
    const std::size_t length = std::strlen(name);
    if (length >= sizeof(part)) return false;
    std::memcpy(part, name, length + 1U);
    for (std::size_t i = 1U; i < length; ++i) {
        if (part[i] == '/') {
            part[i] = '\0';
            if (!SD.exists(part) && !SD.mkdir(part)) return false;
            part[i] = '/';
        }
    }
    return true;
}

TileStoreResult MapTileStoreSD::beginRead(const char* name, std::uint32_t& size) {
    if (!healthy_ || !SDAccess::is_ready() || !SDAccess::acquire_bus(500U)) return TileStoreResult::STORAGE_UNAVAILABLE;
    if (!cardPresentLocked()) { SDAccess::release_bus(); return TileStoreResult::STORAGE_UNAVAILABLE; }
    stream_ = SD.open(name, FILE_READ);
    if (!stream_) { SDAccess::release_bus(); return TileStoreResult::MISS; }
    const std::size_t file_size = stream_.size();
    if (file_size > UINT32_MAX) { stream_.close(); SDAccess::release_bus(); return TileStoreResult::TOO_LARGE; }
    size = static_cast<std::uint32_t>(file_size);
    writing_ = false;
    SDAccess::release_bus();
    return TileStoreResult::OK;
}

TileStoreResult MapTileStoreSD::readChunk(std::uint8_t* output, std::size_t capacity, std::size_t& count) {
    if (!healthy_ || !stream_) return TileStoreResult::IO_ERROR;
    if (!SDAccess::acquire_bus(500U)) return TileStoreResult::STORAGE_UNAVAILABLE;
    if (!cardPresentLocked()) { SDAccess::release_bus(); return TileStoreResult::STORAGE_UNAVAILABLE; }
    count = stream_.read(output, capacity);
    const bool failed = (count == 0U) && stream_.available();
    SDAccess::release_bus();
    return failed ? TileStoreResult::IO_ERROR : TileStoreResult::OK;
}

void MapTileStoreSD::endRead() {
    if (!stream_ || writing_) return;
    if (SDAccess::acquire_bus(500U)) {
        stream_.close();
        SDAccess::release_bus();
    } else {
        healthy_ = false;
    }
}

TileStoreResult MapTileStoreSD::beginWrite(const char* name) {
    if (!healthy_ || !SDAccess::is_ready() || !SDAccess::acquire_bus(500U)) return TileStoreResult::STORAGE_UNAVAILABLE;
    if (!cardPresentLocked()) { SDAccess::release_bus(); return TileStoreResult::STORAGE_UNAVAILABLE; }
    if (!makeParentDirectoriesLocked(name)) { SDAccess::release_bus(); return TileStoreResult::IO_ERROR; }
    stream_ = SD.open(name, FILE_WRITE);
    writing_ = static_cast<bool>(stream_);
    SDAccess::release_bus();
    return writing_ ? TileStoreResult::OK : TileStoreResult::IO_ERROR;
}

TileStoreResult MapTileStoreSD::writeChunk(const std::uint8_t* data, std::size_t size, std::size_t& written) {
    if (!healthy_ || !stream_ || !writing_) return TileStoreResult::IO_ERROR;
    if (!SDAccess::acquire_bus(500U)) return TileStoreResult::STORAGE_UNAVAILABLE;
    if (!cardPresentLocked()) { SDAccess::release_bus(); return TileStoreResult::STORAGE_UNAVAILABLE; }
    written = stream_.write(data, size);
    SDAccess::release_bus();
    return (written == size) ? TileStoreResult::OK : TileStoreResult::IO_ERROR;
}

TileStoreResult MapTileStoreSD::commitWrite() {
    if (!healthy_ || !stream_ || !writing_) return TileStoreResult::IO_ERROR;
    if (!SDAccess::acquire_bus(500U)) return TileStoreResult::STORAGE_UNAVAILABLE;
    if (!cardPresentLocked()) { SDAccess::release_bus(); return TileStoreResult::STORAGE_UNAVAILABLE; }
    stream_.flush();
    stream_.close();
    writing_ = false;
    SDAccess::release_bus();
    return TileStoreResult::OK;
}

void MapTileStoreSD::abortWrite() {
    if (!stream_ || !writing_) return;
    if (SDAccess::acquire_bus(500U)) {
        stream_.close();
        writing_ = false;
        SDAccess::release_bus();
    } else {
        healthy_ = false;
    }
}

TileStoreResult MapTileStoreSD::remove(const char* name) {
    if (!healthy_ || !SDAccess::is_ready() || !SDAccess::acquire_bus(500U)) return TileStoreResult::STORAGE_UNAVAILABLE;
    if (!cardPresentLocked()) { SDAccess::release_bus(); return TileStoreResult::STORAGE_UNAVAILABLE; }
    const bool existed = SD.exists(name);
    const bool removed = !existed || SD.remove(name);
    SDAccess::release_bus();
    return !existed ? TileStoreResult::MISS : (removed ? TileStoreResult::OK : TileStoreResult::IO_ERROR);
}

TileStoreResult MapTileStoreSD::rename(const char* from, const char* to) {
    if (!healthy_ || !SDAccess::is_ready() || !SDAccess::acquire_bus(500U)) return TileStoreResult::STORAGE_UNAVAILABLE;
    if (!cardPresentLocked()) { SDAccess::release_bus(); return TileStoreResult::STORAGE_UNAVAILABLE; }
    if (!SD.exists(from)) { SDAccess::release_bus(); return TileStoreResult::MISS; }
    if (SD.exists(to) && !SD.remove(to)) { SDAccess::release_bus(); return TileStoreResult::IO_ERROR; }
    const bool renamed = SD.rename(from, to);
    SDAccess::release_bus();
    return renamed ? TileStoreResult::OK : TileStoreResult::IO_ERROR;
}

TileStoreResult MapTileStoreSD::stat(const char* name, std::uint32_t& size) {
    if (!healthy_ || !SDAccess::is_ready() || !SDAccess::acquire_bus(500U)) return TileStoreResult::STORAGE_UNAVAILABLE;
    if (!cardPresentLocked()) { SDAccess::release_bus(); return TileStoreResult::STORAGE_UNAVAILABLE; }
    fs::File file = SD.open(name, FILE_READ);
    if (!file) { SDAccess::release_bus(); return TileStoreResult::MISS; }
    const std::size_t file_size = file.size();
    file.close();
    SDAccess::release_bus();
    if (file_size > UINT32_MAX) return TileStoreResult::TOO_LARGE;
    size = static_cast<std::uint32_t>(file_size);
    return TileStoreResult::OK;
}

TileStoreResult MapTileStoreSD::beginList() {
    endList();
    if (!healthy_ || !SDAccess::is_ready() || !SDAccess::acquire_bus(500U)) return TileStoreResult::STORAGE_UNAVAILABLE;
    if (!cardPresentLocked()) { SDAccess::release_bus(); return TileStoreResult::STORAGE_UNAVAILABLE; }
    list_root_ = SD.open("/pyxis-map/tiles", FILE_READ);
    SDAccess::release_bus();
    return TileStoreResult::OK; // An absent cache directory is an empty cache.
}

TileStoreResult MapTileStoreSD::nextList(char* name, std::size_t capacity, bool& done) {
    done = false;
    if (!healthy_) return TileStoreResult::STORAGE_UNAVAILABLE;
    if (!list_root_) { done = true; return TileStoreResult::OK; }
    if (!SDAccess::acquire_bus(500U)) return TileStoreResult::STORAGE_UNAVAILABLE;
    if (!cardPresentLocked()) { SDAccess::release_bus(); return TileStoreResult::STORAGE_UNAVAILABLE; }
    while (true) {
        if (list_x_) {
            fs::File item = list_x_.openNextFile();
            if (item) {
                const TileStoreResult copied = copyName(item.path(), name, capacity);
                item.close(); SDAccess::release_bus(); return copied;
            }
            list_x_.close();
        }
        if (list_zoom_) {
            fs::File item = list_zoom_.openNextFile();
            if (item) {
                if (item.isDirectory()) { list_x_ = item; continue; }
                const TileStoreResult copied = copyName(item.path(), name, capacity);
                item.close(); SDAccess::release_bus(); return copied;
            }
            list_zoom_.close();
        }
        fs::File item = list_root_.openNextFile();
        if (item) {
            if (item.isDirectory()) { list_zoom_ = item; continue; }
            const TileStoreResult copied = copyName(item.path(), name, capacity);
            item.close(); SDAccess::release_bus(); return copied;
        }
        done = true;
        SDAccess::release_bus();
        return TileStoreResult::OK;
    }
}

void MapTileStoreSD::endList() {
    if (!list_root_ && !list_zoom_ && !list_x_) return;
    if (SDAccess::acquire_bus(500U)) {
        if (list_x_) list_x_.close();
        if (list_zoom_) list_zoom_.close();
        if (list_root_) list_root_.close();
        SDAccess::release_bus();
    } else {
        healthy_ = false;
    }
}

} // namespace TDeck
} // namespace Hardware
#endif // ARDUINO
