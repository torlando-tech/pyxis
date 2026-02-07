// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "Touch.h"

#ifdef ARDUINO

#include "Log.h"

using namespace RNS;

namespace Hardware {
namespace TDeck {

TwoWire* Touch::_wire = nullptr;
bool Touch::_initialized = false;
uint8_t Touch::_i2c_addr = I2C::TOUCH_ADDR_1;
Touch::TouchPoint Touch::_points[Tch::MAX_TOUCH_POINTS];
uint8_t Touch::_touch_count = 0;
uint32_t Touch::_last_poll_time = 0;

bool Touch::init(TwoWire& wire) {
    if (_initialized) {
        return true;
    }

    INFO("Initializing T-Deck touch controller");

    // Initialize hardware first
    if (!init_hardware_only(wire)) {
        return false;
    }

    // Register LVGL input device
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_read_cb;

    lv_indev_t* indev = lv_indev_drv_register(&indev_drv);
    if (!indev) {
        ERROR("Failed to register touch controller with LVGL");
        return false;
    }

    INFO("Touch controller initialized successfully");
    return true;
}

bool Touch::init_hardware_only(TwoWire& wire) {
    if (_initialized) {
        return true;
    }

    INFO("Initializing touch hardware");

    _wire = &wire;

    // Detect I2C address (0x5D or 0x14)
    if (!detect_i2c_address()) {
        ERROR("  Failed to detect GT911 I2C address");
        return false;
    }

    INFO(("  GT911 detected at address 0x" + String(_i2c_addr, HEX)).c_str());

    // Verify product ID
    if (!verify_product_id()) {
        WARNING("  Could not verify GT911 product ID (may not be critical)");
    }

    // Clear initial touch state
    for (uint8_t i = 0; i < Tch::MAX_TOUCH_POINTS; i++) {
        _points[i].valid = false;
    }
    _touch_count = 0;

    _initialized = true;
    INFO("  Touch hardware ready");
    return true;
}

uint8_t Touch::poll() {
    if (!_initialized) {
        return 0;
    }

    uint32_t now = millis();
    if (now - _last_poll_time < Tch::POLL_INTERVAL_MS) {
        return _touch_count;  // Don't poll too frequently
    }
    _last_poll_time = now;

    // Read status register
    uint8_t status = 0;
    if (!read_register(Tch::REG_STATUS, &status)) {
        return 0;
    }

    // Check if touch data is ready
    bool buffer_status = (status & 0x80) != 0;
    uint8_t touch_count = status & 0x0F;

    if (!buffer_status || touch_count == 0) {
        // No touch or data not ready
        _touch_count = 0;
        for (uint8_t i = 0; i < Tch::MAX_TOUCH_POINTS; i++) {
            _points[i].valid = false;
        }

        // Clear status register
        write_register(Tch::REG_STATUS, 0x00);
        return 0;
    }

    // Limit to max supported points
    if (touch_count > Tch::MAX_TOUCH_POINTS) {
        touch_count = Tch::MAX_TOUCH_POINTS;
    }

    // Read touch point data
    _touch_count = 0;
    for (uint8_t i = 0; i < touch_count; i++) {
        uint8_t point_data[8];
        uint16_t point_reg = Tch::REG_POINT_1 + (i * 8);

        if (read_registers(point_reg, point_data, 8)) {
            uint8_t track_id = point_data[0];
            uint16_t x = point_data[1] | (point_data[2] << 8);
            uint16_t y = point_data[3] | (point_data[4] << 8);
            uint16_t size = point_data[5] | (point_data[6] << 8);

            // Validate coordinates
            if (x < Tch::RAW_WIDTH && y < Tch::RAW_HEIGHT) {
                _points[i].x = x;
                _points[i].y = y;
                _points[i].size = size;
                _points[i].track_id = track_id;
                _points[i].valid = true;
                _touch_count++;
            } else {
                _points[i].valid = false;
            }
        } else {
            _points[i].valid = false;
        }
    }

    // Clear remaining points
    for (uint8_t i = touch_count; i < Tch::MAX_TOUCH_POINTS; i++) {
        _points[i].valid = false;
    }

    // Clear status register
    write_register(Tch::REG_STATUS, 0x00);

    return _touch_count;
}

bool Touch::get_point(uint8_t index, TouchPoint& point) {
    if (index >= Tch::MAX_TOUCH_POINTS) {
        return false;
    }

    if (!_points[index].valid) {
        return false;
    }

    point = _points[index];
    return true;
}

bool Touch::is_touched() {
    return _touch_count > 0;
}

uint8_t Touch::get_touch_count() {
    return _touch_count;
}

String Touch::get_product_id() {
    uint8_t product_id[4];
    if (!read_registers(Tch::REG_PRODUCT_ID, product_id, 4)) {
        return "";
    }

    char id_str[5];
    id_str[0] = (char)product_id[0];
    id_str[1] = (char)product_id[1];
    id_str[2] = (char)product_id[2];
    id_str[3] = (char)product_id[3];
    id_str[4] = '\0';

    return String(id_str);
}

void Touch::lvgl_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
    // Poll for new touch data
    poll();

