// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_KEYBOARD_H
#define HARDWARE_TDECK_KEYBOARD_H

#include "Config.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

namespace Hardware {
namespace TDeck {

/**
 * T-Deck Keyboard Driver (ESP32-C3 I2C Controller)
 *
 * The T-Deck keyboard is controlled by an ESP32-C3 microcontroller
 * that communicates via I2C at address 0x55. Key presses are buffered
 * and can be read via I2C registers.
 *
 * Features:
 * - ASCII key code reading
 * - Key buffer (up to 8 keys)
 * - LVGL keyboard input driver integration
 * - Modifier keys (Shift, Alt, Ctrl, Fn)
 */
class Keyboard {
public:
    /**
     * Initialize keyboard I2C communication
     * @param wire Wire interface to use (default Wire)
     * @return true if initialization successful
     */
    static bool init(TwoWire& wire = Wire);

    /**
     * Initialize keyboard without LVGL (for hardware testing)
     * @param wire Wire interface to use (default Wire)
     * @return true if initialization successful
     */
    static bool init_hardware_only(TwoWire& wire = Wire);

    /**
     * Poll keyboard for new key presses
     * Should be called periodically (e.g., every 10ms)
     * @return number of new keys available
     */
    static uint8_t poll();

    /**
     * Read next key from buffer
     * @return ASCII key code, or 0 if buffer empty
     */
    static char read_key();

    /**
     * Check if keys are available in buffer
     * @return true if keys available
     */
    static bool available();

    /**
     * Get number of keys in buffer
     * @return number of keys buffered
     */
    static uint8_t get_key_count();

    /**
     * Clear key buffer
     */
    static void clear_buffer();

    /**
     * LVGL keyboard input callback
     * Do not call directly - used internally by LVGL
     */
    static void lvgl_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data);

    /**
     * Get keyboard firmware version (for diagnostics)
     * @return firmware version, or 0 if read failed
     */
    static uint8_t get_firmware_version();

    /**
     * Get the LVGL input device for the keyboard
     * @return LVGL input device, or nullptr if not initialized
     */
    static lv_indev_t* get_indev();

    /**
     * Set keyboard backlight brightness
     * @param brightness 0-255 (0 = off, 255 = max)
     */
    static void set_backlight(uint8_t brightness);

    /**
     * Turn keyboard backlight on (max brightness)
     */
    static void backlight_on();

    /**
     * Turn keyboard backlight off
     */
    static void backlight_off();

    /**
     * Get time of last key press
     * @return millis() timestamp of last key press, or 0 if never
     */
    static uint32_t get_last_key_time();

private:
    // Special key codes
    enum SpecialKey : uint8_t {
        KEY_NONE = 0x00,
        KEY_BACKSPACE = 0x08,
        KEY_TAB = 0x09,
        KEY_ENTER = 0x0D,
        KEY_ESC = 0x1B,
        KEY_DELETE = 0x7F,
        KEY_UP = 0x80,
        KEY_DOWN = 0x81,
        KEY_LEFT = 0x82,
        KEY_RIGHT = 0x83,
        KEY_FN = 0x90,
        KEY_SYM = 0x91,
        KEY_MIC = 0x92
    };

    // Register addresses
    enum Register : uint8_t {
        REG_VERSION = 0x00,
        REG_KEY_STATE = 0x01,
        REG_KEY_COUNT = 0x02,
        REG_KEY_DATA = 0x03
    };

    static TwoWire* _wire;
    static bool _initialized;
    static lv_indev_t* _indev;
    static char _key_buffer[Kbd::MAX_KEYS_BUFFERED];
    static uint8_t _buffer_head;
    static uint8_t _buffer_tail;
    static uint8_t _buffer_count;
    static uint32_t _last_poll_time;
    static uint32_t _last_key_time;

    // I2C communication
    static bool write_register(uint8_t reg, uint8_t value);
    static bool read_register(uint8_t reg, uint8_t* value);
    static bool read_registers(uint8_t reg, uint8_t* buffer, size_t len);

    // Buffer management
    static void buffer_push(char key);
    static char buffer_pop();
};

} // namespace TDeck
} // namespace Hardware

#endif // ARDUINO
#endif // HARDWARE_TDECK_KEYBOARD_H
