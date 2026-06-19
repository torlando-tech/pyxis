#pragma once

/**
 * BytesPool.h - Pool for Bytes Data objects to prevent heap fragmentation
 *
 * Problem: Bytes uses shared_ptr<Data> where Data = vector<uint8_t>.
 * Each make_shared allocates a control block (24 bytes) + vector metadata.
 * In long-running firmware, this causes heap fragmentation.
 *
 * Solution: Pool the Data objects (vectors) themselves. When a Bytes object
 * needs storage for common sizes (<=1024 bytes), it gets a pre-allocated
 * Data from the pool with capacity already reserved. When the shared_ptr
 * refcount hits 0, a custom deleter returns the Data to the pool instead
 * of destroying it.
 *
 * The pool has four tiers sized for Reticulum packet processing:
 *   - 64 bytes (1024 slots): hashes (16-32 bytes), small fields - highest traffic
 *   - 256 bytes (16 slots): keys, small announces
 *   - 512 bytes (12 slots): standard packets (MTU=500 + margin)
 *   - 1024 bytes (12 slots): resource advertisements, large packets
 *
 * All storage arrays are dynamically allocated in PSRAM on ESP32 to avoid
 * consuming internal RAM (BSS). Only the pointers (~32 bytes) stay in BSS.
 *
 * Thread-safe via FreeRTOS spinlock (ESP32) or std::mutex (native).
 *
 * Usage (in Bytes.cpp):
 *   auto [data, tier] = BytesPool::instance().acquire(capacity);
 *   if (data) {
 *       // Got pooled Data - use with custom deleter
 *       _data = SharedData(data, BytesPoolDeleter{tier});
 *   } else {
 *       // Pool exhausted - fall back to make_shared
 *       _data = std::make_shared<Data>();
 *   }
 */

#include "PSRAMAllocator.h"
#include "Log.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// FreeRTOS spinlock support - only on ESP32
#if defined(ESP_PLATFORM) || defined(ARDUINO)
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <esp_heap_caps.h>
#define BYTESPOOL_USE_SPINLOCK 1
#else
// Native build - use std::mutex instead
#include <mutex>
#define BYTESPOOL_USE_SPINLOCK 0
#endif

namespace RNS {

// Pool configuration - sized for Reticulum packet processing
// MTU=500, most packets fit in 512 bytes, large resources may need 1024
namespace BytesPoolConfig {
    static constexpr size_t TIER_TINY = 64;       // Hashes (16-32 bytes), small fields
    static constexpr size_t TIER_SMALL = 256;     // Small packets, keys
    static constexpr size_t TIER_MEDIUM = 512;    // Standard packets
    static constexpr size_t TIER_LARGE = 1024;    // Large packets, resource ads

    // Slot counts per tier — tuned 2026-02-19
    // Storage arrays now live in PSRAM, so internal RAM cost is only pointers (~32B).
    // Tiny tier for transient packet processing only. Known destinations now use
    // fixed buffers (zero pool slots). 1024 provides ample headroom for packet
    // hashes, Transport tables, and burst announce processing.
    static constexpr size_t TINY_SLOTS = 1024;    // Transient packet processing (hashes, keys, fields)
    static constexpr size_t SMALL_SLOTS = 16;     // Keys, small announces
    static constexpr size_t MEDIUM_SLOTS = 12;    // Standard packets
    static constexpr size_t LARGE_SLOTS = 12;     // Resource ads, large packets

    // Tier identifiers for deleter
    enum Tier : uint8_t {
        TIER_NONE = 0,    // Not from pool (fallback allocation)
        TIER_64 = 1,
        TIER_256 = 2,
        TIER_512 = 3,
        TIER_1024 = 4
    };
}

// Forward declaration - Data is vector with PSRAMAllocator
using PooledData = std::vector<uint8_t, PSRAMAllocator<uint8_t>>;

/**
 * Pool for Bytes Data objects (vectors).
 *
 * Each tier maintains a stack of pre-allocated vectors with capacity reserved.
 * Vectors are cleared (size=0) but capacity preserved when returned to pool.
 *
 * This eliminates:
 *   - Repeated vector construction/destruction
 *   - Repeated capacity reservation allocations
 *   - shared_ptr control block allocations (via make_shared replacement)
 *
 * Memory footprint (tuned 2026-02-19, all storage in PSRAM):
 *   - Tiny: 1024 slots x 64 bytes = 64KB backing + ~16KB metadata (PSRAM)
 *   - Small: 16 slots x 256 bytes = 4KB backing + ~256B metadata (PSRAM)
 *   - Medium: 12 slots x 512 bytes = 6KB backing + ~192B metadata (PSRAM)
 *   - Large: 12 slots x 1024 bytes = 12KB backing + ~192B metadata (PSRAM)
 *   - Total: ~103KB PSRAM, ~32 bytes internal RAM (pointers only)
 */
class BytesPool {
public:
    // Singleton access - pool must be global for custom deleter
    static BytesPool& instance() {
        static BytesPool pool;
        return pool;
    }

