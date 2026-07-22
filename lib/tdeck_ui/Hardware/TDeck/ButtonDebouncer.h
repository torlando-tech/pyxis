// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_BUTTON_DEBOUNCER_H
#define HARDWARE_TDECK_BUTTON_DEBOUNCER_H

#include <cstdint>

namespace Hardware {
namespace TDeck {

// Portable, symmetric debounce state machine. `raw_pressed` is the logical
// active state (true means pressed), independent of GPIO polarity.
class ButtonDebouncer {
public:
    explicit ButtonDebouncer(uint32_t interval_ms)
        : interval_ms_(interval_ms) {}

    bool update(bool raw_pressed, uint32_t now_ms) {
        changed_ = false;
        if (!initialized_) {
            initialized_ = true;
            raw_pressed_ = raw_pressed;
            stable_pressed_ = raw_pressed;
            raw_changed_at_ms_ = now_ms;
            return stable_pressed_;
        }

        if (raw_pressed != raw_pressed_) {
            raw_pressed_ = raw_pressed;
            raw_changed_at_ms_ = now_ms;
        }

        if (stable_pressed_ != raw_pressed_ &&
            static_cast<uint32_t>(now_ms - raw_changed_at_ms_) >= interval_ms_) {
            stable_pressed_ = raw_pressed_;
            changed_ = true;
        }
        return stable_pressed_;
    }

    bool stable() const { return stable_pressed_; }
    bool changed() const { return changed_; }

private:
    uint32_t interval_ms_;
    uint32_t raw_changed_at_ms_ = 0;
    bool initialized_ = false;
    bool raw_pressed_ = false;
    bool stable_pressed_ = false;
    bool changed_ = false;
};

}  // namespace TDeck
}  // namespace Hardware

#endif  // HARDWARE_TDECK_BUTTON_DEBOUNCER_H
