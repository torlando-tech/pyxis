// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_OUTGOINGSENDBOARD_H
#define UI_LXMF_OUTGOINGSENDBOARD_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace UI {
namespace LXMF {

// Single-slot handoff for one outgoing message from the LVGL task to the
// Arduino main loop. The LVGL task only publishes (destination, content,
// source); the main loop performs identity recall, signing/pack, the
// RouterLock-scoped router admission, and the LittleFS persistence there, so
// a multi-second save can never hold the LVGL mutex past the 5s deadlock
// guard (LVGLLock.h:45) and trip the assert. The user's input is retained on
// the screen until the main loop confirms acceptance (see
// UIManager::apply_outbound_result).
//
// One producer (LVGL task), one consumer (main loop). The mutex covers a
// small copy (strings are moved out on take). A second send while one is
// pending is rejected and its input is retained for retry — the same UX as
// the router-busy path.
class OutgoingSendMailbox {
public:
    enum class Source : uint8_t {
        None,
        Chat,
        Compose,
    };

    struct Slot {
        Source source = Source::None;
        std::string destination;  // raw peer-hash bytes (binary-safe)
        std::string content;      // UTF-8 message text
        uint32_t enqueued_ms = 0;  // millis() at request() — queue-wait telemetry
    };

    // Returns false when a send is already pending (caller retains input).
    bool request(Source source, const void* destination, size_t destinationSize,
                 const char* content, size_t contentSize) {
        std::lock_guard<std::mutex> guard(_mutex);
        if (_slot.source != Source::None) return false;
        _slot.source = source;
        _slot.destination.assign(static_cast<const char*>(destination), destinationSize);
        _slot.content.assign(content, contentSize);
        _slot.enqueued_ms = millis();
        return true;
    }

    // Returns false when nothing is pending; the slot is consumed on success.
    bool take(Slot& slot) {
        std::lock_guard<std::mutex> guard(_mutex);
        if (_slot.source == Source::None) return false;
        slot = std::move(_slot);
        _slot = Slot{};
        return true;
    }

    bool hasPending() const {
        std::lock_guard<std::mutex> guard(_mutex);
        return _slot.source != Source::None;
    }

private:
    mutable std::mutex _mutex;
    Slot _slot{};
};

} // namespace LXMF
} // namespace UI

#endif // UI_LXMF_OUTGOINGSENDBOARD_H