    /**
     * Acquire a Data object from pool.
     * Returns {pointer, tier} or {nullptr, TIER_NONE} if pool exhausted.
     *
     * The returned Data is empty (size=0) but has capacity >= requested.
     * Caller must use the tier value to construct BytesPoolDeleter.
     */
    std::pair<PooledData*, BytesPoolConfig::Tier> acquire(size_t requested_capacity) {
        _total_requests++;

#if BYTESPOOL_USE_SPINLOCK
        portENTER_CRITICAL(&_mux);
#else
        std::lock_guard<std::mutex> lock(_mutex);
#endif

        PooledData* result = nullptr;
        BytesPoolConfig::Tier tier = BytesPoolConfig::TIER_NONE;

        // Try smallest tier that fits
        if (requested_capacity <= BytesPoolConfig::TIER_TINY) {
            if (_tiny_count > 0) {
                result = _tiny_stack[--_tiny_count];
                tier = BytesPoolConfig::TIER_64;
                _pool_hits++;
            }
        }
        else if (requested_capacity <= BytesPoolConfig::TIER_SMALL) {
            if (_small_count > 0) {
                result = _small_stack[--_small_count];
                tier = BytesPoolConfig::TIER_256;
                _pool_hits++;
            }
        }
        else if (requested_capacity <= BytesPoolConfig::TIER_MEDIUM) {
            if (_medium_count > 0) {
                result = _medium_stack[--_medium_count];
                tier = BytesPoolConfig::TIER_512;
                _pool_hits++;
            }
        }
        else if (requested_capacity <= BytesPoolConfig::TIER_LARGE) {
            if (_large_count > 0) {
                result = _large_stack[--_large_count];
                tier = BytesPoolConfig::TIER_1024;
                _pool_hits++;
            }
        }
        // Oversized requests fall through with nullptr

        if (!result) {
            _pool_misses++;
        }

#if BYTESPOOL_USE_SPINLOCK
        portEXIT_CRITICAL(&_mux);
#endif

        return {result, tier};
    }

    /**
     * Release a Data object back to pool.
     * Called by BytesPoolDeleter when shared_ptr refcount hits 0.
     *
     * The Data is cleared (preserving capacity) and pushed to tier stack.
     */
    void release(PooledData* data, BytesPoolConfig::Tier tier) {
        if (!data || tier == BytesPoolConfig::TIER_NONE) {
            // Not from pool - should not happen, but defensive
            return;
        }

        // Clear but preserve capacity
        data->clear();

#if BYTESPOOL_USE_SPINLOCK
        portENTER_CRITICAL(&_mux);
#else
        std::lock_guard<std::mutex> lock(_mutex);
#endif

        switch (tier) {
            case BytesPoolConfig::TIER_64:
                if (_tiny_count < BytesPoolConfig::TINY_SLOTS) {
                    _tiny_stack[_tiny_count++] = data;
                }
                // else pool full - data leaks (shouldn't happen in normal operation)
                break;
            case BytesPoolConfig::TIER_256:
                if (_small_count < BytesPoolConfig::SMALL_SLOTS) {
                    _small_stack[_small_count++] = data;
                }
                break;
            case BytesPoolConfig::TIER_512:
                if (_medium_count < BytesPoolConfig::MEDIUM_SLOTS) {
                    _medium_stack[_medium_count++] = data;
                }
                break;
            case BytesPoolConfig::TIER_1024:
                if (_large_count < BytesPoolConfig::LARGE_SLOTS) {
                    _large_stack[_large_count++] = data;
                }
                break;
            default:
                break;
        }

#if BYTESPOOL_USE_SPINLOCK
        portEXIT_CRITICAL(&_mux);
#endif
    }