    // Get first touch point
    TouchPoint point;
    if (get_point(0, point)) {
        data->state = LV_INDEV_STATE_PRESSED;

        // Transform touch coordinates for landscape display rotation
        // Display uses MADCTL with MX|MV for landscape (rotation=1)
        // GT911 reports in native portrait orientation:
        //   - raw X: 0-239 (portrait width, maps to screen Y after rotation)
        //   - raw Y: 0-319 (portrait height, maps to screen X after rotation)
        // Transform: swap X/Y and invert the new Y axis
        data->point.x = point.y;
        data->point.y = Disp::HEIGHT - 1 - point.x;  // Use display height (240), not raw height
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }

    // Continue reading is set automatically by LVGL based on state
}

bool Touch::write_register(uint16_t reg, uint8_t value) {
    if (!_wire) {
        return false;
    }

    _wire->beginTransmission(_i2c_addr);
    _wire->write((uint8_t)(reg >> 8));   // Register high byte
    _wire->write((uint8_t)(reg & 0xFF)); // Register low byte
    _wire->write(value);
    uint8_t result = _wire->endTransmission();

    return (result == 0);
}

bool Touch::read_register(uint16_t reg, uint8_t* value) {
    if (!_wire) {
        return false;
    }

    // Write register address
    _wire->beginTransmission(_i2c_addr);
    _wire->write((uint8_t)(reg >> 8));   // Register high byte
    _wire->write((uint8_t)(reg & 0xFF)); // Register low byte
    uint8_t result = _wire->endTransmission(false);  // Send repeated start

    if (result != 0) {
        return false;
    }

    // Read register value
    if (_wire->requestFrom(_i2c_addr, (uint8_t)1) != 1) {
        return false;
    }

    *value = _wire->read();
    return true;
}

bool Touch::read_registers(uint16_t reg, uint8_t* buffer, size_t len) {
    if (!_wire) {
        return false;
    }

    // Write register address
    _wire->beginTransmission(_i2c_addr);
    _wire->write((uint8_t)(reg >> 8));   // Register high byte
    _wire->write((uint8_t)(reg & 0xFF)); // Register low byte
    uint8_t result = _wire->endTransmission(false);  // Send repeated start

    if (result != 0) {
        return false;
    }

    // Read register values
    if (_wire->requestFrom(_i2c_addr, (uint8_t)len) != len) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        buffer[i] = _wire->read();
    }

    return true;
}

bool Touch::detect_i2c_address() {
    // Try primary address first (0x5D)
    _i2c_addr = I2C::TOUCH_ADDR_1;
    _wire->beginTransmission(_i2c_addr);
    uint8_t result = _wire->endTransmission();

    if (result == 0) {
        return true;
    }

    // Try alternative address (0x14)
    _i2c_addr = I2C::TOUCH_ADDR_2;
    _wire->beginTransmission(_i2c_addr);
    result = _wire->endTransmission();

    return (result == 0);
}

bool Touch::verify_product_id() {
    String product_id = get_product_id();
    if (product_id.length() == 0) {
        return false;
    }

    // GT911 should return "911" as product ID
    if (product_id.indexOf("911") >= 0) {
        INFO(("  Touch product ID: " + product_id).c_str());
        return true;
    }

    WARNING(("  Unexpected touch product ID: " + product_id).c_str());
    return false;
}

} // namespace TDeck
} // namespace Hardware

#endif // ARDUINO
