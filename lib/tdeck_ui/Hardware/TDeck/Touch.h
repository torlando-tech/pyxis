// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_TOUCH_H
#define HARDWARE_TDECK_TOUCH_H

#include "Config.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

namespace Hardware {
namespace TDeck {

/**
 * GT911 Capacitive Touch Driver for T-Deck Plus
 *
 * CRITICAL: Uses POLLING MODE ONLY - interrupts cause crashes!
 *
 * The GT911 shares the I2C bus with the keyboard. It supports up to
 * 5 simultaneous touch points but typically we only use 1 for UI navigation.
 *
 * Features:
 * - Multi-touch support (up to 5 points)
 * - Polling mode with 10ms interval
 * - LVGL touchpad integration
 * - Automatic I2C address detection (0x5D or 0x14)
 */
class Touch {
public:
    /**
     * Touch point data
     */
    struct TouchPoint {
        uint16_t x;
        uint16_t y;
        uint16_t size;   // Touch area size
        uint8_t track_id;  // Touch tracking ID
        bool valid;
    };

    /**
     * Initialize touch controller
     * @param wire Wire interface to use (default Wire)
     * @return true if initialization successful
     */
    static bool init(TwoWire& wire = Wire);

    /**
     * Initialize touch without LVGL (for hardware testing)
     * @param wire Wire interface to use (default Wire)
     * @return true if initialization successful
     */
    static bool init_hardware_only(TwoWire& wire = Wire);

    /**
     * Poll touch controller for new touch data
     * Should be called periodically (e.g., every 10ms)
     * @return number of touch points detected
     */
    static uint8_t poll();

    /**
     * Get touch point data
     * @param index Touch point index (0-4)
     * @param point Output touch point data
     * @return true if point is valid
     */
    static bool get_point(uint8_t index, TouchPoint& point);

    /**
     * Check if screen is currently touched
     * @return true if at least one touch point detected
     */
    static bool is_touched();

    /**
     * Get number of touch points
     * @return number of active touch points
     */
    static uint8_t get_touch_count();

    /**
     * LVGL touchpad input callback
     * Do not call directly - used internally by LVGL
     */
    static void lvgl_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data);

    /**
     * Get product ID (for diagnostics)
     * @return product ID string, or empty if read failed
     */
    static String get_product_id();

private:
    // Touch state
    static TwoWire* _wire;
    static bool _initialized;
    static uint8_t _i2c_addr;
    static TouchPoint _points[Tch::MAX_TOUCH_POINTS];
    static uint8_t _touch_count;
    static uint32_t _last_poll_time;

    // I2C communication
    static bool write_register(uint16_t reg, uint8_t value);
    static bool read_register(uint16_t reg, uint8_t* value);
    static bool read_registers(uint16_t reg, uint8_t* buffer, size_t len);

    // Initialization helpers
    static bool detect_i2c_address();
    static bool verify_product_id();
};

} // namespace TDeck
} // namespace Hardware

#endif // ARDUINO
#endif // HARDWARE_TDECK_TOUCH_H
