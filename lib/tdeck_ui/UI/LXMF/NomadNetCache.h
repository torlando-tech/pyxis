// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT
#ifndef UI_LXMF_NOMADNET_CACHE_H
#define UI_LXMF_NOMADNET_CACHE_H

#include "NomadNetMemory.h"
#include "NomadNetStorage.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace UI { namespace LXMF { namespace NomadNet {

enum class RequestDataClass : std::uint8_t { NIL = 0, FIELDS = 1, FORM = 2 };

struct CacheKey {
    CacheKey() = default;
    CacheKey(const std::string& destination_value, const std::string& path_value,
             RequestDataClass request_data_value)
        : destination(destination_value), path(path_value), request_data(request_data_value) {}

    std::string destination;
    std::string path;
    RequestDataClass request_data = RequestDataClass::NIL;
};

std::string canonical_cache_key(const CacheKey& key);

struct CacheEligibility {
    bool successful = false;
    bool valid = false;
    bool partial = false;
    bool malformed = false;
    bool truncated = false;
    bool error = false;
    RequestDataClass request_data = RequestDataClass::NIL;
};

bool cache_eligible(const CacheEligibility& value);

struct CacheConfig {
    static constexpr std::uint32_t DEFAULT_TTL_SECONDS = 12U * 60U * 60U;
    static constexpr std::uint32_t MAX_TTL_SECONDS = 7U * 24U * 60U * 60U;
    static constexpr std::size_t MAX_LOGICAL_ENTRIES = 32;
    static constexpr std::size_t MAX_ON_DISK_BYTES = 2U * 1024U * 1024U;
    static constexpr std::size_t MAX_SCAN_RECORDS = 96;
    static constexpr std::size_t MAX_PAGE_STAGE_RESERVE = 66U * 1024U;
    static constexpr std::size_t MAX_PATH_BYTES = 128;

    std::size_t max_entries = MAX_LOGICAL_ENTRIES;
    std::size_t max_bytes = MAX_ON_DISK_BYTES;
    std::size_t max_scan_records = MAX_SCAN_RECORDS;
    std::size_t max_stage_reserve = MAX_PAGE_STAGE_RESERVE;
    std::size_t chunk_bytes = 1024;
    std::uint64_t minimum_valid_epoch = 1;
};

struct CacheDirective {
    bool present = false;
    bool valid = true;
    std::uint32_t ttl = CacheConfig::DEFAULT_TTL_SECONDS;
};

CacheDirective parse_cache_directive(const std::uint8_t* body, std::size_t size);
std::uint32_t cache_directive_ttl(const std::uint8_t* body, std::size_t size);

enum class CacheResult : std::uint8_t {
    IDLE,
    PENDING,
    HIT,
    MISS,
    EXPIRED,
    STORED,
    BYPASS,
    CANCELLED,
    INVALID,
    STORAGE_ERROR,
    FULL
};

class NomadNetCache {
public:
    explicit NomadNetCache(NomadNetStorage& storage, CacheConfig config = {});

    CacheResult beginLookup(const CacheKey& key, std::uint64_t now, bool bypass = false);
    CacheResult beginCommit(const CacheKey& key, const std::vector<std::uint8_t>& body,
                            std::uint64_t now, std::uint32_t ttl);
    CacheResult beginCommit(const CacheKey& key, ExternalVector<std::uint8_t>&& body,
                            std::uint64_t now, std::uint32_t ttl);
    CacheResult beginRecovery(std::uint64_t now, bool cleanup_stages = false);
    CacheResult invalidate(const CacheKey& key);

    void service();
    void cancel();
    bool busy() const { return operation_ != Operation::NONE; }
    bool recoveryComplete() const { return recovery_complete_; }
    CacheResult lastResult() const { return result_; }
    bool takeBody(ExternalVector<std::uint8_t>& output);

    std::size_t entryCount() const;
    std::size_t totalBytes() const;
    std::string debugMetadataPath(const CacheKey& key, unsigned generation) const;
    std::string debugBodyPath(const CacheKey& key, unsigned generation) const;
    unsigned debugGeneration() const { return generation_; }
    static std::uint64_t hash(const std::uint8_t*, std::size_t,
                              std::uint64_t seed = 1469598103934665603ULL);

private:
    enum class Operation {
        NONE,
        RECOVERY_BEGIN,
        RECOVERY_NEXT,
        RECOVERY_END,
        RECOVERY_META,
        RECOVERY_CLOSE_READ,
        RECOVERY_STAT_BODY,
        RECOVERY_CLEAN_STAGE,
        LOOKUP_META,
        LOOKUP_BODY,
        PREPARE_REMOVE_INACTIVE_META,
        PREPARE_REMOVE_INACTIVE_BODY,
        PREPARE_REMOVE_STAGE_BODY,
        PREPARE_REMOVE_STAGE_META,
        PREPARE_BEGIN_BODY,
        COMMIT_BODY,
        COMMIT_BODY_SYNC,
        COMMIT_META_BEGIN,
        COMMIT_META,
        COMMIT_META_SYNC,
        VERIFY_BODY,
        VERIFY_META,
        PROMOTE_BODY,
        PROMOTE_META,
        EVICT_META_0,
        EVICT_BODY_0,
        EVICT_META_1,
        EVICT_BODY_1,
        INVALIDATE_META_0,
        INVALIDATE_BODY_0,
        INVALIDATE_META_1,
        INVALIDATE_BODY_1,
        CLEANUP_END_READ,
        CLEANUP_ABORT_WRITE,
        CLEANUP_STAGE_BODY,
        CLEANUP_STAGE_META
    };

