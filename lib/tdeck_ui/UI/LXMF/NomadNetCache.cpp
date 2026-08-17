// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT
#include "NomadNetCache.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

namespace UI { namespace LXMF { namespace NomadNet {
namespace {
constexpr std::uint8_t MAGIC[8] = {'P', 'Y', 'N', 'C', 'A', 'C', 'H', '1'};
constexpr std::size_t MAX_PAGE = 64U * 1024U;
constexpr std::size_t MAX_META = 1024U;
constexpr char CACHE_DIRECTORY[] = "/pyxis-nomadnet/cache";
constexpr char CACHE_PREFIX[] = "/pyxis-nomadnet/cache/";

void put32(std::vector<std::uint8_t>& value, std::uint32_t number) {
    for (unsigned i = 0; i < 4; ++i) {
        value.push_back(static_cast<std::uint8_t>(number >> (8U * i)));
    }
}

void put64(std::vector<std::uint8_t>& value, std::uint64_t number) {
    for (unsigned i = 0; i < 8; ++i) {
        value.push_back(static_cast<std::uint8_t>(number >> (8U * i)));
    }
}

bool get32(const std::vector<std::uint8_t>& value, std::size_t& offset,
           std::uint32_t& number) {
    if (offset + 4 > value.size()) {
        return false;
    }
    number = 0;
    for (unsigned i = 0; i < 4; ++i) {
        number |= std::uint32_t(value[offset++]) << (8U * i);
    }
    return true;
}

bool get64(const std::vector<std::uint8_t>& value, std::size_t& offset,
           std::uint64_t& number) {
    if (offset + 8 > value.size()) {
        return false;
    }
    number = 0;
    for (unsigned i = 0; i < 8; ++i) {
        number |= std::uint64_t(value[offset++]) << (8U * i);
    }
    return true;
}

const char* classification(RequestDataClass value) {
    if (value == RequestDataClass::NIL) {
        return "nil";
    }
    if (value == RequestDataClass::FIELDS) {
        return "fields";
    }
    return "form";
}

bool isLowerHex(char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

std::string stemFor(const CacheKey& key) {
    const auto canonical = canonical_cache_key(key);
    const auto first = NomadNetCache::hash(
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size());
    const auto second = NomadNetCache::hash(
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size(),
        1099511628211ULL);
    char output[33] = {};
    std::snprintf(output, sizeof(output), "%016llx%016llx",
                  static_cast<unsigned long long>(first),
                  static_cast<unsigned long long>(second));
    return output;
}
} // namespace

std::string canonical_cache_key(const CacheKey& key) {
    std::string destination = key.destination;
    std::transform(destination.begin(), destination.end(), destination.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string path = key.path.empty() ? "/page/index.mu" : key.path;
    return destination + "\n" + path + "\n" + classification(key.request_data);
}

bool cache_eligible(const CacheEligibility& value) {
    return value.successful && value.valid && !value.partial && !value.malformed &&
           !value.truncated && !value.error &&
           value.request_data == RequestDataClass::NIL;
}

CacheDirective parse_cache_directive(const std::uint8_t* body, std::size_t size) {
    CacheDirective result;
    if (!body || size < 4 || std::memcmp(body, "#!c=", 4) != 0) {
        return result;
    }
    result.present = true;
    std::uint64_t value = 0;
    std::size_t offset = 4;
    if (offset == size || body[offset] < '0' || body[offset] > '9') {
        result.valid = false;
        result.ttl = 0;
        return result;
    }
    for (; offset < size && body[offset] != '\n' && body[offset] != '\r'; ++offset) {
        if (body[offset] < '0' || body[offset] > '9' ||
            value > (std::numeric_limits<std::uint64_t>::max() -
                     (body[offset] - '0')) / 10U) {
            result.valid = false;
            result.ttl = 0;
            return result;
        }
        value = value * 10U + (body[offset] - '0');
    }
    result.ttl = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(value, CacheConfig::MAX_TTL_SECONDS));
    return result;
}

std::uint32_t cache_directive_ttl(const std::uint8_t* body, std::size_t size) {
    return parse_cache_directive(body, size).ttl;
}

NomadNetCache::NomadNetCache(NomadNetStorage& storage, CacheConfig config)
    : storage_(storage), config_(config) {
    if (config_.chunk_bytes == 0) {
        config_.chunk_bytes = 1;
    }
    if (config_.max_scan_records == 0) {
        config_.max_scan_records = 1;
    }
    try {
        entries_.reserve(config_.max_scan_records);
        scan_records_.reserve(config_.max_scan_records);
        stage_records_.reserve(config_.max_scan_records);
    } catch (const std::bad_alloc&) {
        result_ = CacheResult::FULL;
        recovery_complete_ = true;
        return;
    }
    operation_ = Operation::RECOVERY_BEGIN;
    result_ = CacheResult::PENDING;
}

std::uint64_t NomadNetCache::hash(const std::uint8_t* data, std::size_t size,
                                  std::uint64_t value) {
    for (std::size_t i = 0; i < size; ++i) {
        value ^= data[i];
        value *= 1099511628211ULL;
    }
    return value;
}

bool NomadNetCache::sameKey(const CacheKey& first, const CacheKey& second) {
    return first.destination == second.destination && first.path == second.path &&
           first.request_data == second.request_data;
}

bool NomadNetCache::sequenceNewer(std::uint32_t first, std::uint32_t second) {
    const std::uint32_t distance = first - second;
    return distance != 0 && distance < 0x80000000U;
}

bool NomadNetCache::addWouldOverflow(std::size_t first, std::size_t second) {
    return second > std::numeric_limits<std::size_t>::max() - first;
}

bool NomadNetCache::saneTime(std::uint64_t now) const {
    return now >= config_.minimum_valid_epoch && now != 0;
}

std::string NomadNetCache::debugMetadataPath(const CacheKey& key,
                                              unsigned generation) const {
    return std::string(CACHE_PREFIX) + stemFor(key) + "." +
           std::to_string(generation & 1U) + ".meta";
}

std::string NomadNetCache::debugBodyPath(const CacheKey& key,
                                          unsigned generation) const {
    return std::string(CACHE_PREFIX) + stemFor(key) + "." +
           std::to_string(generation & 1U) + ".body";
}

std::string NomadNetCache::stageBody() const {
    return std::string(CACHE_PREFIX) + stemFor(key_) + ".stage.body";
}

std::string NomadNetCache::stageMeta() const {
    return std::string(CACHE_PREFIX) + stemFor(key_) + ".stage.meta";
}

bool NomadNetCache::encodeMetadata() {
    metadata_bytes_.assign(MAGIC, MAGIC + sizeof(MAGIC));
    put32(metadata_bytes_, 1);
    put32(metadata_bytes_, metadata_.sequence);
    put64(metadata_bytes_, metadata_.created);
    put64(metadata_bytes_, metadata_.expires);
    put32(metadata_bytes_, metadata_.body_size);
    put64(metadata_bytes_, metadata_.body_hash);
    put32(metadata_bytes_, static_cast<std::uint32_t>(metadata_.key.destination.size()));
    put32(metadata_bytes_, static_cast<std::uint32_t>(metadata_.key.path.size()));
    metadata_bytes_.push_back(static_cast<std::uint8_t>(metadata_.key.request_data));
    metadata_bytes_.insert(metadata_bytes_.end(), metadata_.key.destination.begin(),
                           metadata_.key.destination.end());
    metadata_bytes_.insert(metadata_bytes_.end(), metadata_.key.path.begin(),
                           metadata_.key.path.end());
    put64(metadata_bytes_, hash(metadata_bytes_.data(), metadata_bytes_.size()));
    return metadata_bytes_.size() <= MAX_META;
}

bool NomadNetCache::decodeMetadata(const std::vector<std::uint8_t>& value,
                                   Metadata& metadata) const {
    if (value.size() < 61 || value.size() > MAX_META ||
        std::memcmp(value.data(), MAGIC, sizeof(MAGIC)) != 0) {
        return false;
    }
    std::size_t offset = 8;
    std::uint32_t version = 0;
    std::uint32_t destination_length = 0;
    std::uint32_t path_length = 0;
    if (!get32(value, offset, version) || version != 1 ||
        !get32(value, offset, metadata.sequence) ||
        !get64(value, offset, metadata.created) ||
        !get64(value, offset, metadata.expires) ||
        !get32(value, offset, metadata.body_size) ||
        !get64(value, offset, metadata.body_hash) ||
        !get32(value, offset, destination_length) ||
        !get32(value, offset, path_length) || offset >= value.size()) {
        return false;
    }
    const auto request_class = value[offset++];
    if (request_class > 2 || destination_length != 32 || path_length == 0 ||
        path_length > 512 ||
        offset + destination_length + path_length + 8 != value.size() ||
        metadata.body_size == 0 || metadata.body_size > MAX_PAGE) {
        return false;
    }
    metadata.key.request_data = static_cast<RequestDataClass>(request_class);
    metadata.key.destination.assign(
        reinterpret_cast<const char*>(value.data() + offset), destination_length);
    offset += destination_length;
    metadata.key.path.assign(reinterpret_cast<const char*>(value.data() + offset),
                             path_length);
    offset += path_length;
    std::uint64_t stored_hash = 0;
    return get64(value, offset, stored_hash) &&
           stored_hash == hash(value.data(), value.size() - 8);
}

StorageResult NomadNetCache::readStep(const std::string& path,
                                      std::vector<std::uint8_t>& output,
                                      std::size_t limit, bool& complete) {
    complete = false;
    if (!read_open_) {
        std::uint32_t size = 0;
        const auto result = storage_.beginRead(path.c_str(), size);
        if (result != StorageResult::OK) {
            return result;
        }
        read_open_ = true;
        read_size_ = size;
        offset_ = 0;
        if (size > limit) {
            return StorageResult::TOO_LARGE;
        }
        try {
            output.assign(size, 0);
        } catch (const std::bad_alloc&) {
            return StorageResult::FULL;
        }
        return StorageResult::OK;
    }
    if (offset_ < read_size_) {
        std::size_t count = 0;
        const auto result = storage_.readChunk(
            output.data() + offset_,
            std::min(config_.chunk_bytes,
                     static_cast<std::size_t>(read_size_) - offset_),
            count);
        if (result != StorageResult::OK || count == 0 ||
            count > read_size_ - offset_) {
            return result == StorageResult::OK ? StorageResult::NO_PROGRESS : result;
        }
        offset_ += count;
        return StorageResult::OK;
    }
    const auto result = storage_.endRead();
    if (result == StorageResult::OK) {
        read_open_ = false;
        offset_ = 0;
        complete = true;
    }
    return result;
}

StorageResult NomadNetCache::readBodyStep(const std::string& path,
                                          std::size_t limit, bool& complete) {
    complete = false;
    if (!read_open_) {
        std::uint32_t size = 0;
        const auto result = storage_.beginRead(path.c_str(), size);
        if (result != StorageResult::OK) {
            return result;
        }
        read_open_ = true;
        read_size_ = size;
        offset_ = 0;
        if (size > limit) {
            return StorageResult::TOO_LARGE;
        }
        try {
            body_.assign(size, 0);
        } catch (const std::bad_alloc&) {
            return StorageResult::FULL;
        }
        return StorageResult::OK;
    }
    if (offset_ < read_size_) {
        std::size_t count = 0;
        const auto result = storage_.readChunk(
            body_.data() + offset_,
            std::min(config_.chunk_bytes,
                     static_cast<std::size_t>(read_size_) - offset_),
            count);
        if (result != StorageResult::OK || count == 0 ||
            count > read_size_ - offset_) {
            return result == StorageResult::OK ? StorageResult::NO_PROGRESS : result;
        }
        offset_ += count;
        return StorageResult::OK;
    }
    const auto result = storage_.endRead();
    if (result == StorageResult::OK) {
        read_open_ = false;
        offset_ = 0;
        complete = true;
    }
    return result;
}

void NomadNetCache::fail(CacheResult result) {
    cleanup_result_ = result;
    cleanup_stages_after_failure_ = commit_job_;
    if (read_open_) {
        operation_ = Operation::CLEANUP_END_READ;
    } else if (write_open_) {
        operation_ = Operation::CLEANUP_ABORT_WRITE;
    } else if (cleanup_stages_after_failure_) {
        operation_ = Operation::CLEANUP_STAGE_BODY;
    } else {
        finishCleanup();
    }
}

void NomadNetCache::finishCleanup() {
    read_open_ = false;
    write_open_ = false;
    commit_job_ = false;
    cleanup_stages_after_failure_ = false;
    offset_ = 0;
    io_.clear();
    metadata_bytes_.clear();
    ExternalVector<std::uint8_t>().swap(body_);
    operation_ = Operation::NONE;
    result_ = cleanup_result_;
}

CacheResult NomadNetCache::beginRecovery(std::uint64_t now, bool cleanup_stages) {
    if (busy()) {
        return CacheResult::PENDING;
    }
    if (cleanup_stages && !saneTime(now)) {
        return result_ = CacheResult::BYPASS;
    }
    now_ = now;
    recovery_cleanup_stages_ = cleanup_stages;
    recovery_complete_ = false;
    scan_seen_ = 0;
    scan_index_ = 0;
    cleanup_index_ = 0;
    scan_records_.clear();
    stage_records_.clear();
    conflicted_keys_.clear();
    namespace_authoritative_ = true;
    entries_.clear();
    operation_ = Operation::RECOVERY_BEGIN;
    result_ = CacheResult::PENDING;
    return result_;
}

bool NomadNetCache::parseOwnedMetadataPath(const char* path, ScanRecord& record) const {
    if (!path) {
        return false;
    }
    const std::size_t prefix = sizeof(CACHE_PREFIX) - 1;
    const std::size_t length = std::strlen(path);
    if (length != prefix + 39 || std::memcmp(path, CACHE_PREFIX, prefix) != 0) {
        return false;
    }
    for (std::size_t i = 0; i < 32; ++i) {
        if (!isLowerHex(path[prefix + i])) {
            return false;
        }
    }
    const char* suffix = path + prefix + 32;
    if (suffix[0] != '.' || (suffix[1] != '0' && suffix[1] != '1') ||
        std::memcmp(suffix + 2, ".meta", 6) != 0) {
        return false;
    }
    std::memcpy(record.stem, path + prefix, 32);
    record.stem[32] = 0;
    record.generation = static_cast<unsigned>(suffix[1] - '0');
    return true;
}

bool NomadNetCache::parseOwnedStagePath(const char* path, StageRecord& record) const {
    if (!path) {
        return false;
    }
    const std::size_t prefix = sizeof(CACHE_PREFIX) - 1;
    const std::size_t length = std::strlen(path);
    const bool body = length == prefix + 43 &&
                      std::memcmp(path + prefix + 32, ".stage.body", 11) == 0;
    const bool meta = length == prefix + 43 &&
                      std::memcmp(path + prefix + 32, ".stage.meta", 11) == 0;
    if ((!body && !meta) || length + 1 > sizeof(record.path) ||
        std::memcmp(path, CACHE_PREFIX, prefix) != 0) {
        return false;
    }
    for (std::size_t i = 0; i < 32; ++i) {
        if (!isLowerHex(path[prefix + i])) {
            return false;
        }
    }
    std::memcpy(record.path, path, length + 1);
    return true;
}

std::string NomadNetCache::scanMetadataPath() const {
    const auto& record = scan_records_[scan_index_];
    return std::string(CACHE_PREFIX) + record.stem + "." +
           std::to_string(record.generation) + ".meta";
}

std::string NomadNetCache::scanBodyPath() const {
    const auto& record = scan_records_[scan_index_];
    return std::string(CACHE_PREFIX) + record.stem + "." +
           std::to_string(record.generation) + ".body";
}

void NomadNetCache::finishRecovery() {
    io_.clear();
    if (!namespace_authoritative_) {
        recovery_complete_ = false;
        operation_ = Operation::NONE;
        result_ = CacheResult::BYPASS;
        return;
    }
    quota_recovery_ = true;
    quota_result_ = CacheResult::IDLE;
    key_ = CacheKey{};
    beginQuotaEviction();
}

void NomadNetCache::finishQuotaReconciliation() {
    operation_ = Operation::NONE;
    result_ = quota_result_;
    metadata_bytes_.clear();
    if (quota_recovery_) {
        recovery_complete_ = true;
        quota_recovery_ = false;
    }
}

bool NomadNetCache::removeStep(const std::string& path, Operation next) {
    const auto storage_result = storage_.remove(path.c_str());
    if (storage_result == StorageResult::OK || storage_result == StorageResult::MISS) {
        operation_ = next;
        return true;
    }
    if (storage_result_is_transient(storage_result)) return false;
    if (commit_job_) {
        fail(CacheResult::STORAGE_ERROR);
        return false;
    }
    operation_ = Operation::NONE;
    result_ = CacheResult::STORAGE_ERROR;
    if (eviction_pending_) namespace_authoritative_ = false;
    if (quota_recovery_) {
        recovery_complete_ = false;
        quota_recovery_ = false;
    }
    return false;
}

CacheResult NomadNetCache::beginLookup(const CacheKey& key, std::uint64_t now,
                                       bool bypass) {
    if (busy()) {
        if (!recovery_complete_) {
            return result_ = CacheResult::BYPASS;
        }
        return CacheResult::PENDING;
    }
    if (!recovery_complete_) {
        return result_ = CacheResult::BYPASS;
    }
    if (bypass || key.request_data != RequestDataClass::NIL) {
        return result_ = CacheResult::BYPASS;
    }
    key_ = key;
    std::transform(key_.destination.begin(), key_.destination.end(),
                   key_.destination.begin(), [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    if (key_.path.empty()) {
        key_.path = "/page/index.mu";
    }
    if (key_.destination.size() != 32) {
        return result_ = CacheResult::BYPASS;
    }
    now_ = now;
    ExternalVector<std::uint8_t>().swap(body_);
    io_.clear();
    has_fallback_ = false;
    metadata_generation_ = 0;
    metadata_valid_[0] = false;
    metadata_valid_[1] = false;
    metadata_records_[0].clear();
    metadata_records_[1].clear();
    read_open_ = false;
    offset_ = 0;
    result_ = CacheResult::PENDING;
    operation_ = Operation::LOOKUP_META;
    return result_;
}

void NomadNetCache::lookupMetadata() {
    bool complete = false;
    const auto read_result = readStep(
        debugMetadataPath(key_, metadata_generation_), io_, MAX_META, complete);
    if (read_result != StorageResult::OK) {
        if (storage_result_is_transient(read_result)) {
            fail(CacheResult::BYPASS);
            return;
        }
        if (read_open_) {
            cleanup_result_ = CacheResult::MISS;
            cleanup_stages_after_failure_ = false;
            operation_ = Operation::CLEANUP_END_READ;
            return;
        }
        io_.clear();
        complete = true;
    }
    if (!complete) {
        return;
    }
    if (read_result == StorageResult::OK) {
        Metadata checked;
        const bool valid = decodeMetadata(io_, checked) && sameKey(checked.key, key_) &&
                           stemFor(checked.key) == stemFor(key_);
        metadata_valid_[metadata_generation_] = valid;
        if (valid) {
            metadata_candidates_[metadata_generation_] = checked;
            metadata_records_[metadata_generation_] = io_;
        }
    }
    io_.clear();
    ++metadata_generation_;
    if (metadata_generation_ < 2) {
        return;
    }
    if (!metadata_valid_[0] && !metadata_valid_[1]) {
        cleanup_result_ = CacheResult::MISS;
        finishCleanup();
        return;
    }
    if (metadata_valid_[0] && metadata_valid_[1] &&
        metadata_candidates_[0].sequence == metadata_candidates_[1].sequence &&
        metadata_records_[0] != metadata_records_[1]) {
        cleanup_result_ = CacheResult::MISS;
        finishCleanup();
        return;
    }
    candidate_ = metadata_valid_[0] &&
                         (!metadata_valid_[1] ||
                          sequenceNewer(metadata_candidates_[0].sequence,
                                        metadata_candidates_[1].sequence))
                     ? 0U
                     : 1U;
    metadata_ = metadata_candidates_[candidate_];
    const unsigned other = candidate_ ^ 1U;
    has_fallback_ = metadata_valid_[other];
    if (has_fallback_) {
        fallback_metadata_ = metadata_candidates_[other];
        fallback_generation_ = other;
    }
    generation_ = candidate_;
    operation_ = Operation::LOOKUP_BODY;
}

void NomadNetCache::lookupBody() {
    bool complete = false;
    const auto read_result = readBodyStep(
        debugBodyPath(key_, candidate_), MAX_PAGE, complete);
    if (!complete && read_result == StorageResult::OK) {
        return;
    }
    if (read_result != StorageResult::OK && read_open_) {
        cleanup_result_ = storage_result_is_transient(read_result)
                              ? CacheResult::BYPASS
                              : CacheResult::MISS;
        cleanup_stages_after_failure_ = false;
        operation_ = Operation::CLEANUP_END_READ;
        return;
    }
    const bool good = read_result == StorageResult::OK &&
                      body_.size() == metadata_.body_size &&
                      hash(body_.data(), body_.size()) == metadata_.body_hash;
    if (!good && has_fallback_) {
        metadata_ = fallback_metadata_;
        candidate_ = fallback_generation_;
        has_fallback_ = false;
        ExternalVector<std::uint8_t>().swap(body_);
        offset_ = 0;
        return;
    }
    if (!good) {
        cleanup_result_ = CacheResult::MISS;
        finishCleanup();
        return;
    }
    if (!saneTime(now_) || metadata_.created == 0 || now_ < metadata_.created) {
        cleanup_result_ = CacheResult::MISS;
        finishCleanup();
        return;
    }
    if (now_ >= metadata_.expires) {
        cleanup_result_ = CacheResult::EXPIRED;
        finishCleanup();
        return;
    }
    operation_ = Operation::NONE;
    result_ = CacheResult::HIT;
}

std::size_t NomadNetCache::inactiveBytes(const CacheKey& key,
                                          unsigned generation) const {
    std::size_t total = 0;
    for (const auto& entry : entries_) {
        if (sameKey(entry.key, key) && entry.generation == generation) {
            if (addWouldOverflow(total, entry.body_bytes + entry.metadata_bytes)) {
                return std::numeric_limits<std::size_t>::max();
            }
            total += entry.body_bytes + entry.metadata_bytes;
        }
    }
    return total;
}

void NomadNetCache::eraseGeneration(const CacheKey& key, unsigned generation) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const Entry& entry) {
                                      return sameKey(entry.key, key) &&
                                             entry.generation == generation;
                                  }),
                   entries_.end());
}

CacheResult NomadNetCache::beginCommit(const CacheKey& key,
                                       const std::vector<std::uint8_t>& body,
                                       std::uint64_t now, std::uint32_t ttl) {
    ExternalVector<std::uint8_t> external;
    try {
        external.assign(body.begin(), body.end());
    } catch (const std::bad_alloc&) {
        return result_ = CacheResult::FULL;
    }
    return beginCommit(key, std::move(external), now, ttl);
}

CacheResult NomadNetCache::beginCommit(const CacheKey& key,
                                       ExternalVector<std::uint8_t>&& body,
                                       std::uint64_t now, std::uint32_t ttl) {
    if (busy()) {
        return recovery_complete_ ? CacheResult::PENDING : CacheResult::BYPASS;
    }
    if (!recovery_complete_) {
        return result_ = CacheResult::BYPASS;
    }
    if (!namespace_authoritative_ || key.request_data != RequestDataClass::NIL ||
        ttl == 0 || !saneTime(now) || body.empty() || body.size() > MAX_PAGE) {
        return result_ = CacheResult::BYPASS;
    }
    key_ = key;
    std::transform(key_.destination.begin(), key_.destination.end(),
                   key_.destination.begin(), [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    if (key_.path.empty()) {
        key_.path = "/page/index.mu";
    }
    if (key_.destination.size() != 32) {
        return result_ = CacheResult::BYPASS;
    }

    const Entry* newest = nullptr;
    for (const auto& entry : entries_) {
        if (sameKey(entry.key, key_) &&
            (!newest || sequenceNewer(entry.sequence, newest->sequence))) {
            newest = &entry;
        }
    }
    candidate_ = newest ? (newest->generation ^ 1U) : 0U;
    generation_ = candidate_;
    std::uint32_t next_sequence = sequence_ + 1U;
    if (next_sequence == 0) {
        next_sequence = 1;
    }

    now_ = now;
    body_.swap(body);
    metadata_.key = key_;
    metadata_.created = now;
    metadata_.expires = now + ttl;
    if (metadata_.expires < now) {
        ExternalVector<std::uint8_t>().swap(body_);
        return result_ = CacheResult::INVALID;
    }
    metadata_.body_size = static_cast<std::uint32_t>(body_.size());
    metadata_.body_hash = hash(body_.data(), body_.size());
    metadata_.sequence = next_sequence;
    try {
        if (!encodeMetadata()) {
            ExternalVector<std::uint8_t>().swap(body_);
            return result_ = CacheResult::INVALID;
        }
    } catch (const std::bad_alloc&) {
        ExternalVector<std::uint8_t>().swap(body_);
        return result_ = CacheResult::FULL;
    }

    const std::size_t candidate_bytes = body_.size() + metadata_bytes_.size();
    const std::size_t reclaimable = inactiveBytes(key_, candidate_);
    const std::size_t physical = totalBytes();
    if (candidate_bytes > config_.max_stage_reserve || reclaimable > physical ||
        addWouldOverflow(physical - reclaimable, candidate_bytes) ||
        physical - reclaimable + candidate_bytes > config_.max_bytes) {
        ExternalVector<std::uint8_t>().swap(body_);
        metadata_bytes_.clear();
        return result_ = CacheResult::FULL;
    }

    sequence_ = next_sequence;
    commit_job_ = true;
    offset_ = 0;
    operation_ = Operation::PREPARE_REMOVE_INACTIVE_META;
    result_ = CacheResult::PENDING;
    return result_;
}

void NomadNetCache::finishCommit() {
    eraseGeneration(key_, generation_);
    Entry entry;
    entry.key = key_;
    entry.created = metadata_.created;
    entry.expires = metadata_.expires;
    entry.body_bytes = metadata_.body_size;
    entry.metadata_bytes = metadata_bytes_.size();
    entry.sequence = metadata_.sequence;
    entry.generation = generation_;
    entry.metadata_record = metadata_bytes_;
    entries_.push_back(entry);
    commit_job_ = false;
    ExternalVector<std::uint8_t>().swap(body_);
    io_.clear();
    quota_result_ = CacheResult::STORED;
    quota_recovery_ = false;
    beginQuotaEviction();
}

std::size_t NomadNetCache::entryCount() const {
    std::size_t count = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        bool seen = false;
        for (std::size_t j = 0; j < i; ++j) {
            if (sameKey(entries_[i].key, entries_[j].key)) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            ++count;
        }
    }
    return count;
}

std::size_t NomadNetCache::totalBytes() const {
    std::size_t total = 0;
    for (const auto& entry : entries_) {
        const std::size_t record = entry.body_bytes + entry.metadata_bytes;
        if (addWouldOverflow(total, record)) {
            return std::numeric_limits<std::size_t>::max();
        }
        total += record;
    }
    return total;
}

void NomadNetCache::beginQuotaEviction() {
    const bool entries_over = entryCount() > config_.max_entries;
    const bool bytes_over = totalBytes() > config_.max_bytes;
    if (!entries_over && !bytes_over) {
        finishQuotaReconciliation();
        return;
    }

    std::vector<const Entry*> logical;
    logical.reserve(entries_.size());
    for (const auto& entry : entries_) {
        auto found = std::find_if(logical.begin(), logical.end(), [&](const Entry* value) {
            return sameKey(value->key, entry.key);
        });
        if (found == logical.end()) logical.push_back(&entry);
        else if (sequenceNewer(entry.sequence, (*found)->sequence)) *found = &entry;
    }

    const Entry* victim = nullptr;
    if (entries_over) {
        for (const Entry* entry : logical) {
            if (!quota_recovery_ && sameKey(entry->key, key_) && logical.size() > 1) continue;
            if (!victim) victim = entry;
            else {
                const bool entry_expired = saneTime(now_) && entry->expires <= now_;
                const bool victim_expired = saneTime(now_) && victim->expires <= now_;
                if ((entry_expired && !victim_expired) ||
                    (entry_expired == victim_expired &&
                     (entry->created < victim->created ||
                      (entry->created == victim->created &&
                       canonical_cache_key(entry->key) < canonical_cache_key(victim->key)))))
                    victim = entry;
            }
        }
        eviction_generation_ = -1;
    } else {
        // Physical-byte reconciliation may need to shed only an older fallback
        // generation for the sole logical key. Prefer expired/oldest records,
        // and never select the newest sequence while an older slot exists.
        for (const auto& entry : entries_) {
            bool has_newer_same_key = false;
            for (const auto& other : entries_) {
                if (sameKey(entry.key, other.key) &&
                    sequenceNewer(other.sequence, entry.sequence)) {
                    has_newer_same_key = true;
                    break;
                }
            }
            if (!has_newer_same_key && entries_.size() > logical.size()) continue;
            if (!victim || entry.created < victim->created ||
                (entry.created == victim->created &&
                 canonical_cache_key(entry.key) < canonical_cache_key(victim->key)))
                victim = &entry;
        }
        if (!victim && !entries_.empty()) victim = &entries_.front();
        eviction_generation_ = victim ? static_cast<int>(victim->generation) : -1;
    }

    if (!victim) {
        operation_ = Operation::NONE;
        result_ = CacheResult::STORAGE_ERROR;
        if (quota_recovery_) {
            recovery_complete_ = false;
            quota_recovery_ = false;
        }
        return;
    }
    eviction_key_ = victim->key;
    eviction_pending_ = true;
    operation_ = eviction_generation_ == 1 ? Operation::EVICT_META_1
                                          : Operation::EVICT_META_0;
}

void NomadNetCache::finishEviction() {
    if (eviction_generation_ >= 0) {
        eraseGeneration(eviction_key_, static_cast<unsigned>(eviction_generation_));
    } else {
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&](const Entry& entry) {
                                          return sameKey(entry.key, eviction_key_);
                                      }),
                       entries_.end());
    }
    eviction_pending_ = false;
    eviction_generation_ = -1;
    beginQuotaEviction();
}

