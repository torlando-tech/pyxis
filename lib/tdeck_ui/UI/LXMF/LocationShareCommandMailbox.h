#ifndef PYXIS_UI_LXMF_LOCATION_SHARE_COMMAND_MAILBOX_H
#define PYXIS_UI_LXMF_LOCATION_SHARE_COMMAND_MAILBOX_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace UI {
namespace LXMF {

// One-producer/one-consumer fixed mailbox. LVGL callbacks only publish a
// command; loopTask takes it before acquiring LVGL_LOCK and performs durable
// location start/stop there. A full mailbox fails closed instead of replacing
// a peer's pending consent operation.
class LocationShareCommandMailbox {
public:
    enum class Action : uint8_t { NONE, QUERY, START, STOP };
    struct Command {
        Action action = Action::NONE;
        uint8_t peer[16] = {};
        uint8_t duration = 0;
        uint32_t cadenceMillis = 0;
        bool hasApproximation = false;
        int32_t approximationMeters = 0;
    };

    bool requestStart(const uint8_t* peer, std::size_t peerSize,
                      uint8_t duration, uint32_t cadenceMillis,
                      bool hasApproximation, int32_t approximationMeters) {
        if (peer == NULL || peerSize != sizeof(command_.peer)) return false;
        uint8_t expected = EMPTY;
        if (!state_.compare_exchange_strong(expected, WRITING,
                                            std::memory_order_acq_rel)) return false;
        command_.action = Action::START;
        std::memcpy(command_.peer, peer, sizeof(command_.peer));
        command_.duration = duration;
        command_.cadenceMillis = cadenceMillis;
        command_.hasApproximation = hasApproximation;
        command_.approximationMeters = approximationMeters;
        state_.store(READY, std::memory_order_release);
        return true;
    }

    bool requestStop(const uint8_t* peer, std::size_t peerSize) {
        if (peer == NULL || peerSize != sizeof(command_.peer)) return false;
        uint8_t expected = EMPTY;
        if (!state_.compare_exchange_strong(expected, WRITING,
                                            std::memory_order_acq_rel)) return false;
        command_ = Command();
        command_.action = Action::STOP;
        std::memcpy(command_.peer, peer, sizeof(command_.peer));
        state_.store(READY, std::memory_order_release);
        return true;
    }

    bool requestQuery(const uint8_t* peer, std::size_t peerSize) {
        if (peer == NULL || peerSize != sizeof(command_.peer)) return false;
        uint8_t expected = EMPTY;
        if (!state_.compare_exchange_strong(expected, WRITING,
                                            std::memory_order_acq_rel)) return false;
        command_ = Command();
        command_.action = Action::QUERY;
        std::memcpy(command_.peer, peer, sizeof(command_.peer));
        state_.store(READY, std::memory_order_release);
        return true;
    }

    Command take() {
        uint8_t expected = READY;
        if (!state_.compare_exchange_strong(expected, READING,
                                            std::memory_order_acq_rel)) return Command();
        const Command output = command_;
        command_ = Command();
        state_.store(EMPTY, std::memory_order_release);
        return output;
    }

private:
    static const uint8_t EMPTY = 0;
    static const uint8_t WRITING = 1;
    static const uint8_t READY = 2;
    static const uint8_t READING = 3;
    std::atomic<uint8_t> state_{EMPTY};
    Command command_{};
};

} // namespace LXMF
} // namespace UI
#endif