    struct Metadata {
        std::uint64_t created = 0;
        std::uint64_t expires = 0;
        std::uint64_t body_hash = 0;
        std::uint32_t body_size = 0;
        std::uint32_t sequence = 0;
        CacheKey key;
    };

    struct Entry {
        CacheKey key;
        std::uint64_t created = 0;
        std::uint64_t expires = 0;
        std::size_t body_bytes = 0;
        std::size_t metadata_bytes = 0;
        std::uint32_t sequence = 0;
        unsigned generation = 0;
        std::vector<std::uint8_t> metadata_record;
    };

    struct ScanRecord {
        char stem[33] = {};
        unsigned generation = 0;
    };

    struct StageRecord {
        char path[CacheConfig::MAX_PATH_BYTES] = {};
    };

    NomadNetStorage& storage_;
    CacheConfig config_;
    Operation operation_ = Operation::NONE;
    CacheResult result_ = CacheResult::IDLE;
    CacheResult cleanup_result_ = CacheResult::IDLE;
    CacheKey key_;
    std::uint64_t now_ = 0;
    unsigned generation_ = 0;
    unsigned candidate_ = 0;
    std::uint32_t sequence_ = 0;
    ExternalVector<std::uint8_t> body_;
    std::vector<std::uint8_t> io_;
    std::vector<std::uint8_t> metadata_bytes_;
    std::size_t offset_ = 0;
    Metadata metadata_;
    Metadata fallback_metadata_;
    bool has_fallback_ = false;
    unsigned fallback_generation_ = 0;
    std::vector<Entry> entries_;
    bool read_open_ = false;
    bool write_open_ = false;
    bool commit_job_ = false;
    bool cleanup_stages_after_failure_ = false;
    std::uint32_t read_size_ = 0;
    unsigned metadata_generation_ = 0;
    Metadata metadata_candidates_[2];
    bool metadata_valid_[2] = {false, false};
    std::vector<std::uint8_t> metadata_records_[2];

    bool recovery_complete_ = false;
    bool recovery_cleanup_stages_ = false;
    std::size_t scan_seen_ = 0;
    std::size_t scan_index_ = 0;
    std::size_t cleanup_index_ = 0;
    std::vector<ScanRecord> scan_records_;
    std::vector<StageRecord> stage_records_;
    char list_path_[CacheConfig::MAX_PATH_BYTES] = {};
    std::size_t pending_metadata_bytes_ = 0;
    std::vector<std::uint8_t> pending_metadata_record_;
    bool recovery_retry_record_ = false;
    bool namespace_authoritative_ = true;
    std::vector<CacheKey> conflicted_keys_;

    CacheKey eviction_key_;
    bool eviction_pending_ = false;
    int eviction_generation_ = -1;
    CacheResult quota_result_ = CacheResult::STORED;
    bool quota_recovery_ = false;

    static constexpr std::size_t VERIFY_SCRATCH_BYTES = 1024;
    std::array<std::uint8_t, VERIFY_SCRATCH_BYTES> verify_scratch_{};
    std::uint64_t verify_hash_ = 1469598103934665603ULL;
    bool verify_match_ = true;

    std::string stageBody() const;
    std::string stageMeta() const;
    static bool sameKey(const CacheKey&, const CacheKey&);
    static bool sequenceNewer(std::uint32_t, std::uint32_t);
    static bool addWouldOverflow(std::size_t, std::size_t);
    bool saneTime(std::uint64_t now) const;
    bool encodeMetadata();
    bool decodeMetadata(const std::vector<std::uint8_t>&, Metadata&) const;
    StorageResult readStep(const std::string&, std::vector<std::uint8_t>&,
                           std::size_t limit, bool& complete);
    StorageResult readBodyStep(const std::string&, std::size_t limit, bool& complete);
    void fail(CacheResult);
    void finishCleanup();
    void lookupMetadata();
    void lookupBody();
    void finishCommit();
    void beginQuotaEviction();
    void finishEviction();
    void finishRecovery();
    void finishQuotaReconciliation();
    bool removeStep(const std::string&, Operation);
    bool parseOwnedMetadataPath(const char*, ScanRecord&) const;
    bool parseOwnedStagePath(const char*, StageRecord&) const;
    std::string scanMetadataPath() const;
    std::string scanBodyPath() const;
    std::size_t inactiveBytes(const CacheKey&, unsigned) const;
    void eraseGeneration(const CacheKey&, unsigned);
};

}}} // namespace UI::LXMF::NomadNet
#endif
