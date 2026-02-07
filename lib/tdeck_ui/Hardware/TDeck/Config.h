// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_CONFIG_H
#define HARDWARE_TDECK_CONFIG_H

#include <cstdint>

namespace Hardware {
namespace TDeck {

/**
 * T-Deck Plus Hardware Configuration
 *
 * Hardware: LilyGO T-Deck Plus
 * MCU: ESP32-S3
 * Display: 320x240 IPS (ST7789V)
 * Keyboard: ESP32-C3 I2C controller
 * Touch: GT911 capacitive (shares I2C bus with keyboard)
 * Trackball: GPIO pulse-based navigation
 */

namespace Pin {
    // Display (ST7789V SPI)
    constexpr uint8_t DISPLAY_CS = 12;
    constexpr uint8_t DISPLAY_DC = 11;
    constexpr uint8_t DISPLAY_BACKLIGHT = 42;
    constexpr uint8_t DISPLAY_MOSI = 41;  // SPI MOSI
    constexpr uint8_t DISPLAY_SCK = 40;   // SPI CLK
    constexpr uint8_t DISPLAY_RST = -1;   // No reset pin (shared reset)

    // I2C Bus (shared by keyboard and touch)
    constexpr uint8_t I2C_SDA = 18;
    constexpr uint8_t I2C_SCL = 8;

    // Keyboard (ESP32-C3 controller on I2C)
    // No additional pins needed - uses I2C bus

    // Touch (GT911 on I2C)
    constexpr uint8_t TOUCH_INT = -1;     // NOT USED - polling mode only
    constexpr uint8_t TOUCH_RST = -1;     // NOT USED

    // Trackball (GPIO pulses) - per ESPP t-deck.hpp
    constexpr uint8_t TRACKBALL_UP = 15;
    constexpr uint8_t TRACKBALL_DOWN = 3;
    constexpr uint8_t TRACKBALL_LEFT = 1;
    constexpr uint8_t TRACKBALL_RIGHT = 2;
    constexpr uint8_t TRACKBALL_BUTTON = 0;

    // Power management
    constexpr uint8_t POWER_EN = 10;      // Power enable pin
    constexpr uint8_t BATTERY_ADC = 4;    // Battery voltage ADC

    // GPS (L76K or UBlox M10Q - built-in on T-Deck Plus)
    // Pin naming from ESP32's perspective (matches LilyGo convention)
    constexpr uint8_t GPS_TX = 43;        // ESP32 TX -> GPS RX (ESP32 transmits)
    constexpr uint8_t GPS_RX = 44;        // ESP32 RX <- GPS TX (ESP32 receives)
}

namespace I2C {
    // I2C addresses
    constexpr uint8_t KEYBOARD_ADDR = 0x55;
    constexpr uint8_t TOUCH_ADDR_1 = 0x5D;   // Primary GT911 address
    constexpr uint8_t TOUCH_ADDR_2 = 0x14;   // Alternative GT911 address

    // I2C timing
    constexpr uint32_t FREQUENCY = 400000;   // 400kHz
    constexpr uint32_t TIMEOUT_MS = 100;
}

namespace Disp {
    // Display dimensions
    constexpr uint16_t WIDTH = 320;
    constexpr uint16_t HEIGHT = 240;
    constexpr uint8_t ROTATION = 1;  // Landscape mode

    // SPI configuration
    constexpr uint32_t SPI_FREQUENCY = 40000000;  // 40MHz
    constexpr uint8_t SPI_HOST = 1;  // HSPI

    // Backlight PWM
    constexpr uint8_t BACKLIGHT_CHANNEL = 0;
    constexpr uint32_t BACKLIGHT_FREQ = 5000;  // 5kHz PWM
    constexpr uint8_t BACKLIGHT_RESOLUTION = 8;  // 8-bit (0-255)
    constexpr uint8_t BACKLIGHT_DEFAULT = 180;   // Default brightness
}

namespace Kbd {
    // Keyboard register addresses (ESP32-C3 controller)
    constexpr uint8_t REG_KEY_STATE = 0x01;      // Key state register
    constexpr uint8_t REG_KEY_COUNT = 0x02;      // Number of keys pressed
    constexpr uint8_t REG_KEY_DATA = 0x03;       // Key data buffer start

    // Maximum keys in buffer
    constexpr uint8_t MAX_KEYS_BUFFERED = 8;

    // Polling interval
    constexpr uint32_t POLL_INTERVAL_MS = 10;
}

namespace Tch {
    // GT911 register addresses
    constexpr uint16_t REG_CONFIG = 0x8047;      // Config start
    constexpr uint16_t REG_PRODUCT_ID = 0x8140;  // Product ID
    constexpr uint16_t REG_STATUS = 0x814E;      // Touch status
    constexpr uint16_t REG_POINT_1 = 0x814F;     // First touch point

    // Touch configuration
    constexpr uint8_t MAX_TOUCH_POINTS = 5;

    // Polling interval (MUST use polling mode - interrupts cause crashes)
    constexpr uint32_t POLL_INTERVAL_MS = 10;

    // Touch calibration (raw coordinates in GT911's native portrait orientation)
    // GT911 reports X for short edge (240) and Y for long edge (320)
    constexpr uint16_t RAW_WIDTH = 240;   // Portrait width (short edge)
    constexpr uint16_t RAW_HEIGHT = 320;  // Portrait height (long edge)
}

namespace Trk {
    // Debounce timing
    constexpr uint32_t DEBOUNCE_MS = 10;

    // Pulse counting for movement speed
    constexpr uint32_t PULSE_RESET_MS = 50;  // Reset pulse count after 50ms idle

    // Movement sensitivity
    constexpr uint8_t PIXELS_PER_PULSE = 5;

    // Focus navigation (KEYPAD mode)
    constexpr uint8_t NAV_THRESHOLD = 1;      // Pulses needed to trigger focus move
    constexpr uint32_t KEY_REPEAT_MS = 100;   // Min time between repeated key events
}

namespace Power {
    // Battery voltage divider (adjust based on hardware)
    constexpr float BATTERY_VOLTAGE_DIVIDER = 2.0;

    // Battery levels (in volts)
    constexpr float BATTERY_FULL = 4.2;
    constexpr float BATTERY_EMPTY = 3.3;
}

namespace Radio {
    // SX1262 LoRa Radio pins
    constexpr uint8_t LORA_CS = 9;        // Chip select
    constexpr uint8_t LORA_BUSY = 13;     // SX1262 busy signal
    constexpr uint8_t LORA_RST = 17;      // Reset
    constexpr uint8_t LORA_DIO1 = 45;     // Interrupt (DIO1, not DIO0)
    // Note: Uses shared SPI bus with display (SCK=40, MOSI=41, MISO=38)
    constexpr uint8_t SPI_MISO = 38;      // SPI MISO (LoRa only, display is write-only)
}

namespace Audio {
    // I2S speaker output pins
    constexpr uint8_t I2S_BCK = 7;        // Bit clock
    constexpr uint8_t I2S_WS = 5;         // Word select (LRCK)
    constexpr uint8_t I2S_DOUT = 6;       // Data out
    // Note: Pin::POWER_EN (10) must be HIGH to enable speaker power
}

namespace SDCard {
    // SD Card pins (shares SPI bus with display and LoRa)
    constexpr uint8_t CS = 39;            // SD card chip select
    // SPI bus shared: SCK=40, MOSI=41, MISO=38
}

} // namespace TDeck
} // namespace Hardware

#endif // HARDWARE_TDECK_CONFIG_H
