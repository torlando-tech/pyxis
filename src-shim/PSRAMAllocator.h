/*
 * PSRAMAllocator.h - Custom STL allocator for ESP32 PSRAM
 *
 * Routes std::vector allocations to PSRAM (external RAM) when available,
 * falling back to internal heap if PSRAM allocation fails.
 *
 * This helps prevent internal heap fragmentation by keeping large
 * data structures in PSRAM.
 *
 * Based on: https://github.com/atanisoft/esp32usb/blob/master/include/psram_allocator.h
 */
#pragma once

#include <cstddef>
#include <new>

#ifdef ARDUINO
#include <esp_heap_caps.h>
#else
// Non-ESP32 platforms: use standard allocation
#include <cstdlib>
#endif

template <class T>
class PSRAMAllocator
{
public:
    using value_type = T;

    PSRAMAllocator() noexcept {}

    template <class U>
    constexpr PSRAMAllocator(const PSRAMAllocator<U>&) noexcept {}

    [[nodiscard]] value_type* allocate(std::size_t n)
    {
#ifdef ARDUINO
#if CONFIG_SPIRAM || defined(BOARD_HAS_PSRAM)
        // Attempt to allocate in PSRAM first
        auto p = static_cast<value_type*>(
            heap_caps_malloc(n * sizeof(value_type), MALLOC_CAP_SPIRAM));
        if (p)
        {
            return p;
        }
#endif // CONFIG_SPIRAM || BOARD_HAS_PSRAM

        // If PSRAM allocation failed (or not available), try default memory pool
        auto p2 = static_cast<value_type*>(
            heap_caps_malloc(n * sizeof(value_type), MALLOC_CAP_DEFAULT));
        if (p2)
        {
            return p2;
        }
#else
        // Non-Arduino: use standard malloc
        auto p = static_cast<value_type*>(std::malloc(n * sizeof(value_type)));
        if (p)
        {
            return p;
        }
#endif

        throw std::bad_alloc();
    }

    void deallocate(value_type* p, std::size_t) noexcept
    {
#ifdef ARDUINO
        heap_caps_free(p);
#else
        std::free(p);
#endif
    }
};

template <class T, class U>
bool operator==(const PSRAMAllocator<T>&, const PSRAMAllocator<U>&)
{
    return true;
}

template <class T, class U>
bool operator!=(const PSRAMAllocator<T>& x, const PSRAMAllocator<U>& y)
{
    return !(x == y);
}
