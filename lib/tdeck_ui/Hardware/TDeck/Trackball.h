// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_TRACKBALL_H
#define HARDWARE_TDECK_TRACKBALL_H

#include "Config.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <lvgl.h>

namespace Hardware {
namespace TDeck {

/**
 * T-Deck Trackball Driver (GPIO Pulse-based)
 *
 * The trackball generates GPIO pulses on directional movement.
 * We count pulses and convert them to cursor movement.
 *
 * Features:
 * - 4-direction movement (up, down, left, right)
 * - Center button press
 * - Pulse counting for movement speed
 * - LVGL encoder integration
 * - Debouncing
 */
class Trackball {
public:
    /**
     * Trackball state
     */
    struct State {
        int16_t delta_x;      // Horizontal movement (-n to +n)
        int16_t delta_y;      // Vertical movement (-n to +n)
        bool button_pressed;  // Center button state
        uint32_t timestamp;   // Last update timestamp
    };

    /**
     * Initialize trackball GPIO pins and interrupts
     * @return true if initialization successful
     */
    static bool init();

    /**
     * Initialize trackball without LVGL (for hardware testing)
     * @return true if initialization successful
     */
    static bool init_hardware_only();

    /**
     * Poll trackball for movement and button state
     * Should be called periodically (e.g., every 10ms)
     * @return true if state changed
     */
    static bool poll();

    /**
     * Get current trackball state
     * @param state Output state structure
     */
    static void get_state(State& state);

    /**
     * Reset accumulated movement deltas
     */
    static void reset_deltas();

    /**
     * Check if button is currently pressed
     * @return true if button pressed
     */
    static bool is_button_pressed();

    /**
     * LVGL input callback
     * Do not call directly - used internally by LVGL
     */
    static void lvgl_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data);

    /**
     * Get the LVGL input device for the trackball
     * @return LVGL input device, or nullptr if not initialized
     */
    static lv_indev_t* get_indev();

private:
    // LVGL input device
    static lv_indev_t* _indev;

    // VOLATILE RATIONALE: ISR pulse counters
    //
    // These are modified by hardware interrupt handlers (IRAM_ATTR ISRs)
    // and read by the main task for trackball input processing.
    //
    // Volatile required because:
    // - ISR context modifies values asynchronously
    // - Without volatile, compiler may cache values in registers
    // - 16-bit/32-bit aligned values are atomic on ESP32
    //
    // Reference: ESP-IDF GPIO interrupt documentation
    static volatile int16_t _pulse_up;
    static volatile int16_t _pulse_down;
    static volatile int16_t _pulse_left;
    static volatile int16_t _pulse_right;
    static volatile uint32_t _last_pulse_time;

    // Button state
    static bool _button_pressed;
    static uint32_t _last_button_time;

    // State tracking
    static State _state;
    static bool _initialized;

    // ISR handlers (ESP-IDF gpio_isr_handler signature)
    static void IRAM_ATTR isr_up(void* arg);
    static void IRAM_ATTR isr_down(void* arg);
    static void IRAM_ATTR isr_left(void* arg);
    static void IRAM_ATTR isr_right(void* arg);

    // Button debouncing
    static bool read_button_debounced();
};

} // namespace TDeck
} // namespace Hardware

#endif // ARDUINO
#endif // HARDWARE_TDECK_TRACKBALL_H
