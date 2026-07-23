// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_CALLCOMMANDMAILBOX_H
#define UI_LXMF_CALLCOMMANDMAILBOX_H

#include <atomic>
#include <cstdint>

namespace UI {
namespace LXMF {

// Single-consumer mailbox for commands issued outside loopTask. Mute and
// hangup use separate atomic slots so publishing a mute can never overwrite a
// pending hangup. Generation zero means "no active call" and is never queued.
class CallCommandMailbox {
public:
    static constexpr uint32_t MAX_GENERATION = 0x7fffffffu;

    enum class Action {
        NONE,
        MUTE,
        HANGUP,
    };

    struct Command {
        Action action = Action::NONE;
        bool muted = false;
    };

    struct Commands {
        uint32_t hangupGeneration = 0;
        uint32_t muteGeneration = 0;
        bool muted = false;
    };

    void requestHangup(uint32_t generation) {
        if (!validGeneration(generation)) return;
        _hangupGeneration.store(generation, std::memory_order_release);
    }

    void requestMute(uint32_t generation, bool muted) {
        if (!validGeneration(generation)) return;
        const uint32_t packed = generation | (muted ? MUTE_BIT : 0u);
        _mute.store(packed, std::memory_order_release);
    }

    // Atomically consumes each slot. A producer racing this exchange either
    // appears in this result or remains pending for the next take().
    Commands take() {
        Commands commands;
        commands.hangupGeneration =
            _hangupGeneration.exchange(0, std::memory_order_acq_rel);
        const uint32_t mute = _mute.exchange(0, std::memory_order_acq_rel);
        commands.muteGeneration = mute & MAX_GENERATION;
        commands.muted = (mute & MUTE_BIT) != 0;
        return commands;
    }

    // Consume all pending slots and select the command applicable to the
    // active call. Stale generations are discarded, and hangup deliberately
    // wins when both commands target the current generation.
    Command takeForGeneration(uint32_t activeGeneration) {
        const Commands commands = take();
        if (!validGeneration(activeGeneration)) return {};
        Command command;
        if (commands.hangupGeneration == activeGeneration) {
            command.action = Action::HANGUP;
            return command;
        }
        if (commands.muteGeneration == activeGeneration) {
            command.action = Action::MUTE;
            command.muted = commands.muted;
            return command;
        }
        return {};
    }

private:
    static constexpr uint32_t MUTE_BIT = 0x80000000u;

    static bool validGeneration(uint32_t generation) {
        return generation != 0 && generation <= MAX_GENERATION;
    }

    std::atomic<uint32_t> _hangupGeneration{0};
    std::atomic<uint32_t> _mute{0};
};

} // namespace LXMF
} // namespace UI

#endif // UI_LXMF_CALLCOMMANDMAILBOX_H
