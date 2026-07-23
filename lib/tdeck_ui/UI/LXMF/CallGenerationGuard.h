// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_CALLGENERATIONGUARD_H
#define UI_LXMF_CALLGENERATIONGUARD_H

#include <atomic>
#include <cstdint>

namespace UI {
namespace LXMF {

// Lock-free admission guard for call setup. Each successful reservation owns a
// generation until that exact generation releases it; generation zero always
// means "unowned".
class CallGenerationGuard {
public:
    static constexpr uint32_t MAX_GENERATION = 0x7fffffffu;

    uint32_t tryReserve() {
        const uint32_t generation = nextGeneration();
        uint32_t expected = 0;
        if (!active.compare_exchange_strong(expected, generation,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            return 0;
        }
        return generation;
    }

    bool owns(uint32_t generation) const {
        return generation != 0 &&
               active.load(std::memory_order_acquire) == generation;
    }

    uint32_t current() const {
        return active.load(std::memory_order_acquire);
    }

    bool release(uint32_t generation) {
        if (generation == 0) return false;
        return active.compare_exchange_strong(generation, 0,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

private:
    uint32_t nextGeneration() {
        uint32_t generation = counter.load(std::memory_order_relaxed);
        for (;;) {
            const uint32_t next = generation == MAX_GENERATION
                                      ? 1u
                                      : generation + 1u;
            if (counter.compare_exchange_weak(generation, next,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
                return generation;
            }
        }
    }

    std::atomic<uint32_t> active{0};
    std::atomic<uint32_t> counter{1};
};

} // namespace LXMF
} // namespace UI

#endif // UI_LXMF_CALLGENERATIONGUARD_H
