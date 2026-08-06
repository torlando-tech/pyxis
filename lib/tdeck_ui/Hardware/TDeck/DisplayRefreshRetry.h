// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_DISPLAY_REFRESH_RETRY_H
#define HARDWARE_TDECK_DISPLAY_REFRESH_RETRY_H

namespace Hardware {
namespace TDeck {

// Coalesces one or more unflushed LVGL regions into a single redraw request.
// The flush callback and consumer run on the LVGL task, so no synchronization
// or allocation is required.
class DisplayRefreshRetry {
public:
    void mark_failed() { _pending = true; }

    bool consume() {
        const bool pending = _pending;
        _pending = false;
        return pending;
    }

private:
    bool _pending = false;
};

} // namespace TDeck
} // namespace Hardware

#endif // HARDWARE_TDECK_DISPLAY_REFRESH_RETRY_H
