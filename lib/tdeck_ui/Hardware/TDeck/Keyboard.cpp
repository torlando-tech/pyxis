// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "Keyboard.h"

#ifdef ARDUINO

#include "Log.h"

using namespace RNS;

namespace Hardware {
namespace TDeck {

TwoWire* Keyboard::_wire = nullptr;
bool Keyboard::_initialized = false;
lv_indev_t* Keyboard::_indev = nullptr;
char Keyboard::_key_buffer[Kbd::MAX_KEYS_BUFFERED];
uint8_t Keyboard::_buffer_head = 0;
uint8_t Keyboard::_buffer_tail = 0;
uint8_t Keyboard::_buffer_count = 0;
uint32_t Keyboard::_last_poll_time = 0;
uint32_t Keyboard::_last_key_time = 0;

bool Keyboard::init(TwoWire& wire) {
    if (_initialized) {
        return true;
    }

    INFO("Initializing T-Deck keyboard");

    // Initialize hardware first
    if (!init_hardware_only(wire)) {
        return false;
    }

    // Register LVGL input device
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_KEYPAD;
    indev_drv.read_cb = lvgl_read_cb;

    _indev = lv_indev_drv_register(&indev_drv);
    if (!_indev) {
        ERROR("Failed to register keyboard with LVGL");
        return false;
    }

    INFO("Keyboard initialized successfully");
    return true;
}

lv_indev_t* Keyboard::get_indev() {
    return _indev;
}

bool Keyboard::init_hardware_only(TwoWire& wire) {
    if (_initialized) {
        return true;
    }

    INFO("Initializing keyboard hardware");

    _wire = &wire;

    // Verify keyboard is present by checking I2C address
    _wire->beginTransmission(I2C::KEYBOARD_ADDR);
    uint8_t result = _wire->endTransmission();
    if (result != 0) {
        WARNING("  Keyboard not found on I2C bus");
        _wire = nullptr;
        return false;
    }

    // Clear any pending keys by reading and discarding
    _wire->requestFrom(I2C::KEYBOARD_ADDR, (uint8_t)1);
    while (_wire->available()) {
        _wire->read();
    }

    // Clear buffer
    clear_buffer();

    _initialized = true;
    INFO("  Keyboard hardware ready");
    return true;
}

uint8_t Keyboard::poll() {
    if (!_initialized || !_wire) {
        return 0;
    }

    uint32_t now = millis();
    if (now - _last_poll_time < Kbd::POLL_INTERVAL_MS) {
        return 0;  // Don't poll too frequently
    }
    _last_poll_time = now;

    // T-Deck keyboard uses simple protocol: just read 1 byte directly
    // Returns ASCII key code if pressed, 0 if no key
    uint8_t bytes_read = _wire->requestFrom(I2C::KEYBOARD_ADDR, (uint8_t)1);
    if (bytes_read != 1) {
        return 0;
    }

    uint8_t key = _wire->read();

    // No key pressed or invalid
    if (key == KEY_NONE || key == 0xFF) {
        return 0;
    }

    // Add key to buffer
    buffer_push((char)key);
    return 1;
}

char Keyboard::read_key() {
    if (_buffer_count == 0) {
        return 0;
    }
    return buffer_pop();
}

bool Keyboard::available() {
    return _buffer_count > 0;
}

uint8_t Keyboard::get_key_count() {
    return _buffer_count;
}

void Keyboard::clear_buffer() {
    _buffer_head = 0;
    _buffer_tail = 0;
    _buffer_count = 0;
}

uint8_t Keyboard::get_firmware_version() {
    uint8_t version = 0;
    if (!read_register(Register::REG_VERSION, &version)) {
        return 0;
    }
    return version;
}

void Keyboard::set_backlight(uint8_t brightness) {
    if (!_wire || !_initialized) {
        return;
    }

    // Command 0x01 = LILYGO_KB_BRIGHTNESS_CMD
    _wire->beginTransmission(I2C::KEYBOARD_ADDR);
    _wire->write(0x01);
    _wire->write(brightness);
    _wire->endTransmission();
}

void Keyboard::backlight_on() {
    set_backlight(255);
}

void Keyboard::backlight_off() {
    set_backlight(0);
}

void Keyboard::lvgl_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
    // Safety check - LVGL should never pass null but be defensive
    if (!data) {
        return;
    }

    // Skip if not properly initialized
    if (!_initialized || !_wire) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = 0;
        data->continue_reading = false;
        return;
    }

    // Poll for new keys
    poll();

    // Read next key from buffer
    char key = read_key();

    if (key != 0) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = key;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = 0;
    }

    // Continue reading is set automatically by LVGL based on state
    data->continue_reading = (available() > 0);
}

bool Keyboard::write_register(uint8_t reg, uint8_t value) {
    if (!_wire) {
        return false;
    }

    _wire->beginTransmission(I2C::KEYBOARD_ADDR);
    _wire->write(reg);
    _wire->write(value);
    uint8_t result = _wire->endTransmission();

    return (result == 0);
}

bool Keyboard::read_register(uint8_t reg, uint8_t* value) {
    if (!_wire) {
        return false;
    }

    // Write register address
    _wire->beginTransmission(I2C::KEYBOARD_ADDR);
    _wire->write(reg);
    uint8_t result = _wire->endTransmission(false);  // Send repeated start

    if (result != 0) {
        return false;
    }

    // Read register value
    if (_wire->requestFrom(I2C::KEYBOARD_ADDR, (uint8_t)1) != 1) {
        return false;
    }

    *value = _wire->read();
    return true;
}

bool Keyboard::read_registers(uint8_t reg, uint8_t* buffer, size_t len) {
    if (!_wire) {
        return false;
    }

    // Write register address
    _wire->beginTransmission(I2C::KEYBOARD_ADDR);
    _wire->write(reg);
    uint8_t result = _wire->endTransmission(false);  // Send repeated start

    if (result != 0) {
        return false;
    }

    // Read register values
    if (_wire->requestFrom(I2C::KEYBOARD_ADDR, (uint8_t)len) != len) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        buffer[i] = _wire->read();
    }

    return true;
}

void Keyboard::buffer_push(char key) {
    if (_buffer_count >= Kbd::MAX_KEYS_BUFFERED) {
        // Buffer full, drop oldest key
        buffer_pop();
    }

    _key_buffer[_buffer_tail] = key;
    _buffer_tail = (_buffer_tail + 1) % Kbd::MAX_KEYS_BUFFERED;
    _buffer_count++;
    _last_key_time = millis();
}

uint32_t Keyboard::get_last_key_time() {
    return _last_key_time;
}

char Keyboard::buffer_pop() {
    if (_buffer_count == 0) {
        return 0;
    }

    char key = _key_buffer[_buffer_head];
    _buffer_head = (_buffer_head + 1) % Kbd::MAX_KEYS_BUFFERED;
    _buffer_count--;

    return key;
}

} // namespace TDeck
} // namespace Hardware

#endif // ARDUINO
