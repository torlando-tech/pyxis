// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_MAP_TILE_STORE_H
#define HARDWARE_TDECK_MAP_TILE_STORE_H

#include <cstddef>
#include <cstdint>

namespace Hardware {
namespace TDeck {

enum class TileStoreResult : std::uint8_t {
    OK,
    MISS,
    INVALID_KEY,
    INVALID_ARGUMENT,
    STORAGE_UNAVAILABLE,
    IO_ERROR,
    INVALID_PNG,
    TOO_LARGE,
    QUOTA_EXCEEDED,
    INDEX_FULL,
    INDEX_MISMATCH,
    NOT_INITIALIZED,
    BUSY
};

struct TileKey {
    std::uint8_t zoom;
    std::uint32_t x;
    std::uint32_t y;
};

struct TileStoreConfig {
    std::uint16_t max_entries;
    std::uint32_t byte_quota;
    std::uint32_t max_tile_bytes;
};

/** Storage boundary used by the portable fixed-capacity cache core. */
class MapTileStorage {
public:
    virtual ~MapTileStorage() {}
    virtual bool isAvailable() const = 0;
    virtual TileStoreResult beginRead(const char* name, std::uint32_t& size) = 0;
    virtual TileStoreResult readChunk(std::uint8_t* output, std::size_t capacity, std::size_t& count) = 0;
    virtual void endRead() = 0;
    virtual TileStoreResult beginWrite(const char* name) = 0;
    virtual TileStoreResult writeChunk(const std::uint8_t* data, std::size_t size, std::size_t& written) = 0;
    virtual TileStoreResult commitWrite() = 0;
    virtual void abortWrite() = 0;
    virtual TileStoreResult remove(const char* name) = 0;
    virtual TileStoreResult rename(const char* from, const char* to) = 0;
    virtual TileStoreResult stat(const char* name, std::uint32_t& size) = 0;
    virtual TileStoreResult beginList() = 0;
    virtual TileStoreResult nextList(char* name, std::size_t capacity, bool& done) = 0;
    virtual void endList() = 0;
};

/**
 * Bounded, allocation-free offline slippy-map tile store.
 *
 * initialize() reconstructs a fixed index from the owned SD namespace.  It
 * rejects malformed/unaccounted names, over-capacity media, and over-quota
 * media rather than allowing pre-existing files to escape accounting.  LRU
 * order after reboot is the canonical TileKey order and is therefore
 * deterministic; successful reads/writes advance the in-memory sequence.
 */
class MapTileStore {
public:
    static const std::uint8_t MAX_ZOOM = 22U;
    static const std::size_t PATH_CAPACITY = 64U;
    static const std::uint16_t HARD_MAX_ENTRIES = 128U;

    MapTileStore(MapTileStorage& storage, const TileStoreConfig& config);

    TileStoreResult initialize();
    static bool isValidKey(const TileKey& key);
    static TileStoreResult canonicalPath(const TileKey& key, char* output, std::size_t capacity);

    TileStoreResult beginGet(const TileKey& key, std::uint32_t& size);
    TileStoreResult readGetChunk(std::uint8_t* output, std::size_t capacity, std::size_t& count);
    void endGet();

    TileStoreResult beginPut(const TileKey& key);
    TileStoreResult writePutChunk(const std::uint8_t* data, std::size_t size);
    TileStoreResult finishPut();
    void abortPut();

    std::uint16_t entryCount() const { return entry_count_; }
    std::uint32_t totalBytes() const { return total_bytes_; }
    static std::size_t ramBytes() { return sizeof(MapTileStore); }

private:
    struct Entry {
        TileKey key;
        std::uint32_t size;
        std::uint64_t sequence;
        std::uint8_t recovery_flags;
    };

    enum RecoveryFlag {
        HAS_LIVE = 1,
        HAS_TEMP = 2,
        HAS_BACKUP = 4
    };

    MapTileStorage& storage_;
    TileStoreConfig config_;
    Entry entries_[HARD_MAX_ENTRIES];
    std::uint16_t entry_count_;
    std::uint32_t total_bytes_;
    std::uint64_t next_sequence_;
    bool initialized_;
    bool read_open_;
    bool write_open_;
    TileKey put_key_;
    char put_live_[PATH_CAPACITY];
    char put_temp_[PATH_CAPACITY];
    char put_backup_[PATH_CAPACITY];
    std::uint8_t png_header_[24];
    std::size_t png_header_count_;
    std::uint32_t put_size_;

    static bool sameKey(const TileKey& a, const TileKey& b);
    static bool keyLess(const TileKey& a, const TileKey& b);
    static TileStoreResult parseOwnedPath(const char* name, TileKey& key, std::uint8_t& flag);
    int findEntry(const TileKey& key) const;
    void removeEntry(std::uint16_t index);
    TileStoreResult validatePathHeader(const char* path, std::uint32_t& size);
    TileStoreResult validateLiveHeader(const Entry& entry);
    TileStoreResult recoverIndex();
    TileStoreResult evictFor(const TileKey& key, std::uint32_t new_size);
    bool validPngHeader() const;
    void failPut();
};

} // namespace TDeck
} // namespace Hardware

#endif
