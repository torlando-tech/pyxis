// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#include "MapTileStore.h"

#include <cstdio>
#include <cstring>

namespace Hardware {
namespace TDeck {

namespace {
const char TILE_PREFIX[] = "/pyxis-map/tiles/";
const char LIVE_SUFFIX[] = ".png";
const char TEMP_SUFFIX[] = ".png.tmp";
const char BACKUP_SUFFIX[] = ".png.bak";
const char EVICT_SUFFIX[] = ".png.evict";
const char EVICTION_TRANSACTION[] = "/pyxis-map/tiles/.evict.txn";
const char EVICTION_TRANSACTION_TEMP[] = "/pyxis-map/tiles/.evict.txn.tmp";
const std::size_t TRANSACTION_BASE_BYTES = 21U;
const std::size_t TRANSACTION_KEY_BYTES = 9U;
const std::size_t TRANSACTION_MAX_BYTES =
    TRANSACTION_BASE_BYTES + (MapTileStore::HARD_MAX_ENTRIES * TRANSACTION_KEY_BYTES);

bool appendSuffix(const char* live, const char* suffix, char* output, std::size_t capacity) {
    const std::size_t a = std::strlen(live);
    const std::size_t b = std::strlen(suffix);
    if ((a + b + 1U) > capacity) return false;
    std::memcpy(output, live, a);
    std::memcpy(output + a, suffix, b + 1U);
    return true;
}

bool parseNumber(const char*& cursor, char delimiter, std::uint32_t& value) {
    if ((*cursor < '0') || (*cursor > '9')) return false;
    if ((*cursor == '0') && (cursor[1] != delimiter)) return false;
    std::uint32_t result = 0U;
    while ((*cursor >= '0') && (*cursor <= '9')) {
        const std::uint32_t digit = static_cast<std::uint32_t>(*cursor - '0');
        if (result > ((UINT32_MAX - digit) / 10U)) return false;
        result = (result * 10U) + digit;
        ++cursor;
    }
    if (*cursor != delimiter) return false;
    ++cursor;
    value = result;
    return true;
}

void writeU32(std::uint8_t* output, std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value >> 24U);
    output[1] = static_cast<std::uint8_t>(value >> 16U);
    output[2] = static_cast<std::uint8_t>(value >> 8U);
    output[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t readU32(const std::uint8_t* input) {
    return (static_cast<std::uint32_t>(input[0]) << 24U) |
           (static_cast<std::uint32_t>(input[1]) << 16U) |
           (static_cast<std::uint32_t>(input[2]) << 8U) |
           static_cast<std::uint32_t>(input[3]);
}

std::uint32_t transactionCrc(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = UINT32_C(0xffffffff);
    for (std::size_t i = 0U; i < size; ++i) {
        crc ^= data[i];
        for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) ? UINT32_C(0xedb88320) : 0U);
        }
    }
    return ~crc;
}

void encodeKey(std::uint8_t* output, const TileKey& key) {
    output[0] = key.zoom;
    writeU32(output + 1U, key.x);
    writeU32(output + 5U, key.y);
}

TileKey decodeKey(const std::uint8_t* input) {
    const TileKey key = {input[0], readU32(input + 1U), readU32(input + 5U)};
    return key;
}
}

MapTileStore::MapTileStore(MapTileStorage& storage, const TileStoreConfig& config)
    : storage_(storage), config_(config), entry_count_(0U), total_bytes_(0U), next_sequence_(1U),
      initialized_(false), read_open_(false), write_open_(false), put_key_{0U, 0U, 0U},
      put_live_{0}, put_temp_{0}, put_backup_{0}, png_header_{0}, png_header_count_(0U), put_size_(0U) {}

bool MapTileStore::sameKey(const TileKey& a, const TileKey& b) {
    return (a.zoom == b.zoom) && (a.x == b.x) && (a.y == b.y);
}

bool MapTileStore::keyLess(const TileKey& a, const TileKey& b) {
    if (a.zoom != b.zoom) return a.zoom < b.zoom;
    if (a.x != b.x) return a.x < b.x;
    return a.y < b.y;
}

bool MapTileStore::isValidKey(const TileKey& key) {
    if (key.zoom > MAX_ZOOM) return false;
    const std::uint32_t count = UINT32_C(1) << key.zoom;
    return (key.x < count) && (key.y < count);
}

