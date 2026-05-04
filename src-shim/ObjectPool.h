#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

// FreeRTOS spinlock support - only on ESP32
#if defined(ESP_PLATFORM) || defined(ARDUINO)
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#define OBJECTPOOL_USE_SPINLOCK 1
#else
// Native build - use std::mutex instead
#include <mutex>
#define OBJECTPOOL_USE_SPINLOCK 0
#endif

namespace RNS {

/**
 * Fixed-size object pool with O(1) allocate/deallocate.
 * Thread-safe via spinlock (ESP32) or mutex (native).
 * Falls back to nullptr on exhaustion.
 *
 * Template parameters:
 *   T - Object type to pool
 *   N - Pool capacity (number of slots)
 *
 * Usage:
 *   ObjectPool<MyClass, 16> pool;
 *   MyClass* obj = pool.allocate();  // nullptr if exhausted
 *   if (obj) {
 *       // use object...
 *       pool.deallocate(obj);
 *   }
 *
 * Design notes:
 *   - Freelist stored in-place using union with storage
 *   - O(1) allocate: pop from freelist head
 *   - O(1) deallocate: push to freelist head
 *   - Placement new for construction, explicit destructor for cleanup
 *   - Pool exhaustion returns nullptr (caller should fall back to heap)
 */
template <typename T, size_t N>
class ObjectPool {
public:
    ObjectPool() : _first_free(0), _allocated_count(0) {
#if OBJECTPOOL_USE_SPINLOCK
        portMUX_INITIALIZE(&_mux);
#endif
        // Initialize freelist chain
        for (size_t i = 0; i < N - 1; i++) {
            _slots[i].next_free = i + 1;
        }
        _slots[N - 1].next_free = INVALID_SLOT;
    }

    /**
     * Allocate object from pool.
     * Returns nullptr if pool exhausted (caller should fall back to heap).
     * Thread-safe.
     *
     * Supports variadic constructor arguments via perfect forwarding.
     * Example: pool.allocate(arg1, arg2) calls T(arg1, arg2)
     */
    template<typename... Args>
    T* allocate(Args&&... args) {
#if OBJECTPOOL_USE_SPINLOCK
        portENTER_CRITICAL(&_mux);
#else
        std::lock_guard<std::mutex> lock(_mutex);
#endif
        if (_first_free == INVALID_SLOT) {
#if OBJECTPOOL_USE_SPINLOCK
            portEXIT_CRITICAL(&_mux);
#endif
            return nullptr;  // Pool exhausted
        }
        size_t slot = _first_free;
        _first_free = _slots[slot].next_free;
        _allocated_count++;
#if OBJECTPOOL_USE_SPINLOCK
        portEXIT_CRITICAL(&_mux);
#endif

        // Placement new to construct object with forwarded arguments
        return new (&_slots[slot].storage) T(std::forward<Args>(args)...);
    }

    /**
     * Return object to pool.
     * Pointer must have been obtained from this pool's allocate().
     * Thread-safe.
     */
    void deallocate(T* ptr) {
        if (!ptr) return;

        // Calculate slot index from pointer
        uintptr_t offset = reinterpret_cast<uintptr_t>(ptr) -
                          reinterpret_cast<uintptr_t>(_slots);
        size_t slot = offset / sizeof(Slot);

        if (slot >= N) {
            // Not from this pool - ignore (caller's responsibility)
            return;
        }

        // Explicit destructor call
        ptr->~T();

#if OBJECTPOOL_USE_SPINLOCK
        portENTER_CRITICAL(&_mux);
#else
        std::lock_guard<std::mutex> lock(_mutex);
#endif
        _slots[slot].next_free = _first_free;
        _first_free = slot;
        _allocated_count--;
#if OBJECTPOOL_USE_SPINLOCK
        portEXIT_CRITICAL(&_mux);
#endif
    }

    /**
     * Check if pointer was allocated from this pool.
     */
    bool owns(T* ptr) const {
        if (!ptr) return false;
        uintptr_t start = reinterpret_cast<uintptr_t>(_slots);
        uintptr_t end = start + sizeof(_slots);
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        return addr >= start && addr < end;
    }

    size_t allocated() const { return _allocated_count; }
    size_t capacity() const { return N; }
    size_t available() const { return N - _allocated_count; }

private:
    static constexpr size_t INVALID_SLOT = ~size_t(0);

    struct Slot {
        union {
            alignas(T) char storage[sizeof(T)];
            size_t next_free;
        };
    };

    Slot _slots[N];
    size_t _first_free;
    size_t _allocated_count;
#if OBJECTPOOL_USE_SPINLOCK
    portMUX_TYPE _mux;
#else
    std::mutex _mutex;
#endif
};

} // namespace RNS