bool NomadNetCache::takeBody(ExternalVector<std::uint8_t>& output) {
    if (result_ != CacheResult::HIT) {
        return false;
    }
    output.swap(body_);
    return true;
}

CacheResult NomadNetCache::invalidate(const CacheKey& key) {
    if (busy()) {
        return CacheResult::PENDING;
    }
    key_ = key;
    std::transform(key_.destination.begin(), key_.destination.end(),
                   key_.destination.begin(), [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    if (key_.path.empty()) {
        key_.path = "/page/index.mu";
    }
    result_ = CacheResult::PENDING;
    operation_ = Operation::INVALIDATE_META_0;
    return result_;
}

void NomadNetCache::service() {
    switch (operation_) {
        case Operation::NONE:
            return;
        case Operation::RECOVERY_BEGIN: {
            const auto storage_result = storage_.beginList(CACHE_DIRECTORY);
            if (storage_result != StorageResult::OK) {
                result_ = CacheResult::BYPASS;
                if (!storage_result_is_transient(storage_result)) {
                    namespace_authoritative_ = false;
                    operation_ = Operation::NONE;
                }
                return;
            }
            operation_ = Operation::RECOVERY_NEXT;
            return;
        }
        case Operation::RECOVERY_NEXT: {
            bool done = false;
            std::memset(list_path_, 0, sizeof(list_path_));
            const auto storage_result = storage_.nextList(
                list_path_, sizeof(list_path_), done);
            if (storage_result != StorageResult::OK &&
                storage_result != StorageResult::TOO_LARGE) {
                namespace_authoritative_ = false;
                operation_ = Operation::RECOVERY_END;
                return;
            }
            if (done || scan_seen_ >= config_.max_scan_records) {
                if (!done) namespace_authoritative_ = false;
                operation_ = Operation::RECOVERY_END;
                return;
            }
            ++scan_seen_;
            if (storage_result == StorageResult::OK) {
                ScanRecord record;
                if (parseOwnedMetadataPath(list_path_, record)) {
                    scan_records_.push_back(record);
                } else if (recovery_cleanup_stages_) {
                    StageRecord stage;
                    if (parseOwnedStagePath(list_path_, stage)) {
                        stage_records_.push_back(stage);
                    }
                }
            }
            return;
        }
        case Operation::RECOVERY_END: {
            const auto storage_result = storage_.endList();
            if (storage_result != StorageResult::OK) {
                result_ = CacheResult::BYPASS;
                if (!storage_result_is_transient(storage_result)) {
                    operation_ = Operation::NONE;
                }
                return;
            }
            scan_index_ = 0;
            io_.clear();
            if (!namespace_authoritative_) {
                finishRecovery();
                return;
            }
            operation_ = scan_records_.empty()
                             ? (stage_records_.empty()
                                    ? Operation::NONE
                                    : Operation::RECOVERY_CLEAN_STAGE)
                             : Operation::RECOVERY_META;
            if (operation_ == Operation::NONE) {
                finishRecovery();
            }
            return;
        }
        case Operation::RECOVERY_META: {
            bool complete = false;
            const auto storage_result = readStep(
                scanMetadataPath(), io_, MAX_META, complete);
            if (storage_result != StorageResult::OK) {
                if (storage_result_is_transient(storage_result)) {
                    if (read_open_) {
                        recovery_retry_record_ = true;
                        operation_ = Operation::RECOVERY_CLOSE_READ;
                    }
                    return;
                }
                if (read_open_) {
                    // Enumeration proved that this owned slot exists, but its
                    // metadata could not be read. Its key/generation is now
                    // unknowable, so later commits must not infer ownership
                    // from only the records that remained readable.
                    namespace_authoritative_ = false;
                    recovery_retry_record_ = false;
                    operation_ = Operation::RECOVERY_CLOSE_READ;
                    return;
                }
                namespace_authoritative_ = false;
                io_.clear();
                ++scan_index_;
                operation_ = scan_index_ < scan_records_.size()
                                 ? Operation::RECOVERY_META
                                 : (stage_records_.empty()
                                        ? Operation::NONE
                                        : Operation::RECOVERY_CLEAN_STAGE);
                if (operation_ == Operation::NONE) {
                    finishRecovery();
                }
                return;
            }
            if (!complete) {
                return;
            }
            Metadata checked;
            const auto& scan = scan_records_[scan_index_];
            const bool decoded = decodeMetadata(io_, checked);
            const std::string checked_stem = decoded ? stemFor(checked.key) : std::string();
            if (!decoded || checked_stem != scan.stem) {
                io_.clear();
                ++scan_index_;
                if (scan_index_ >= scan_records_.size()) {
                    if (stage_records_.empty()) {
                        finishRecovery();
                    } else {
                        operation_ = Operation::RECOVERY_CLEAN_STAGE;
                    }
                }
                return;
            }
            metadata_ = checked;
            pending_metadata_bytes_ = io_.size();
            pending_metadata_record_ = io_;
            io_.clear();
            operation_ = Operation::RECOVERY_STAT_BODY;
            return;
        }
        case Operation::RECOVERY_CLOSE_READ: {
            const auto storage_result = storage_.endRead();
            if (storage_result != StorageResult::OK) {
                return;
            }
            read_open_ = false;
            io_.clear();
            offset_ = 0;
            if (!recovery_retry_record_) {
                ++scan_index_;
            }
            recovery_retry_record_ = false;
            if (scan_index_ < scan_records_.size()) {
                operation_ = Operation::RECOVERY_META;
            } else if (!stage_records_.empty()) {
                cleanup_index_ = 0;
                operation_ = Operation::RECOVERY_CLEAN_STAGE;
            } else {
                finishRecovery();
            }
            return;
        }
        case Operation::RECOVERY_STAT_BODY: {
            std::uint32_t body_size = 0;
            const auto storage_result = storage_.stat(scanBodyPath().c_str(), body_size);
            if (storage_result_is_transient(storage_result)) {
                return;
            }
            if (storage_result != StorageResult::OK) {
                // Metadata enumeration established ownership of this body slot,
                // but its state is unknown. Do not let a partial recovery make
                // overwrite decisions for the namespace.
                namespace_authoritative_ = false;
            }
            const auto& scan = scan_records_[scan_index_];
            if (storage_result == StorageResult::OK &&
                body_size == metadata_.body_size &&
                entries_.size() < config_.max_scan_records) {
                const bool already_conflicted = std::any_of(
                    conflicted_keys_.begin(), conflicted_keys_.end(),
                    [&](const CacheKey& value) { return sameKey(value, metadata_.key); });
                auto equal = std::find_if(entries_.begin(), entries_.end(),
                    [&](const Entry& value) {
                        return sameKey(value.key, metadata_.key) &&
                               value.sequence == metadata_.sequence;
                    });
                if (!already_conflicted && equal != entries_.end() &&
                    equal->metadata_record != pending_metadata_record_) {
                    conflicted_keys_.push_back(metadata_.key);
                    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                        [&](const Entry& value) { return sameKey(value.key, metadata_.key); }),
                        entries_.end());
                } else if (!already_conflicted) {
                    Entry entry;
                    entry.key = metadata_.key;
                    entry.created = metadata_.created;
                    entry.expires = metadata_.expires;
                    entry.body_bytes = body_size;
                    entry.metadata_bytes = pending_metadata_bytes_;
                    entry.sequence = metadata_.sequence;
                    entry.generation = scan.generation;
                    entry.metadata_record = pending_metadata_record_;
                    entries_.push_back(entry);
                    if (sequence_ == 0 || sequenceNewer(entry.sequence, sequence_)) {
                        sequence_ = entry.sequence;
                    }
                }
            }
            ++scan_index_;
            if (scan_index_ < scan_records_.size()) {
                operation_ = Operation::RECOVERY_META;
            } else if (!stage_records_.empty()) {
                cleanup_index_ = 0;
                operation_ = Operation::RECOVERY_CLEAN_STAGE;
            } else {
                finishRecovery();
            }
            return;
        }
        case Operation::RECOVERY_CLEAN_STAGE: {
            const auto storage_result = storage_.remove(stage_records_[cleanup_index_].path);
            if (storage_result_is_transient(storage_result)) {
                return;
            }
            ++cleanup_index_;
            if (cleanup_index_ >= stage_records_.size()) {
                finishRecovery();
            }
            return;
        }
        case Operation::LOOKUP_META:
            lookupMetadata();
            return;
        case Operation::LOOKUP_BODY:
            lookupBody();
            return;
        case Operation::PREPARE_REMOVE_INACTIVE_META:
            removeStep(debugMetadataPath(key_, candidate_),
                       Operation::PREPARE_REMOVE_INACTIVE_BODY);
            return;
        case Operation::PREPARE_REMOVE_INACTIVE_BODY:
            if (removeStep(debugBodyPath(key_, candidate_),
                           Operation::PREPARE_REMOVE_STAGE_BODY))
                eraseGeneration(key_, candidate_);
            return;
        case Operation::PREPARE_REMOVE_STAGE_BODY:
            removeStep(stageBody(), Operation::PREPARE_REMOVE_STAGE_META);
            return;
        case Operation::PREPARE_REMOVE_STAGE_META:
            removeStep(stageMeta(), Operation::PREPARE_BEGIN_BODY);
            return;
        case Operation::PREPARE_BEGIN_BODY: {
            const auto storage_result = storage_.beginWrite(stageBody().c_str());
            if (storage_result != StorageResult::OK) {
                fail(storage_result == StorageResult::FULL ? CacheResult::FULL
                                                           : CacheResult::STORAGE_ERROR);
                return;
            }
            write_open_ = true;
            offset_ = 0;
            operation_ = Operation::COMMIT_BODY;
            return;
        }
        case Operation::COMMIT_BODY: {
            const std::size_t amount =
                std::min(config_.chunk_bytes, body_.size() - offset_);
            std::size_t written = 0;
            const auto storage_result = storage_.writeChunk(
                body_.data() + offset_, amount, written);
            if (storage_result != StorageResult::OK || written != amount) {
                fail(storage_result == StorageResult::FULL ? CacheResult::FULL
                                                           : CacheResult::STORAGE_ERROR);
                return;
            }
            offset_ += written;
            if (offset_ == body_.size()) {
                operation_ = Operation::COMMIT_BODY_SYNC;
            }
            return;
        }
        case Operation::COMMIT_BODY_SYNC: {
            const auto storage_result = storage_.commitWrite();
            write_open_ = false;
            if (storage_result != StorageResult::OK) {
                fail(storage_result == StorageResult::FULL ? CacheResult::FULL
                                                           : CacheResult::STORAGE_ERROR);
                return;
            }
            operation_ = Operation::COMMIT_META_BEGIN;
            return;
        }
        case Operation::COMMIT_META_BEGIN: {
            const auto storage_result = storage_.beginWrite(stageMeta().c_str());
            if (storage_result != StorageResult::OK) {
                fail(storage_result == StorageResult::FULL ? CacheResult::FULL
                                                           : CacheResult::STORAGE_ERROR);
                return;
            }
            write_open_ = true;
            offset_ = 0;
            operation_ = Operation::COMMIT_META;
            return;
        }
        case Operation::COMMIT_META: {
            const std::size_t amount =
                std::min(config_.chunk_bytes, metadata_bytes_.size() - offset_);
            std::size_t written = 0;
            const auto storage_result = storage_.writeChunk(
                metadata_bytes_.data() + offset_, amount, written);
            if (storage_result != StorageResult::OK || written != amount) {
                fail(storage_result == StorageResult::FULL ? CacheResult::FULL
                                                           : CacheResult::STORAGE_ERROR);
                return;
            }
            offset_ += written;
            if (offset_ == metadata_bytes_.size()) {
                operation_ = Operation::COMMIT_META_SYNC;
            }
            return;
        }
        case Operation::COMMIT_META_SYNC: {
            const auto storage_result = storage_.commitWrite();
            write_open_ = false;
            if (storage_result != StorageResult::OK) {
                fail(storage_result == StorageResult::FULL ? CacheResult::FULL
                                                           : CacheResult::STORAGE_ERROR);
                return;
            }
            io_.clear();
            offset_ = 0;
            operation_ = Operation::VERIFY_BODY;
            return;
        }
        case Operation::VERIFY_BODY: {
            if (!read_open_) {
                std::uint32_t size = 0;
                const auto storage_result = storage_.beginRead(stageBody().c_str(), size);
                if (storage_result != StorageResult::OK) {
                    fail(storage_result == StorageResult::FULL ? CacheResult::FULL
                                                               : CacheResult::STORAGE_ERROR);
                    return;
                }
                read_open_ = true;
                read_size_ = size;
                offset_ = 0;
                verify_hash_ = 1469598103934665603ULL;
                verify_match_ = size == metadata_.body_size && size == body_.size();
                if (size > MAX_PAGE) verify_match_ = false;
                return;
            }
            if (offset_ < read_size_) {
                std::size_t count = 0;
                const std::size_t amount = std::min<std::size_t>(
                    std::min<std::size_t>(config_.chunk_bytes, verify_scratch_.size()),
                    static_cast<std::size_t>(read_size_) - offset_);
                const auto storage_result = storage_.readChunk(
                    verify_scratch_.data(), amount, count);
                if (storage_result != StorageResult::OK || count == 0 || count > amount) {
                    fail(storage_result == StorageResult::FULL ? CacheResult::FULL
                                                               : CacheResult::STORAGE_ERROR);
                    return;
                }
                verify_hash_ = hash(verify_scratch_.data(), count, verify_hash_);
                if (verify_match_) {
                    for (std::size_t i = 0; i < count; ++i) {
                        if (verify_scratch_[i] != body_[offset_ + i]) {
                            verify_match_ = false;
                            break;
                        }
                    }
                }
                offset_ += count;
                return;
            }
            const auto storage_result = storage_.endRead();
            if (storage_result != StorageResult::OK) return;
            read_open_ = false;
            offset_ = 0;
            if (!verify_match_ || verify_hash_ != metadata_.body_hash) {
                fail(CacheResult::INVALID);
                return;
            }
            operation_ = Operation::VERIFY_META;
            return;
        }
        case Operation::VERIFY_META: {
            bool complete = false;
            const auto storage_result = readStep(stageMeta(), io_, MAX_META, complete);
            if (storage_result != StorageResult::OK) {
                fail(storage_result == StorageResult::FULL ? CacheResult::FULL
                                                           : CacheResult::STORAGE_ERROR);
                return;
            }
            if (!complete) {
                return;
            }
            Metadata checked;
            if (!decodeMetadata(io_, checked) || !sameKey(checked.key, key_) ||
                checked.sequence != metadata_.sequence ||
                checked.created != metadata_.created ||
                checked.expires != metadata_.expires ||
                checked.body_size != metadata_.body_size ||
                checked.body_hash != metadata_.body_hash ||
                stemFor(checked.key) != stemFor(key_)) {
                fail(CacheResult::INVALID);
                return;
            }
            io_.clear();
            operation_ = Operation::PROMOTE_BODY;
            return;
        }
        case Operation::PROMOTE_BODY:
            if (storage_.rename(stageBody().c_str(),
                                debugBodyPath(key_, generation_).c_str()) !=
                StorageResult::OK) {
                fail(CacheResult::STORAGE_ERROR);
                return;
            }
            operation_ = Operation::PROMOTE_META;
            return;
        case Operation::PROMOTE_META:
            if (storage_.rename(stageMeta().c_str(),
                                debugMetadataPath(key_, generation_).c_str()) !=
                StorageResult::OK) {
                fail(CacheResult::STORAGE_ERROR);
                return;
            }
            finishCommit();
            return;
        case Operation::EVICT_META_0:
            removeStep(debugMetadataPath(eviction_key_, 0), Operation::EVICT_BODY_0);
            return;
        case Operation::EVICT_BODY_0:
            if (eviction_generation_ == 0) {
                const auto before = operation_;
                if (removeStep(debugBodyPath(eviction_key_, 0), Operation::EVICT_META_1) &&
                    before != Operation::NONE)
                    finishEviction();
            } else {
                removeStep(debugBodyPath(eviction_key_, 0), Operation::EVICT_META_1);
            }
            return;
        case Operation::EVICT_META_1:
            removeStep(debugMetadataPath(eviction_key_, 1), Operation::EVICT_BODY_1);
            return;
        case Operation::EVICT_BODY_1:
            if (removeStep(debugBodyPath(eviction_key_, 1), Operation::NONE))
                finishEviction();
            return;
        case Operation::INVALIDATE_META_0:
            removeStep(debugMetadataPath(key_, 0), Operation::INVALIDATE_BODY_0);
            return;
        case Operation::INVALIDATE_BODY_0:
            removeStep(debugBodyPath(key_, 0), Operation::INVALIDATE_META_1);
            return;
        case Operation::INVALIDATE_META_1:
            removeStep(debugMetadataPath(key_, 1), Operation::INVALIDATE_BODY_1);
            return;
        case Operation::INVALIDATE_BODY_1:
            if (!removeStep(debugBodyPath(key_, 1), Operation::NONE)) return;
            entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                          [&](const Entry& entry) {
                                              return sameKey(entry.key, key_);
                                          }),
                           entries_.end());
            result_ = CacheResult::MISS;
            return;
        case Operation::CLEANUP_END_READ: {
            const auto storage_result = storage_.endRead();
            if (storage_result != StorageResult::OK) return;
            read_open_ = false;
            operation_ = write_open_ ? Operation::CLEANUP_ABORT_WRITE
                                     : (cleanup_stages_after_failure_
                                            ? Operation::CLEANUP_STAGE_BODY
                                            : Operation::NONE);
            if (operation_ == Operation::NONE) finishCleanup();
            return;
        }
        case Operation::CLEANUP_ABORT_WRITE: {
            const auto storage_result = storage_.abortWrite();
            if (storage_result != StorageResult::OK) return;
            write_open_ = false;
            operation_ = cleanup_stages_after_failure_
                             ? Operation::CLEANUP_STAGE_BODY
                             : Operation::NONE;
            if (operation_ == Operation::NONE) finishCleanup();
            return;
        }
        case Operation::CLEANUP_STAGE_BODY: {
            const auto storage_result = storage_.remove(stageBody().c_str());
            if (storage_result_is_transient(storage_result)) return;
            if (storage_result != StorageResult::OK && storage_result != StorageResult::MISS)
                cleanup_result_ = CacheResult::STORAGE_ERROR;
            operation_ = Operation::CLEANUP_STAGE_META;
            return;
        }
        case Operation::CLEANUP_STAGE_META: {
            const auto storage_result = storage_.remove(stageMeta().c_str());
            if (storage_result_is_transient(storage_result)) return;
            if (storage_result != StorageResult::OK && storage_result != StorageResult::MISS)
                cleanup_result_ = CacheResult::STORAGE_ERROR;
            finishCleanup();
            return;
        }
    }
}

void NomadNetCache::cancel() {
    if (!busy() || eviction_pending_ || quota_recovery_) {
        return;
    }
    if (!recovery_complete_) {
        return;
    }
    cleanup_result_ = CacheResult::CANCELLED;
    cleanup_stages_after_failure_ = commit_job_;
    if (read_open_) {
        operation_ = Operation::CLEANUP_END_READ;
    } else if (write_open_) {
        operation_ = Operation::CLEANUP_ABORT_WRITE;
    } else if (cleanup_stages_after_failure_) {
        operation_ = Operation::CLEANUP_STAGE_BODY;
    } else {
        finishCleanup();
    }
}

}}} // namespace UI::LXMF::NomadNet