    // Instrumentation
    size_t total_requests() const { return _total_requests; }
    size_t pool_hits() const { return _pool_hits; }
    size_t pool_misses() const { return _pool_misses; }
    size_t fallback_count() const { return _fallback_count; }
    float hit_rate() const {
        return _total_requests > 0 ? (float)_pool_hits / _total_requests : 0.0f;
    }

    /**
     * Record a fallback to heap allocation and log WARNING.
     * Called by Bytes.cpp when pool is exhausted but fallback succeeds.
     */
    void recordFallback(size_t requested_size) {
        _fallback_count++;
        WARNINGF("BytesPool: exhausted, falling back to heap (requested=%zu bytes, "
                 "tiny=%zu/%zu small=%zu/%zu med=%zu/%zu large=%zu/%zu)",
                 requested_size,
                 tiny_in_use(), BytesPoolConfig::TINY_SLOTS,
                 small_in_use(), BytesPoolConfig::SMALL_SLOTS,
                 medium_in_use(), BytesPoolConfig::MEDIUM_SLOTS,
                 large_in_use(), BytesPoolConfig::LARGE_SLOTS);
    }

    // Current pool state
    size_t tiny_available() const { return _tiny_count; }
    size_t small_available() const { return _small_count; }
    size_t medium_available() const { return _medium_count; }
    size_t large_available() const { return _large_count; }
    size_t tiny_in_use() const { return BytesPoolConfig::TINY_SLOTS - _tiny_count; }
    size_t small_in_use() const { return BytesPoolConfig::SMALL_SLOTS - _small_count; }
    size_t medium_in_use() const { return BytesPoolConfig::MEDIUM_SLOTS - _medium_count; }
    size_t large_in_use() const { return BytesPoolConfig::LARGE_SLOTS - _large_count; }

    // Log statistics for tuning
    void logStats() const {
        INFOF("BytesPool: requests=%zu hits=%zu misses=%zu fallbacks=%zu hit_rate=%d%% "
              "tiny=%zu/%zu small=%zu/%zu med=%zu/%zu large=%zu/%zu",
              _total_requests, _pool_hits, _pool_misses, _fallback_count,
              (int)(hit_rate() * 100),
              tiny_in_use(), BytesPoolConfig::TINY_SLOTS,
              small_in_use(), BytesPoolConfig::SMALL_SLOTS,
              medium_in_use(), BytesPoolConfig::MEDIUM_SLOTS,
              large_in_use(), BytesPoolConfig::LARGE_SLOTS);
    }

private:
    BytesPool() {
#if BYTESPOOL_USE_SPINLOCK
        portMUX_INITIALIZE(&_mux);
#endif
        allocateStorage();
        initializeTier(_tiny_storage, _tiny_stack, _tiny_count,
                       BytesPoolConfig::TIER_TINY, BytesPoolConfig::TINY_SLOTS);
        initializeTier(_small_storage, _small_stack, _small_count,
                       BytesPoolConfig::TIER_SMALL, BytesPoolConfig::SMALL_SLOTS);
        initializeTier(_medium_storage, _medium_stack, _medium_count,
                       BytesPoolConfig::TIER_MEDIUM, BytesPoolConfig::MEDIUM_SLOTS);
        initializeTier(_large_storage, _large_stack, _large_count,
                       BytesPoolConfig::TIER_LARGE, BytesPoolConfig::LARGE_SLOTS);
    }

    // Non-copyable
    BytesPool(const BytesPool&) = delete;
    BytesPool& operator=(const BytesPool&) = delete;

