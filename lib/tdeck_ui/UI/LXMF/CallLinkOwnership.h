// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_CALLLINKOWNERSHIP_H
#define UI_LXMF_CALLLINKOWNERSHIP_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace UI {
namespace LXMF {

// Lock-free-shaped publication of the exact 128-bit Reticulum link ID together
// with its call generation. Atomic words avoid data races while the generation
// acts as a publication seqlock, so readers never accept a torn ID. Writers are
// serialized by CallGenerationGuard in UIManager.
class CallLinkOwnership {
public:
    using LinkId = std::array<uint8_t, 16>;
    static constexpr uint32_t MAX_GENERATION = 0x7fffffffu;

    bool publish(uint32_t generation, const LinkId& id) {
        if (generation == 0 || generation > MAX_GENERATION) return false;

        // Detach the previous publication before changing any ID word. Readers
        // that began against the old owner reject it on their final generation
        // read even if they observe a mixture of old and new words.
        _generation.store(0, std::memory_order_release);
        for (size_t word = 0; word < _id_words.size(); ++word) {
            _id_words[word].store(packWord(id, word), std::memory_order_relaxed);
        }
        _close_state.store(generation << 1, std::memory_order_release);
        _generation.store(generation, std::memory_order_release);
        return true;
    }

    bool owns(uint32_t generation, const LinkId& id) const {
        if (generation == 0 ||
            _generation.load(std::memory_order_acquire) != generation) {
            return false;
        }

        for (size_t word = 0; word < _id_words.size(); ++word) {
            if (_id_words[word].load(std::memory_order_relaxed) !=
                packWord(id, word)) {
                return false;
            }
        }

        // A writer may have detached and started replacing the ID after the
        // first generation read. Accept only a stable publication.
        return _generation.load(std::memory_order_acquire) == generation;
    }

    uint32_t generationFor(const LinkId& id) const {
        const uint32_t generation = _generation.load(std::memory_order_acquire);
        return owns(generation, id) ? generation : 0;
    }

    bool markClosed(uint32_t generation, const LinkId& id) {
        if (!owns(generation, id)) return false;

        uint32_t expected = generation << 1;
        return _close_state.compare_exchange_strong(
            expected, expected | 1u, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    // Returns the generation whose pending close was consumed, or zero. The
    // exact encoded generation prevents an old callback from clearing or
    // replacing a newer owner's pending close.
    uint32_t takeClosed() {
        const uint32_t generation = _generation.load(std::memory_order_acquire);
        if (generation == 0) return 0;

        uint32_t expected = (generation << 1) | 1u;
        if (!_close_state.compare_exchange_strong(
                expected, generation << 1, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return 0;
        }
        return _generation.load(std::memory_order_acquire) == generation
                   ? generation
                   : 0;
    }

    bool clear(uint32_t generation) {
        if (generation == 0) return false;

        uint32_t expected_generation = generation;
        if (!_generation.compare_exchange_strong(
                expected_generation, 0, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return false;
        }

        // A validated close callback may pause before its CAS. Keep clearing
        // only this generation's two exact states until neither remains; a
        // newer generation's slot is never overwritten.
        for (;;) {
            uint32_t state = _close_state.load(std::memory_order_acquire);
            if ((state >> 1) != generation) break;
            if (_close_state.compare_exchange_weak(
                    state, 0, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                break;
            }
        }
        return true;
    }

private:
    static uint32_t packWord(const LinkId& id, size_t word) {
        const size_t offset = word * 4;
        return static_cast<uint32_t>(id[offset]) |
               (static_cast<uint32_t>(id[offset + 1]) << 8) |
               (static_cast<uint32_t>(id[offset + 2]) << 16) |
               (static_cast<uint32_t>(id[offset + 3]) << 24);
    }

    std::array<std::atomic<uint32_t>, 4> _id_words{};
    std::atomic<uint32_t> _generation{0};
    // (generation << 1) | pending; generation is bounded to 31 bits.
    std::atomic<uint32_t> _close_state{0};
};

} // namespace LXMF
} // namespace UI

#endif // UI_LXMF_CALLLINKOWNERSHIP_H