TileStoreResult MapTileStore::canonicalPath(const TileKey& key, char* output, std::size_t capacity) {
    if (!isValidKey(key)) return TileStoreResult::INVALID_KEY;
    if ((output == NULL) || (capacity == 0U)) return TileStoreResult::INVALID_ARGUMENT;
    const int count = std::snprintf(output, capacity, "/pyxis-map/tiles/%u/%lu/%lu.png",
                                    static_cast<unsigned>(key.zoom), static_cast<unsigned long>(key.x),
                                    static_cast<unsigned long>(key.y));
    if ((count < 0) || (static_cast<std::size_t>(count) >= capacity)) return TileStoreResult::INVALID_ARGUMENT;
    return TileStoreResult::OK;
}

TileStoreResult MapTileStore::parseOwnedPath(const char* name, TileKey& key, std::uint8_t& flag) {
    if (name == NULL) return TileStoreResult::INDEX_MISMATCH;
    const std::size_t prefix_size = sizeof(TILE_PREFIX) - 1U;
    if (std::strncmp(name, TILE_PREFIX, prefix_size) != 0) return TileStoreResult::INDEX_MISMATCH;
    const char* cursor = name + prefix_size;
    std::uint32_t zoom = 0U;
    if (!parseNumber(cursor, '/', zoom) || !parseNumber(cursor, '/', key.x)) return TileStoreResult::INDEX_MISMATCH;
    const char* y_start = cursor;
    if ((*cursor < '0') || (*cursor > '9')) return TileStoreResult::INDEX_MISMATCH;
    while ((*cursor >= '0') && (*cursor <= '9')) ++cursor;
    if ((*y_start == '0') && ((cursor - y_start) != 1)) return TileStoreResult::INDEX_MISMATCH;
    std::uint64_t y = 0U;
    for (const char* p = y_start; p != cursor; ++p) {
        y = (y * 10U) + static_cast<std::uint64_t>(*p - '0');
        if (y > UINT32_MAX) return TileStoreResult::INDEX_MISMATCH;
    }
    key.zoom = (zoom <= UINT8_MAX) ? static_cast<std::uint8_t>(zoom) : UINT8_MAX;
    key.y = static_cast<std::uint32_t>(y);
    if (!isValidKey(key)) return TileStoreResult::INDEX_MISMATCH;
    if (std::strcmp(cursor, LIVE_SUFFIX) == 0) flag = HAS_LIVE;
    else if (std::strcmp(cursor, TEMP_SUFFIX) == 0) flag = HAS_TEMP;
    else if (std::strcmp(cursor, BACKUP_SUFFIX) == 0) flag = HAS_BACKUP;
    else if (std::strcmp(cursor, EVICT_SUFFIX) == 0) flag = HAS_EVICT;
    else return TileStoreResult::INDEX_MISMATCH;
    char canonical[PATH_CAPACITY] = {};
    if ((canonicalPath(key, canonical, sizeof(canonical)) != TileStoreResult::OK) ||
        (std::strncmp(name, canonical, std::strlen(canonical)) != 0)) return TileStoreResult::INDEX_MISMATCH;
    return TileStoreResult::OK;
}

int MapTileStore::findEntry(const TileKey& key) const {
    for (std::uint16_t i = 0U; i < entry_count_; ++i) if (sameKey(entries_[i].key, key)) return static_cast<int>(i);
    return -1;
}

void MapTileStore::removeEntry(std::uint16_t index) {
    total_bytes_ -= entries_[index].size;
    for (std::uint16_t i = index; (i + 1U) < entry_count_; ++i) entries_[i] = entries_[i + 1U];
    --entry_count_;
}

