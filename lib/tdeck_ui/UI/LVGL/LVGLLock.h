// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LVGL_LVGLLOCK_H
#define UI_LVGL_LVGLLOCK_H

#ifdef ARDUINO

#include "LVGLInit.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace UI {
namespace LVGL {

/**
 * RAII lock for thread-safe LVGL access
 *
 * Usage:
 *   void SomeScreen::update() {
 *       LVGLLock lock;  // Acquires mutex
 *       lv_label_set_text(label, "Hello");
 *       // ... more LVGL calls ...
 *   }  // Mutex released when lock goes out of scope
 *
 * Or use the macro:
 *   void SomeScreen::update() {
 *       LVGL_LOCK();
 *       lv_label_set_text(label, "Hello");
 *   }
 */
class LVGLLock {
public:
    LVGLLock() {
        SemaphoreHandle_t mutex = LVGLInit::get_mutex();
        if (mutex) {
#ifndef NDEBUG
            // Debug builds: Use 5-second timeout for deadlock detection
            BaseType_t result = xSemaphoreTakeRecursive(mutex, pdMS_TO_TICKS(5000));
            if (result != pdTRUE) {
                // Log holder info if available
                TaskHandle_t holder = xSemaphoreGetMutexHolder(mutex);
                (void)holder;  // Suppress unused warning if logging disabled
                // Crash with diagnostic info
                assert(false && "LVGL mutex timeout (5s) - possible deadlock");
            }
            _acquired = true;
#else
            // Release builds: Wait indefinitely (production behavior)
            xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
            _acquired = true;
#endif
        }
    }

    ~LVGLLock() {
        if (_acquired) {
            SemaphoreHandle_t mutex = LVGLInit::get_mutex();
            if (mutex) {
                xSemaphoreGiveRecursive(mutex);
            }
        }
    }

    // Non-copyable
    LVGLLock(const LVGLLock&) = delete;
    LVGLLock& operator=(const LVGLLock&) = delete;

private:
    bool _acquired = false;
};

} // namespace LVGL
} // namespace UI

// Convenience macro - creates a scoped lock variable
#define LVGL_LOCK() UI::LVGL::LVGLLock _lvgl_lock_guard

#endif // ARDUINO
#endif // UI_LVGL_LVGLLOCK_H
