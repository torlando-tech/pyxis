// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_CALLSTARTMAILBOX_H
#define UI_LXMF_CALLSTARTMAILBOX_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace UI {
namespace LXMF {

// Atomic SPSC handoff for an exact 128-bit peer hash. The producer first owns
// EMPTY by changing it to WRITING, then publishes all four words before READY.
// The consumer reads words only while READY and returns the slot to EMPTY only
// after copying them, so the producer can never overwrite a payload being read.
// A second request is rejected rather than replacing a pending request.
class CallStartMailbox {
public:
    using PeerHash = std::array<uint8_t, 16>;

    bool request(const PeerHash& peerHash) {
        uint32_t expected = EMPTY;
        if (!_state.compare_exchange_strong(
                expected, WRITING, std::memory_order_seq_cst,
                std::memory_order_seq_cst)) {
            return false;
        }

        for (size_t word = 0; word < _words.size(); ++word) {
            _words[word].store(packWord(peerHash, word),
                               std::memory_order_seq_cst);
        }
        _state.store(READY, std::memory_order_seq_cst);
        return true;
    }

    bool take(PeerHash& peerHash) {
        if (_state.load(std::memory_order_seq_cst) != READY) return false;

        PeerHash snapshot{};
        for (size_t word = 0; word < _words.size(); ++word) {
            unpackWord(_words[word].load(std::memory_order_seq_cst),
                       snapshot, word);
        }

        uint32_t expected = READY;
        if (!_state.compare_exchange_strong(
                expected, EMPTY, std::memory_order_seq_cst,
                std::memory_order_seq_cst)) {
            return false;
        }
        peerHash = snapshot;
        return true;
    }

private:
    static constexpr uint32_t EMPTY = 0;
    static constexpr uint32_t WRITING = 1;
    static constexpr uint32_t READY = 2;

    static uint32_t packWord(const PeerHash& peerHash, size_t word) {
        const size_t offset = word * 4;
        return static_cast<uint32_t>(peerHash[offset]) |
               (static_cast<uint32_t>(peerHash[offset + 1]) << 8) |
               (static_cast<uint32_t>(peerHash[offset + 2]) << 16) |
               (static_cast<uint32_t>(peerHash[offset + 3]) << 24);
    }

    static void unpackWord(uint32_t packed, PeerHash& peerHash, size_t word) {
        const size_t offset = word * 4;
        peerHash[offset] = static_cast<uint8_t>(packed);
        peerHash[offset + 1] = static_cast<uint8_t>(packed >> 8);
        peerHash[offset + 2] = static_cast<uint8_t>(packed >> 16);
        peerHash[offset + 3] = static_cast<uint8_t>(packed >> 24);
    }

    std::array<std::atomic<uint32_t>, 4> _words{};
    std::atomic<uint32_t> _state{EMPTY};
};

} // namespace LXMF
} // namespace UI

#endif // UI_LXMF_CALLSTARTMAILBOX_H