TileStoreResult MapTileStore::validatePathHeader(const char* path, std::uint32_t& size) {
    TileStoreResult result = storage_.beginRead(path, size);
    if (result != TileStoreResult::OK) return result;
    std::uint8_t header[24] = {};
    std::size_t total = 0U;
    while (total < sizeof(header)) {
        std::size_t got = 0U;
        result = storage_.readChunk(header + total, sizeof(header) - total, got);
        if (result != TileStoreResult::OK) { storage_.endRead(); return result; }
        if (got == 0U) break;
        total += got;
    }
    storage_.endRead();
    if (total != sizeof(header)) return TileStoreResult::INDEX_MISMATCH;
    static const std::uint8_t signature[8] = {137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
    if ((std::memcmp(header, signature, sizeof(signature)) != 0) || header[8] != 0U || header[9] != 0U ||
        header[10] != 0U || header[11] != 13U || std::memcmp(header + 12U, "IHDR", 4U) != 0 ||
        header[16] != 0U || header[17] != 0U || header[18] != 1U || header[19] != 0U ||
        header[20] != 0U || header[21] != 0U || header[22] != 1U || header[23] != 0U) {
        return TileStoreResult::INDEX_MISMATCH;
    }
    return TileStoreResult::OK;
}

TileStoreResult MapTileStore::validateLiveHeader(const Entry& entry) {
    char name[PATH_CAPACITY] = {};
    if (canonicalPath(entry.key, name, sizeof(name)) != TileStoreResult::OK) {
        return TileStoreResult::INDEX_MISMATCH;
    }
    std::uint32_t size = 0U;
    const TileStoreResult result = validatePathHeader(name, size);
    if (result != TileStoreResult::OK) return result;
    return (size == entry.size) ? TileStoreResult::OK : TileStoreResult::INDEX_MISMATCH;
}

TileStoreResult MapTileStore::writeEvictionTransaction(
        const TileKey& key, bool duplicate,
        const TileKey* victims, std::uint16_t victim_count) {
    if ((victims == NULL) || (victim_count == 0U) ||
        (victim_count > HARD_MAX_ENTRIES)) return TileStoreResult::INVALID_ARGUMENT;
    std::uint8_t record[TRANSACTION_MAX_BYTES] = {};
    const std::size_t size = TRANSACTION_BASE_BYTES +
        (static_cast<std::size_t>(victim_count) * TRANSACTION_KEY_BYTES);
    record[0] = 'P'; record[1] = 'Y'; record[2] = 'E'; record[3] = 'V';
    record[4] = 1U;
    record[5] = duplicate ? 1U : 0U;
    record[6] = static_cast<std::uint8_t>(victim_count >> 8U);
    record[7] = static_cast<std::uint8_t>(victim_count);
    encodeKey(record + 8U, key);
    for (std::uint16_t i = 0U; i < victim_count; ++i) {
        encodeKey(record + 17U + (static_cast<std::size_t>(i) * TRANSACTION_KEY_BYTES),
                  victims[i]);
    }
    writeU32(record + size - 4U, transactionCrc(record, size - 4U));

    TileStoreResult result = storage_.remove(EVICTION_TRANSACTION_TEMP);
    if ((result != TileStoreResult::OK) && (result != TileStoreResult::MISS)) return result;
    result = storage_.remove(EVICTION_TRANSACTION);
    if ((result != TileStoreResult::OK) && (result != TileStoreResult::MISS)) return result;
    result = storage_.beginWrite(EVICTION_TRANSACTION_TEMP);
    if (result != TileStoreResult::OK) return result;
    std::size_t written = 0U;
    result = storage_.writeChunk(record, size, written);
    if ((result != TileStoreResult::OK) || (written != size)) {
        storage_.abortWrite();
        storage_.remove(EVICTION_TRANSACTION_TEMP);
        return (result == TileStoreResult::OK) ? TileStoreResult::IO_ERROR : result;
    }
    result = storage_.commitWrite();
    if (result != TileStoreResult::OK) {
        storage_.abortWrite();
        storage_.remove(EVICTION_TRANSACTION_TEMP);
        return result;
    }
    result = storage_.rename(EVICTION_TRANSACTION_TEMP, EVICTION_TRANSACTION);
    if (result != TileStoreResult::OK) storage_.remove(EVICTION_TRANSACTION_TEMP);
    return result;
}

TileStoreResult MapTileStore::recoverEvictionTransaction() {
    // An unpromoted manifest temp cannot guard any file mutation.
    TileStoreResult result = storage_.remove(EVICTION_TRANSACTION_TEMP);
    if ((result != TileStoreResult::OK) && (result != TileStoreResult::MISS)) return result;
    std::uint32_t size = 0U;
    result = storage_.beginRead(EVICTION_TRANSACTION, size);
    if (result == TileStoreResult::MISS) return TileStoreResult::OK;
    if (result != TileStoreResult::OK) return result;
    if ((size < TRANSACTION_BASE_BYTES) || (size > TRANSACTION_MAX_BYTES)) {
        storage_.endRead();
        return TileStoreResult::INDEX_MISMATCH;
    }
    std::uint8_t record[TRANSACTION_MAX_BYTES] = {};
    std::size_t total = 0U;
    while (total < size) {
        std::size_t got = 0U;
        result = storage_.readChunk(record + total, size - total, got);
        if (result != TileStoreResult::OK) { storage_.endRead(); return result; }
        if (got == 0U) break;
        total += got;
    }
    storage_.endRead();
    const std::uint16_t victim_count = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(record[6]) << 8U) | record[7]);
    const std::size_t expected = TRANSACTION_BASE_BYTES +
        (static_cast<std::size_t>(victim_count) * TRANSACTION_KEY_BYTES);
    if ((total != size) || (size != expected) || (victim_count == 0U) ||
        (victim_count > HARD_MAX_ENTRIES) ||
        std::memcmp(record, "PYEV", 4U) != 0 || record[4] != 1U || record[5] > 1U ||
        readU32(record + size - 4U) != transactionCrc(record, size - 4U)) {
        return TileStoreResult::INDEX_MISMATCH;
    }
    const TileKey candidate = decodeKey(record + 8U);
    if (!isValidKey(candidate)) return TileStoreResult::INDEX_MISMATCH;
    char live[PATH_CAPACITY] = {}, temp[PATH_CAPACITY] = {}, backup[PATH_CAPACITY] = {};
    canonicalPath(candidate, live, sizeof(live));
    appendSuffix(live, ".tmp", temp, sizeof(temp));
    appendSuffix(live, ".bak", backup, sizeof(backup));
    std::uint32_t ignored = 0U;
    if (record[5] != 0U) {
        const TileStoreResult backup_state = storage_.stat(backup, ignored);
        if (backup_state == TileStoreResult::OK) {
            result = storage_.remove(live);
            if ((result != TileStoreResult::OK) && (result != TileStoreResult::MISS)) return result;
            result = storage_.rename(backup, live);
            if (result != TileStoreResult::OK) return result;
        } else if (backup_state != TileStoreResult::MISS) {
            return backup_state;
        }
        result = storage_.remove(temp);
        if ((result != TileStoreResult::OK) && (result != TileStoreResult::MISS)) return result;
    } else {
        result = storage_.remove(live);
        if ((result != TileStoreResult::OK) && (result != TileStoreResult::MISS)) return result;
        result = storage_.remove(temp);
        if ((result != TileStoreResult::OK) && (result != TileStoreResult::MISS)) return result;
    }

    for (std::uint16_t i = 0U; i < victim_count; ++i) {
        const TileKey victim = decodeKey(
            record + 17U + (static_cast<std::size_t>(i) * TRANSACTION_KEY_BYTES));
        if (!isValidKey(victim) || sameKey(victim, candidate)) return TileStoreResult::INDEX_MISMATCH;
        char victim_live[PATH_CAPACITY] = {}, evicted[PATH_CAPACITY] = {};
        canonicalPath(victim, victim_live, sizeof(victim_live));
        appendSuffix(victim_live, ".evict", evicted, sizeof(evicted));
        const TileStoreResult live_state = storage_.stat(victim_live, ignored);
        if (live_state == TileStoreResult::OK) {
            result = storage_.remove(evicted);
            if ((result != TileStoreResult::OK) && (result != TileStoreResult::MISS)) return result;
        } else if (live_state == TileStoreResult::MISS) {
            const TileStoreResult evicted_state = storage_.stat(evicted, ignored);
            if (evicted_state != TileStoreResult::OK) return TileStoreResult::INDEX_MISMATCH;
            result = storage_.rename(evicted, victim_live);
            if (result != TileStoreResult::OK) return result;
        } else {
            return live_state;
        }
    }
    result = storage_.remove(EVICTION_TRANSACTION);
    return ((result == TileStoreResult::OK) || (result == TileStoreResult::MISS))
        ? TileStoreResult::OK : result;
}

