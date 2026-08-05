#pragma once

#include <cstddef>
#include <new>
#include <vector>

#ifdef ARDUINO
#include <esp32-hal-psram.h>
#include <cstdlib>
#endif

namespace UI::LXMF::NomadNet {

// Large remote-page buffers must not compete with the ESP32-S3's constrained
// internal DRAM. The T-Deck build requires PSRAM; host tests use normal heap.
template <typename T>
class ExternalAllocator {
public:
    using value_type = T;

    ExternalAllocator() noexcept = default;
    template <typename U>
    ExternalAllocator(const ExternalAllocator<U>&) noexcept {}
    template <typename U>
    struct rebind { using other = ExternalAllocator<U>; };

    T* allocate(std::size_t count) {
        if (count == 0) return nullptr;
        if (count > static_cast<std::size_t>(-1) / sizeof(T)) throw std::bad_alloc();
#ifdef ARDUINO
        void* allocation = ps_malloc(count * sizeof(T));
#else
        void* allocation = ::operator new(count * sizeof(T));
#endif
        if (!allocation) throw std::bad_alloc();
        return static_cast<T*>(allocation);
    }

    void deallocate(T* allocation, std::size_t) noexcept {
#ifdef ARDUINO
        free(allocation);
#else
        ::operator delete(allocation);
#endif
    }

    template <typename U>
    bool operator==(const ExternalAllocator<U>&) const noexcept { return true; }
    template <typename U>
    bool operator!=(const ExternalAllocator<U>&) const noexcept { return false; }
};

template <typename T>
using ExternalVector = std::vector<T, ExternalAllocator<T>>;

} // namespace UI::LXMF::NomadNet
