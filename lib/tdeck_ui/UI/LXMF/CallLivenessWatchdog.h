#pragma once

#include <cstdint>
#include <atomic>

/**
 * Wrap-safe liveness guard for continuously streamed LXST call media.
 *
 * It is armed only after a call becomes ACTIVE. Every valid inbound audio frame
 * refreshes the observation timestamp. A long absence therefore provides a
 * conservative fallback when both the application terminal signal and
 * Reticulum Link-close notification are lost.
 */
class CallLivenessWatchdog {
public:
    void arm(uint32_t now_ms) {
        _last_observed_ms.store(now_ms, std::memory_order_relaxed);
        _armed.store(true, std::memory_order_release);
    }

    void observe(uint32_t now_ms) {
        if (_armed.load(std::memory_order_acquire)) {
            _last_observed_ms.store(now_ms, std::memory_order_relaxed);
        }
    }

    void disarm() {
        _armed.store(false, std::memory_order_release);
        _last_observed_ms.store(0, std::memory_order_relaxed);
    }

    bool expired(uint32_t now_ms, uint32_t timeout_ms) const {
        if (!_armed.load(std::memory_order_acquire)) return false;
        const uint32_t last = _last_observed_ms.load(std::memory_order_relaxed);
        return static_cast<uint32_t>(now_ms - last) >= timeout_ms;
    }

private:
    std::atomic<uint32_t> _last_observed_ms{0};
    std::atomic<bool> _armed{false};
};