TileStoreResult MapTileStore::recoverIndex() {
    TileStoreResult result = recoverEvictionTransaction();
    if (result != TileStoreResult::OK) return result;
    result = storage_.beginList();
    if (result != TileStoreResult::OK) return result;
    while (true) {
        char name[PATH_CAPACITY] = {};
        bool done = false;
        result = storage_.nextList(name, sizeof(name), done);
        if (result != TileStoreResult::OK) { storage_.endList(); return result; }
        if (done) break;
        TileKey key = {0U, 0U, 0U};
        std::uint8_t flag = 0U;
        result = parseOwnedPath(name, key, flag);
        if (result != TileStoreResult::OK) { storage_.endList(); return result; }
        int index = findEntry(key);
        if (index < 0) {
            if (entry_count_ >= config_.max_entries) { storage_.endList(); return TileStoreResult::INDEX_FULL; }
            index = static_cast<int>(entry_count_++);
            entries_[static_cast<std::size_t>(index)] = Entry{key, 0U, 0U, 0U};
        }
        Entry& entry = entries_[static_cast<std::size_t>(index)];
        if ((entry.recovery_flags & flag) != 0U) { storage_.endList(); return TileStoreResult::INDEX_MISMATCH; }
        entry.recovery_flags = static_cast<std::uint8_t>(entry.recovery_flags | flag);
    }
    storage_.endList();

    std::uint16_t i = 0U;
    while (i < entry_count_) {
        Entry& entry = entries_[i];
        char live[PATH_CAPACITY] = {}, temp[PATH_CAPACITY] = {}, backup[PATH_CAPACITY] = {},
             evicted[PATH_CAPACITY] = {};
        canonicalPath(entry.key, live, sizeof(live));
        appendSuffix(live, ".tmp", temp, sizeof(temp));
        appendSuffix(live, ".bak", backup, sizeof(backup));
        appendSuffix(live, ".evict", evicted, sizeof(evicted));

        bool keep = false;
        if ((entry.recovery_flags & HAS_LIVE) != 0U) {
            result = validatePathHeader(live, entry.size);
            if (result == TileStoreResult::OK) {
                keep = true;
            } else if ((result == TileStoreResult::IO_ERROR) ||
                       (result == TileStoreResult::STORAGE_UNAVAILABLE)) {
                return result;
            }
        }

        if (!keep && ((entry.recovery_flags & HAS_BACKUP) != 0U)) {
            std::uint32_t backup_size = 0U;
            result = validatePathHeader(backup, backup_size);
            if (result == TileStoreResult::OK) {
                const TileStoreResult removed = storage_.remove(live);
                if ((removed != TileStoreResult::OK) && (removed != TileStoreResult::MISS)) return removed;
                result = storage_.rename(backup, live);
                if (result != TileStoreResult::OK) return result;
                entry.size = backup_size;
                keep = true;
            } else if ((result == TileStoreResult::IO_ERROR) ||
                       (result == TileStoreResult::STORAGE_UNAVAILABLE)) {
                return result;
            }
        }

        if (keep) {
            const TileStoreResult temp_removed = storage_.remove(temp);
            if ((temp_removed != TileStoreResult::OK) && (temp_removed != TileStoreResult::MISS)) return temp_removed;
            const TileStoreResult backup_removed = storage_.remove(backup);
            if ((backup_removed != TileStoreResult::OK) && (backup_removed != TileStoreResult::MISS)) return backup_removed;
            const TileStoreResult evicted_removed = storage_.remove(evicted);
            if ((evicted_removed != TileStoreResult::OK) && (evicted_removed != TileStoreResult::MISS)) return evicted_removed;
            if ((entry.size > config_.max_tile_bytes) ||
                (UINT32_MAX - total_bytes_ < entry.size)) return TileStoreResult::QUOTA_EXCEEDED;
            total_bytes_ += entry.size;
            ++i;
            continue;
        }

        const TileStoreResult live_removed = storage_.remove(live);
        if ((live_removed != TileStoreResult::OK) && (live_removed != TileStoreResult::MISS)) return live_removed;
        const TileStoreResult temp_removed = storage_.remove(temp);
        if ((temp_removed != TileStoreResult::OK) && (temp_removed != TileStoreResult::MISS)) return temp_removed;
        const TileStoreResult backup_removed = storage_.remove(backup);
        if ((backup_removed != TileStoreResult::OK) && (backup_removed != TileStoreResult::MISS)) return backup_removed;
        const TileStoreResult evicted_removed = storage_.remove(evicted);
        if ((evicted_removed != TileStoreResult::OK) && (evicted_removed != TileStoreResult::MISS)) return evicted_removed;
        for (std::uint16_t j = i; (j + 1U) < entry_count_; ++j) entries_[j] = entries_[j + 1U];
        --entry_count_;
    }
    if (total_bytes_ > config_.byte_quota) return TileStoreResult::QUOTA_EXCEEDED;
    for (std::uint16_t a = 1U; a < entry_count_; ++a) {
        Entry value = entries_[a]; std::uint16_t b = a;
        while ((b > 0U) && keyLess(value.key, entries_[b - 1U].key)) { entries_[b] = entries_[b - 1U]; --b; }
        entries_[b] = value;
    }
    for (std::uint16_t n = 0U; n < entry_count_; ++n) entries_[n].sequence = next_sequence_++;
    return TileStoreResult::OK;
}

