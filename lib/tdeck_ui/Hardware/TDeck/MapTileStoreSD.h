// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_MAP_TILE_STORE_SD_H
#define HARDWARE_TDECK_MAP_TILE_STORE_SD_H

#include "MapTileStore.h"

#ifdef ARDUINO
#include "SDAccess.h"
#include <Arduino.h>
#include <FS.h>
#include <SD.h>

namespace Hardware {
namespace TDeck {

/**
 * MapTileStorage adapter for the already-mounted SD card owned by SDAccess.
 * It never mounts, remounts, or formats media. Each filesystem/chunk action
 * takes and releases the shared SPI mutex so display and radio traffic can
 * run between chunks.
 */
class MapTileStoreSD : public MapTileStorage {
public:
    MapTileStoreSD();
    virtual ~MapTileStoreSD();

    virtual bool isAvailable() const;
    virtual TileStoreResult beginRead(const char* name, std::uint32_t& size);
    virtual TileStoreResult readChunk(std::uint8_t* output, std::size_t capacity, std::size_t& count);
    virtual void endRead();
    virtual TileStoreResult beginWrite(const char* name);
    virtual TileStoreResult writeChunk(const std::uint8_t* data, std::size_t size, std::size_t& written);
    virtual TileStoreResult commitWrite();
    virtual void abortWrite();
    virtual TileStoreResult remove(const char* name);
    virtual TileStoreResult rename(const char* from, const char* to);
    virtual TileStoreResult stat(const char* name, std::uint32_t& size);
    virtual TileStoreResult beginList();
    virtual TileStoreResult nextList(char* name, std::size_t capacity, bool& done);
    virtual void endList();

private:
    fs::File stream_;
    fs::File list_root_;
    fs::File list_zoom_;
    fs::File list_x_;
    bool writing_;
    bool healthy_;

    static bool cardPresentLocked();
    static TileStoreResult copyName(const char* source, char* output, std::size_t capacity);
    static bool makeParentDirectoriesLocked(const char* name);
};

} // namespace TDeck
} // namespace Hardware
#endif // ARDUINO
#endif
