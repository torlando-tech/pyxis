// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#include "MapTileStoreSD.h"

#ifdef ARDUINO
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Hardware {
namespace TDeck {

namespace {
bool makeMountedPath(const char* name, char* mounted, std::size_t capacity) {
    if ((name == NULL) || (mounted == NULL)) return false;
    const int written = std::snprintf(mounted, capacity, "/sd%s", name);
    return (written >= 0) && (static_cast<std::size_t>(written) < capacity);
}
TileStoreResult statMountedPathLocked(const char* name, struct stat& info) {
    char mounted[MapTileStore::PATH_CAPACITY + 4U] = {};
    if (!makeMountedPath(name, mounted, sizeof(mounted))) return TileStoreResult::INVALID_ARGUMENT;
    errno = 0;
    if (::stat(mounted, &info) == 0) return TileStoreResult::OK;
    return (errno == ENOENT) ? TileStoreResult::MISS : TileStoreResult::IO_ERROR;
}
}

MapTileStoreSD::MapTileStoreSD()
    : stream_(), list_root_(), list_zoom_(), list_x_(), write_fd_(-1), writing_(false), healthy_(true) {}
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
    struct stat info = {};
    const TileStoreResult present = statMountedPathLocked(name, info);
    if (present != TileStoreResult::OK) { SDAccess::release_bus(); return present; }
    stream_ = SD.open(name, FILE_READ);
    if (!stream_) { SDAccess::release_bus(); return TileStoreResult::IO_ERROR; }
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
    char mounted[MapTileStore::PATH_CAPACITY + 4U] = {};
    if (!makeMountedPath(name, mounted, sizeof(mounted))) { SDAccess::release_bus(); return TileStoreResult::INVALID_ARGUMENT; }
    write_fd_ = ::open(mounted, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    writing_ = (write_fd_ >= 0);
    SDAccess::release_bus();
    return writing_ ? TileStoreResult::OK : TileStoreResult::IO_ERROR;
}

TileStoreResult MapTileStoreSD::writeChunk(const std::uint8_t* data, std::size_t size, std::size_t& written) {
    if (!healthy_ || (write_fd_ < 0) || !writing_) return TileStoreResult::IO_ERROR;
    if (!SDAccess::acquire_bus(500U)) return TileStoreResult::STORAGE_UNAVAILABLE;
    if (!cardPresentLocked()) { SDAccess::release_bus(); return TileStoreResult::STORAGE_UNAVAILABLE; }
    const ssize_t result = ::write(write_fd_, data, size);
    written = (result < 0) ? 0U : static_cast<std::size_t>(result);
    SDAccess::release_bus();
    return (written == size) ? TileStoreResult::OK : TileStoreResult::IO_ERROR;
}

TileStoreResult MapTileStoreSD::commitWrite() {
    if (!healthy_ || (write_fd_ < 0) || !writing_) return TileStoreResult::IO_ERROR;
    if (!SDAccess::acquire_bus(500U)) return TileStoreResult::STORAGE_UNAVAILABLE;
    if (!cardPresentLocked()) { SDAccess::release_bus(); return TileStoreResult::STORAGE_UNAVAILABLE; }
    const bool synced = (::fsync(write_fd_) == 0);
    const bool closed = (::close(write_fd_) == 0);
    write_fd_ = -1;
    writing_ = false;
    SDAccess::release_bus();
    return (synced && closed) ? TileStoreResult::OK : TileStoreResult::IO_ERROR;
}

void MapTileStoreSD::abortWrite() {
    if ((write_fd_ < 0) || !writing_) return;
    if (SDAccess::acquire_bus(500U)) {
        if (::close(write_fd_) != 0) healthy_ = false;
        write_fd_ = -1;
        writing_ = false;
        SDAccess::release_bus();
    } else {
        healthy_ = false;
    }
}

TileStoreResult MapTileStoreSD::remove(const char* name) {
    if (!healthy_ || !SDAccess::is_ready() || !SDAccess::acquire_bus(500U)) return TileStoreResult::STORAGE_UNAVAILABLE;
    if (!cardPresentLocked()) { SDAccess::release_bus(); return TileStoreResult::STORAGE_UNAVAILABLE; }
    struct stat info = {};
    const TileStoreResult present = statMountedPathLocked(name, info);
    if (present != TileStoreResult::OK) { SDAccess::release_bus(); return present; }
    const bool removed = SD.remove(name);
    SDAccess::release_bus();
    return removed ? TileStoreResult::OK : TileStoreResult::IO_ERROR;
}

TileStoreResult MapTileStoreSD::rename(const char* from, const char* to) {
    if (!healthy_ || !SDAccess::is_ready() || !SDAccess::acquire_bus(500U)) return TileStoreResult::STORAGE_UNAVAILABLE;
    if (!cardPresentLocked()) { SDAccess::release_bus(); return TileStoreResult::STORAGE_UNAVAILABLE; }
    struct stat source_info = {}, destination_info = {};
    const TileStoreResult source = statMountedPathLocked(from, source_info);
    if (source != TileStoreResult::OK) { SDAccess::release_bus(); return source; }
    const TileStoreResult destination = statMountedPathLocked(to, destination_info);
    if ((destination != TileStoreResult::MISS) && (destination != TileStoreResult::OK)) {
        SDAccess::release_bus(); return destination;
    }
    if ((destination == TileStoreResult::OK) && !SD.remove(to)) { SDAccess::release_bus(); return TileStoreResult::IO_ERROR; }
    const bool renamed = SD.rename(from, to);
    SDAccess::release_bus();
    return renamed ? TileStoreResult::OK : TileStoreResult::IO_ERROR;
}

TileStoreResult MapTileStoreSD::stat(const char* name, std::uint32_t& size) {
    if (!healthy_ || !SDAccess::is_ready() || !SDAccess::acquire_bus(500U)) return TileStoreResult::STORAGE_UNAVAILABLE;
    if (!cardPresentLocked()) { SDAccess::release_bus(); return TileStoreResult::STORAGE_UNAVAILABLE; }
    struct stat info = {};
    const TileStoreResult present = statMountedPathLocked(name, info);
    SDAccess::release_bus();
    if (present != TileStoreResult::OK) return present;
    if ((info.st_size < 0) || (static_cast<std::uint64_t>(info.st_size) > UINT32_MAX)) return TileStoreResult::TOO_LARGE;
    size = static_cast<std::uint32_t>(info.st_size);
    return TileStoreResult::OK;
}

TileStoreResult MapTileStoreSD::beginList() {
    endList();
    if (!healthy_ || !SDAccess::is_ready() || !SDAccess::acquire_bus(500U)) return TileStoreResult::STORAGE_UNAVAILABLE;
    if (!cardPresentLocked()) { SDAccess::release_bus(); return TileStoreResult::STORAGE_UNAVAILABLE; }
    struct stat info = {};
    const TileStoreResult present = statMountedPathLocked("/pyxis-map/tiles", info);
    if (present == TileStoreResult::MISS) { SDAccess::release_bus(); return TileStoreResult::OK; }
    if ((present != TileStoreResult::OK) || !S_ISDIR(info.st_mode)) {
        SDAccess::release_bus(); return TileStoreResult::IO_ERROR;
    }
    list_root_ = SD.open("/pyxis-map/tiles", FILE_READ);
    if (!list_root_) { SDAccess::release_bus(); return TileStoreResult::IO_ERROR; }
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
            errno = 0;
            fs::File item = list_x_.openNextFile();
            if (item) {
                const TileStoreResult copied = copyName(item.path(), name, capacity);
                item.close(); SDAccess::release_bus(); return copied;
            }
            if (errno != 0) { SDAccess::release_bus(); return TileStoreResult::IO_ERROR; }
            list_x_.close();
        }
        if (list_zoom_) {
            errno = 0;
            fs::File item = list_zoom_.openNextFile();
            if (item) {
                if (item.isDirectory()) { list_x_ = item; continue; }
                const TileStoreResult copied = copyName(item.path(), name, capacity);
                item.close(); SDAccess::release_bus(); return copied;
            }
            if (errno != 0) { SDAccess::release_bus(); return TileStoreResult::IO_ERROR; }
            list_zoom_.close();
        }
        errno = 0;
        fs::File item = list_root_.openNextFile();
        if (item) {
            if (item.isDirectory()) { list_zoom_ = item; continue; }
            const TileStoreResult copied = copyName(item.path(), name, capacity);
            item.close(); SDAccess::release_bus(); return copied;
        }
        if (errno != 0) { SDAccess::release_bus(); return TileStoreResult::IO_ERROR; }
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