TileStoreResult MapTileStore::initialize() {
    if ((config_.max_entries == 0U) || (config_.max_entries > HARD_MAX_ENTRIES) ||
        (config_.byte_quota == 0U) || (config_.max_tile_bytes < 24U)) return TileStoreResult::INVALID_ARGUMENT;
    if (!storage_.isAvailable()) return TileStoreResult::STORAGE_UNAVAILABLE;
    entry_count_ = 0U; total_bytes_ = 0U; next_sequence_ = 1U; initialized_ = false;
    const TileStoreResult result = recoverIndex();
    if (result == TileStoreResult::OK) initialized_ = true;
    return result;
}

TileStoreResult MapTileStore::beginGet(const TileKey& key, std::uint32_t& size) {
    if (!initialized_) return TileStoreResult::NOT_INITIALIZED;
    if (read_open_ || write_open_) return TileStoreResult::BUSY;
    if (!isValidKey(key)) return TileStoreResult::INVALID_KEY;
    if (!storage_.isAvailable()) return TileStoreResult::STORAGE_UNAVAILABLE;
    const int index = findEntry(key);
    if (index < 0) return TileStoreResult::MISS;
    char name[PATH_CAPACITY] = {};
    canonicalPath(key, name, sizeof(name));
    TileStoreResult result = storage_.beginRead(name, size);
    if (result != TileStoreResult::OK) return result;
    if (size != entries_[static_cast<std::size_t>(index)].size) { storage_.endRead(); return TileStoreResult::INDEX_MISMATCH; }
    entries_[static_cast<std::size_t>(index)].sequence = next_sequence_++;
    read_open_ = true;
    return TileStoreResult::OK;
}