    // Allocate storage and stack arrays in PSRAM (ESP32) or heap (native)
    void allocateStorage() {
#if BYTESPOOL_USE_SPINLOCK
        // ESP32: allocate in PSRAM to avoid consuming internal RAM (BSS)
        #define POOL_ALLOC(ptr, type, count) do { \
            ptr = static_cast<type*>(heap_caps_aligned_alloc( \
                alignof(type), (count) * sizeof(type), \
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); \
            if (!ptr) { \
                ptr = static_cast<type*>(heap_caps_aligned_alloc( \
                    alignof(type), (count) * sizeof(type), \
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)); \
                if (ptr) WARNING("BytesPool: " #ptr " fell back to internal RAM"); \
            } \
        } while(0)

        POOL_ALLOC(_tiny_storage,   PooledData,  BytesPoolConfig::TINY_SLOTS);
        POOL_ALLOC(_tiny_stack,     PooledData*, BytesPoolConfig::TINY_SLOTS);
        POOL_ALLOC(_small_storage,  PooledData,  BytesPoolConfig::SMALL_SLOTS);
        POOL_ALLOC(_small_stack,    PooledData*, BytesPoolConfig::SMALL_SLOTS);
        POOL_ALLOC(_medium_storage, PooledData,  BytesPoolConfig::MEDIUM_SLOTS);
        POOL_ALLOC(_medium_stack,   PooledData*, BytesPoolConfig::MEDIUM_SLOTS);
        POOL_ALLOC(_large_storage,  PooledData,  BytesPoolConfig::LARGE_SLOTS);
        POOL_ALLOC(_large_stack,    PooledData*, BytesPoolConfig::LARGE_SLOTS);
        #undef POOL_ALLOC
#else
        // Native: use new[]
        _tiny_storage   = new PooledData[BytesPoolConfig::TINY_SLOTS];
        _tiny_stack     = new PooledData*[BytesPoolConfig::TINY_SLOTS];
        _small_storage  = new PooledData[BytesPoolConfig::SMALL_SLOTS];
        _small_stack    = new PooledData*[BytesPoolConfig::SMALL_SLOTS];
        _medium_storage = new PooledData[BytesPoolConfig::MEDIUM_SLOTS];
        _medium_stack   = new PooledData*[BytesPoolConfig::MEDIUM_SLOTS];
        _large_storage  = new PooledData[BytesPoolConfig::LARGE_SLOTS];
        _large_stack    = new PooledData*[BytesPoolConfig::LARGE_SLOTS];
#endif
    }

    // Initialize a tier with pre-allocated vectors
    void initializeTier(PooledData* storage, PooledData** stack, size_t& count,
                        size_t capacity, size_t slots) {
        if (!storage || !stack) {
            ERROR("BytesPool: allocation failed for tier, pool will be undersized");
            count = 0;
            return;
        }
        for (size_t i = 0; i < slots; i++) {
            // Placement new to construct in allocated storage
            new (&storage[i]) PooledData();
            storage[i].reserve(capacity);
            stack[i] = &storage[i];
        }
        count = slots;
    }

    // Storage for pooled vectors — dynamically allocated (PSRAM on ESP32)
    PooledData* _tiny_storage = nullptr;
    PooledData* _small_storage = nullptr;
    PooledData* _medium_storage = nullptr;
    PooledData* _large_storage = nullptr;

    // Stacks of available vectors (pointers into storage arrays)
    PooledData** _tiny_stack = nullptr;
    PooledData** _small_stack = nullptr;
    PooledData** _medium_stack = nullptr;
    PooledData** _large_stack = nullptr;

    // Stack counts (how many available in each tier)
    size_t _tiny_count = 0;
    size_t _small_count = 0;
    size_t _medium_count = 0;
    size_t _large_count = 0;

    // Instrumentation counters
    size_t _total_requests = 0;
    size_t _pool_hits = 0;
    size_t _pool_misses = 0;
    size_t _fallback_count = 0;  // Heap fallbacks due to pool exhaustion

#if BYTESPOOL_USE_SPINLOCK
    portMUX_TYPE _mux;
#else
    std::mutex _mutex;
#endif
};

/**
 * Custom deleter for shared_ptr that returns Data to pool.
 *
 * When shared_ptr refcount hits 0, this deleter is called instead of delete.
 * The Data is returned to the appropriate tier in BytesPool.
 */
struct BytesPoolDeleter {
    BytesPoolConfig::Tier tier;

    explicit BytesPoolDeleter(BytesPoolConfig::Tier t = BytesPoolConfig::TIER_NONE)
        : tier(t) {}

    void operator()(PooledData* data) const {
        if (data && tier != BytesPoolConfig::TIER_NONE) {
            // Return to pool instead of destroying
            BytesPool::instance().release(data, tier);
        }
        // Note: We never delete the data - it lives in BytesPool's storage arrays
        // If tier is TIER_NONE, this is a fallback allocation and won't be called
    }
};

} // namespace RNS
