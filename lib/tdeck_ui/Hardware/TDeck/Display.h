// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_DISPLAY_H
#define HARDWARE_TDECK_DISPLAY_H

#include "Config.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <SPI.h>
#include <lvgl.h>

namespace Hardware {
namespace TDeck {

/**
 * ST7789V Display Driver for T-Deck Plus
 *
 * Provides:
 * - SPI initialization for ST7789V controller
 * - Backlight control via PWM
 * - LVGL display flush callback
 * - Display rotation support
 *
 * Memory: Uses PSRAM for LVGL buffers (2 x 320x240x2 bytes = 307KB)
 */
class Display {
public:
    /**
     * Initialize display and LVGL
     * @return true if initialization successful
     */
    static bool init();

    /**
     * Initialize display without LVGL (for hardware testing)
     * @return true if initialization successful
     */
    static bool init_hardware_only();

    /**
     * Set backlight brightness
     * @param brightness 0-255 (0=off, 255=max)
     */
    static void set_brightness(uint8_t brightness);

    /**
     * Get current backlight brightness
     * @return brightness 0-255
     */
    static uint8_t get_brightness();

    /**
     * Turn display on/off
     * @param on true to turn on, false to turn off
     */
    static void set_power(bool on);

    /**
     * Fill entire display with color (for testing)
     * @param color RGB565 color
     */
    static void fill_screen(uint16_t color);

    /**
     * Draw rectangle (for testing)
     */
    static void draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

    /**
     * LVGL flush callback - called by LVGL to update display
     * Do not call directly - used internally by LVGL
     */
    static void lvgl_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p);

private:
    // SPI commands for ST7789V
    enum Command : uint8_t {
        NOP = 0x00,
        SWRESET = 0x01,
        RDDID = 0x04,
        RDDST = 0x09,

        SLPIN = 0x10,
        SLPOUT = 0x11,
        PTLON = 0x12,
        NORON = 0x13,

        INVOFF = 0x20,
        INVON = 0x21,
        DISPOFF = 0x28,
        DISPON = 0x29,
        CASET = 0x2A,
        RASET = 0x2B,
        RAMWR = 0x2C,
        RAMRD = 0x2E,

        PTLAR = 0x30,
        COLMOD = 0x3A,
        MADCTL = 0x36,

        FRMCTR1 = 0xB1,
        FRMCTR2 = 0xB2,
        FRMCTR3 = 0xB3,
        INVCTR = 0xB4,
        DISSET5 = 0xB6,

        PWCTR1 = 0xC0,
        PWCTR2 = 0xC1,
        PWCTR3 = 0xC2,
        PWCTR4 = 0xC3,
        PWCTR5 = 0xC4,
        VMCTR1 = 0xC5,

        PWCTR6 = 0xFC,

        GMCTRP1 = 0xE0,
        GMCTRN1 = 0xE1
    };

    // MADCTL bits
    enum MADCTL : uint8_t {
        MY = 0x80,   // Row address order
        MX = 0x40,   // Column address order
        MV = 0x20,   // Row/column exchange
        ML = 0x10,   // Vertical refresh order
        RGB = 0x00,  // RGB order
        BGR = 0x08,  // BGR order
        MH = 0x04    // Horizontal refresh order
    };

    static SPIClass* _spi;
    static uint8_t _brightness;
    static bool _initialized;

    // Low-level SPI functions
    static void write_command(uint8_t cmd);
    static void write_data(uint8_t data);
    static void write_data(const uint8_t* data, size_t len);
    static void set_addr_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
    static void begin_write();
    static void end_write();

    // Initialization sequence
    static void init_registers();
};

} // namespace TDeck
} // namespace Hardware

#endif // ARDUINO
#endif // HARDWARE_TDECK_DISPLAY_H