TileStoreResult MapTileStore::readGetChunk(std::uint8_t* output, std::size_t capacity, std::size_t& count) {
    if (!read_open_) return TileStoreResult::BUSY;
    if ((output == NULL) && (capacity != 0U)) return TileStoreResult::INVALID_ARGUMENT;
    return storage_.readChunk(output, capacity, count);
}

void MapTileStore::endGet() { if (read_open_) storage_.endRead(); read_open_ = false; }

TileStoreResult MapTileStore::removeTile(const TileKey& key) {
    if (!initialized_) return TileStoreResult::NOT_INITIALIZED;
    if (read_open_ || write_open_) return TileStoreResult::BUSY;
    if (!storage_.isAvailable()) return TileStoreResult::STORAGE_UNAVAILABLE;
    const int index = findEntry(key);
    if (index < 0) return TileStoreResult::MISS;
    char path[PATH_CAPACITY] = {};
    TileStoreResult result = canonicalPath(key, path, sizeof(path));
    if (result != TileStoreResult::OK) return result;
    result = storage_.remove(path);
    if (result != TileStoreResult::OK && result != TileStoreResult::MISS) return result;
    removeEntry(static_cast<std::uint16_t>(index));
    return TileStoreResult::OK;
}

TileStoreResult MapTileStore::beginPut(const TileKey& key) {
    if (!initialized_) return TileStoreResult::NOT_INITIALIZED;
    if (read_open_ || write_open_) return TileStoreResult::BUSY;
    if (!isValidKey(key)) return TileStoreResult::INVALID_KEY;
    if (!storage_.isAvailable()) return TileStoreResult::STORAGE_UNAVAILABLE;
    TileStoreResult result = canonicalPath(key, put_live_, sizeof(put_live_));
    if (result != TileStoreResult::OK) return result;
    if (!appendSuffix(put_live_, ".tmp", put_temp_, sizeof(put_temp_)) ||
        !appendSuffix(put_live_, ".bak", put_backup_, sizeof(put_backup_))) return TileStoreResult::INVALID_ARGUMENT;
    result = storage_.remove(put_temp_);
    if ((result != TileStoreResult::OK) && (result != TileStoreResult::MISS)) return result;
    result = storage_.beginWrite(put_temp_);
    if (result != TileStoreResult::OK) return result;
    put_key_ = key; put_size_ = 0U; png_header_count_ = 0U; std::memset(png_header_, 0, sizeof(png_header_)); write_open_ = true;
    return TileStoreResult::OK;
}

void MapTileStore::failPut() {
    if (write_open_) storage_.abortWrite();
    storage_.remove(put_temp_);
    write_open_ = false;
}

TileStoreResult MapTileStore::writePutChunk(const std::uint8_t* data, std::size_t size) {
    if (!write_open_) return TileStoreResult::BUSY;
    if ((data == NULL) && (size != 0U)) { failPut(); return TileStoreResult::INVALID_ARGUMENT; }
    if ((size > config_.max_tile_bytes) || (put_size_ > config_.max_tile_bytes - static_cast<std::uint32_t>(size))) {
        failPut(); return TileStoreResult::TOO_LARGE;
    }
    const std::size_t needed = sizeof(png_header_) - png_header_count_;
    const std::size_t copy = (size < needed) ? size : needed;
    if (copy != 0U) { std::memcpy(png_header_ + png_header_count_, data, copy); png_header_count_ += copy; }
    std::size_t written = 0U;
    const TileStoreResult result = storage_.writeChunk(data, size, written);
    if ((result != TileStoreResult::OK) || (written != size)) { failPut(); return (result == TileStoreResult::OK) ? TileStoreResult::IO_ERROR : result; }
    put_size_ += static_cast<std::uint32_t>(size);
    return TileStoreResult::OK;
}

bool MapTileStore::validPngHeader() const {
    static const std::uint8_t signature[8] = {137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
    return (png_header_count_ == sizeof(png_header_)) &&
        (std::memcmp(png_header_, signature, sizeof(signature)) == 0) &&
        (png_header_[8] == 0U) && (png_header_[9] == 0U) && (png_header_[10] == 0U) && (png_header_[11] == 13U) &&
        (std::memcmp(png_header_ + 12U, "IHDR", 4U) == 0) &&
        (png_header_[16] == 0U) && (png_header_[17] == 0U) && (png_header_[18] == 1U) && (png_header_[19] == 0U) &&
        (png_header_[20] == 0U) && (png_header_[21] == 0U) && (png_header_[22] == 1U) && (png_header_[23] == 0U);
}

TileStoreResult MapTileStore::planEviction(
        const TileKey& key, std::uint32_t new_size,
        TileKey* victims, std::uint16_t& victim_count) const {
    if (victims == NULL) return TileStoreResult::INVALID_ARGUMENT;
    victim_count = 0U;
    const int existing = findEntry(key);
    std::uint32_t prospective = total_bytes_;
    std::uint16_t count = entry_count_;
    if (existing >= 0) prospective -= entries_[static_cast<std::size_t>(existing)].size;
    else ++count;
    if (UINT32_MAX - prospective < new_size) return TileStoreResult::QUOTA_EXCEEDED;
    prospective += new_size;
    while ((prospective > config_.byte_quota) || (count > config_.max_entries)) {
        int victim = -1;
        for (std::uint16_t i = 0U; i < entry_count_; ++i) {
            if (sameKey(entries_[i].key, key)) continue;
            bool selected = false;
            for (std::uint16_t j = 0U; j < victim_count; ++j) {
                if (sameKey(victims[j], entries_[i].key)) { selected = true; break; }
            }
            if (selected) continue;
            if ((victim < 0) ||
                (entries_[i].sequence < entries_[static_cast<std::size_t>(victim)].sequence)) {
                victim = static_cast<int>(i);
            }
        }
        if (victim < 0) {
            return (prospective > config_.byte_quota)
                ? TileStoreResult::QUOTA_EXCEEDED : TileStoreResult::INDEX_FULL;
        }
        victims[victim_count++] = entries_[static_cast<std::size_t>(victim)].key;
        prospective -= entries_[static_cast<std::size_t>(victim)].size;
        --count;
    }
    return TileStoreResult::OK;
}

TileStoreResult MapTileStore::evictFor(
        const TileKey* victims, std::uint16_t victim_count) {
    for (std::uint16_t i = 0U; i < victim_count; ++i) {
        char live[PATH_CAPACITY] = {}, evicted[PATH_CAPACITY] = {};
        canonicalPath(victims[i], live, sizeof(live));
        appendSuffix(live, ".evict", evicted, sizeof(evicted));
        TileStoreResult result = storage_.remove(evicted);
        if ((result != TileStoreResult::OK) && (result != TileStoreResult::MISS)) return result;
        result = storage_.rename(live, evicted);
        if (result != TileStoreResult::OK) return result;
    }
    for (std::uint16_t i = 0U; i < victim_count; ++i) {
        const int index = findEntry(victims[i]);
        if (index >= 0) removeEntry(static_cast<std::uint16_t>(index));
    }
    return TileStoreResult::OK;
}

void MapTileStore::cleanupEvicted(
        const TileKey* victims, std::uint16_t victim_count) {
    for (std::uint16_t i = 0U; i < victim_count; ++i) {
        char live[PATH_CAPACITY] = {}, evicted[PATH_CAPACITY] = {};
        canonicalPath(victims[i], live, sizeof(live));
        appendSuffix(live, ".evict", evicted, sizeof(evicted));
        storage_.remove(evicted);
    }
}

TileStoreResult MapTileStore::finishPut() {
    if (!write_open_) return TileStoreResult::BUSY;
    if (!validPngHeader()) { failPut(); return TileStoreResult::INVALID_PNG; }
    TileStoreResult result = storage_.commitWrite();
    if (result != TileStoreResult::OK) { failPut(); return result; }
    write_open_ = false;
    int index = findEntry(put_key_);
    const bool duplicate = index >= 0;
    TileKey victims[HARD_MAX_ENTRIES] = {};
    std::uint16_t victim_count = 0U;
    result = planEviction(put_key_, put_size_, victims, victim_count);
    if (result != TileStoreResult::OK) {
        storage_.remove(put_temp_);
        return result;
    }
    const bool transactional_eviction = victim_count != 0U;
    if (duplicate) {
        // A stale backup must not become the rollback generation recorded by a
        // new transaction. Verify its removal before publishing the manifest.
        result = storage_.remove(put_backup_);
        if ((result != TileStoreResult::OK) && (result != TileStoreResult::MISS)) {
            storage_.remove(put_temp_);
            return result;
        }
    }
    if (transactional_eviction) {
        result = writeEvictionTransaction(put_key_, duplicate, victims, victim_count);
        if (result != TileStoreResult::OK) {
            storage_.remove(put_temp_);
            return result;
        }
    }
    if (duplicate) {
        result = storage_.rename(put_live_, put_backup_);
        if (result != TileStoreResult::OK) {
            if (transactional_eviction) recoverEvictionTransaction();
            else storage_.remove(put_temp_);
            return result;
        }
    }
    result = storage_.rename(put_temp_, put_live_);
    if (result != TileStoreResult::OK) {
        if (transactional_eviction) {
            const TileStoreResult recovered = recoverEvictionTransaction();
            if (recovered != TileStoreResult::OK) initialized_ = false;
        } else {
            if (duplicate) storage_.rename(put_backup_, put_live_);
            storage_.remove(put_temp_);
        }
        return result;
    }
    if (transactional_eviction) {
        result = evictFor(victims, victim_count);
        if (result != TileStoreResult::OK) {
            const TileStoreResult recovered = recoverEvictionTransaction();
            if (recovered != TileStoreResult::OK) initialized_ = false;
            return result;
        }
    }
    index = findEntry(put_key_);
    if (duplicate) {
        Entry& entry = entries_[static_cast<std::size_t>(index)];
        total_bytes_ -= entry.size;
        total_bytes_ += put_size_;
        entry.size = put_size_;
        entry.sequence = next_sequence_++;
    } else {
        if (entry_count_ >= config_.max_entries) {
            if (transactional_eviction) recoverEvictionTransaction();
            else storage_.remove(put_live_);
            return TileStoreResult::INDEX_FULL;
        }
        entries_[entry_count_++] = Entry{put_key_, put_size_, next_sequence_++, HAS_LIVE};
        total_bytes_ += put_size_;
    }
    if (transactional_eviction) {
        // Removing the manifest is the transaction-wide commit boundary. Until
        // this succeeds, initialize() restores the complete old generation.
        result = storage_.remove(EVICTION_TRANSACTION);
        if (result != TileStoreResult::OK) {
            const TileStoreResult recovered = recoverEvictionTransaction();
            entry_count_ = 0U; total_bytes_ = 0U; next_sequence_ = 1U;
            initialized_ = false;
            if (recovered == TileStoreResult::OK && recoverIndex() == TileStoreResult::OK) {
                initialized_ = true;
            }
            return result;
        }
        cleanupEvicted(victims, victim_count);
    }
    if (duplicate) storage_.remove(put_backup_);
    return TileStoreResult::OK;
}

void MapTileStore::abortPut() { failPut(); }

} // namespace TDeck
} // namespace Hardware
