// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "Display.h"

#ifdef ARDUINO

#include "Log.h"
#include <esp_heap_caps.h>

using namespace RNS;

namespace Hardware {
namespace TDeck {

SPIClass* Display::_spi = nullptr;
uint8_t Display::_brightness = Disp::BACKLIGHT_DEFAULT;
bool Display::_initialized = false;

bool Display::init() {
    if (_initialized) {
        return true;
    }

    INFO("Initializing T-Deck display");

    // Initialize hardware first
    if (!init_hardware_only()) {
        return false;
    }

    // Allocate LVGL buffers in PSRAM (2 buffers for double buffering)
    static lv_disp_draw_buf_t draw_buf;
    size_t buf_size = Disp::WIDTH * Disp::HEIGHT * sizeof(lv_color_t);

    lv_color_t* buf1 = (lv_color_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    lv_color_t* buf2 = (lv_color_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);

    if (!buf1 || !buf2) {
        ERROR("Failed to allocate LVGL buffers in PSRAM");
        if (buf1) heap_caps_free(buf1);
        if (buf2) heap_caps_free(buf2);
        return false;
    }

    INFO(("  LVGL buffers allocated in PSRAM (" + String(buf_size * 2) + " bytes)").c_str());

    // Initialize LVGL draw buffer
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, Disp::WIDTH * Disp::HEIGHT);

    // Register display driver with LVGL
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = Disp::WIDTH;
    disp_drv.ver_res = Disp::HEIGHT;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;

    lv_disp_t* disp = lv_disp_drv_register(&disp_drv);
    if (!disp) {
        ERROR("Failed to register display driver with LVGL");
        return false;
    }

    INFO("Display initialized successfully");
    return true;
}

bool Display::init_hardware_only() {
    if (_initialized) {
        return true;
    }

    INFO("Initializing display hardware");

    // Configure backlight PWM
    ledcSetup(Disp::BACKLIGHT_CHANNEL, Disp::BACKLIGHT_FREQ, Disp::BACKLIGHT_RESOLUTION);
    ledcAttachPin(Pin::DISPLAY_BACKLIGHT, Disp::BACKLIGHT_CHANNEL);
    set_brightness(_brightness);

    // Initialize SPI
    _spi = new SPIClass(HSPI);
    _spi->begin(Pin::DISPLAY_SCK, -1, Pin::DISPLAY_MOSI, Pin::DISPLAY_CS);

    // Configure CS and DC pins
    pinMode(Pin::DISPLAY_CS, OUTPUT);
    pinMode(Pin::DISPLAY_DC, OUTPUT);
    digitalWrite(Pin::DISPLAY_CS, HIGH);
    digitalWrite(Pin::DISPLAY_DC, HIGH);

    // Initialize ST7789V registers
    init_registers();

    _initialized = true;
    INFO("  Display hardware ready");
    return true;
}

void Display::init_registers() {
    INFO("  Configuring ST7789V registers");

    // Software reset
    write_command(Command::SWRESET);
    // DELAY RATIONALE: LCD reset pulse width
    // ST7789 datasheet specifies minimum 120ms reset low time for reliable initialization.
    // Using 150ms for margin. Shorter values cause display initialization failures.
    delay(150);

    // Sleep out
    write_command(Command::SLPOUT);
    // DELAY RATIONALE: SPI command settling - allow display controller to process command before next
    delay(10);

    // Color mode: 16-bit (RGB565)
    write_command(Command::COLMOD);
    write_data(0x55);  // 16-bit color

    // Memory data access control (rotation + RGB order)
    write_command(Command::MADCTL);
    uint8_t madctl = MADCTL::MX | MADCTL::MY | MADCTL::RGB;
    if (Disp::ROTATION == 1) {
        madctl = MADCTL::MX | MADCTL::MV | MADCTL::RGB;  // Landscape
    }
    write_data(madctl);

    // Inversion on (required for ST7789V panels)
    write_command(Command::INVON);
    // DELAY RATIONALE: SPI command settling - allow display controller to process command before next
    delay(10);

    // Normal display mode
    write_command(Command::NORON);
    // DELAY RATIONALE: SPI command settling - allow display controller to process command before next
    delay(10);

    // Display on
    write_command(Command::DISPON);
    // DELAY RATIONALE: SPI command settling - allow display controller to process command before next
    delay(10);

    // Clear screen to black
    fill_screen(0x0000);

    INFO("  ST7789V initialized");
}

void Display::set_brightness(uint8_t brightness) {
    _brightness = brightness;
    ledcWrite(Disp::BACKLIGHT_CHANNEL, brightness);
}

uint8_t Display::get_brightness() {
    return _brightness;
}

void Display::set_power(bool on) {
    if (on) {
        write_command(Command::DISPON);
        set_brightness(_brightness);
    } else {
        set_brightness(0);
        write_command(Command::DISPOFF);
    }
}

void Display::fill_screen(uint16_t color) {
    set_addr_window(0, 0, Disp::WIDTH - 1, Disp::HEIGHT - 1);

    begin_write();
    write_command(Command::RAMWR);

    // Send color data for entire screen
    uint8_t color_bytes[2];
    color_bytes[0] = (color >> 8) & 0xFF;
    color_bytes[1] = color & 0xFF;

    for (uint32_t i = 0; i < (uint32_t)Disp::WIDTH * Disp::HEIGHT; i++) {
        write_data(color_bytes, 2);
    }
    end_write();
}

void Display::draw_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (x < 0 || y < 0 || x + w > Disp::WIDTH || y + h > Disp::HEIGHT) {
        return;
    }

    set_addr_window(x, y, x + w - 1, y + h - 1);

    begin_write();
    write_command(Command::RAMWR);

    uint8_t color_bytes[2];
    color_bytes[0] = (color >> 8) & 0xFF;
    color_bytes[1] = color & 0xFF;

    for (int32_t i = 0; i < w * h; i++) {
        write_data(color_bytes, 2);
    }
    end_write();
}

void Display::lvgl_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;

    set_addr_window(area->x1, area->y1, area->x2, area->y2);

    begin_write();
    write_command(Command::RAMWR);

    // Send pixel data
    // lv_color_t is RGB565, which matches ST7789V format
    size_t len = w * h * sizeof(lv_color_t);
    write_data((const uint8_t*)color_p, len);

    end_write();

    // Tell LVGL we're done flushing
    lv_disp_flush_ready(drv);
}

void Display::write_command(uint8_t cmd) {
    digitalWrite(Pin::DISPLAY_DC, LOW);   // Command mode
    digitalWrite(Pin::DISPLAY_CS, LOW);
    _spi->transfer(cmd);
    digitalWrite(Pin::DISPLAY_CS, HIGH);
}

void Display::write_data(uint8_t data) {
    digitalWrite(Pin::DISPLAY_DC, HIGH);  // Data mode
    digitalWrite(Pin::DISPLAY_CS, LOW);
    _spi->transfer(data);
    digitalWrite(Pin::DISPLAY_CS, HIGH);
}

void Display::write_data(const uint8_t* data, size_t len) {
    digitalWrite(Pin::DISPLAY_DC, HIGH);  // Data mode
    digitalWrite(Pin::DISPLAY_CS, LOW);
    _spi->transferBytes(data, nullptr, len);
    digitalWrite(Pin::DISPLAY_CS, HIGH);
}

void Display::set_addr_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    write_command(Command::CASET);  // Column address set
    write_data(x0 >> 8);
    write_data(x0 & 0xFF);
    write_data(x1 >> 8);
    write_data(x1 & 0xFF);

    write_command(Command::RASET);  // Row address set
    write_data(y0 >> 8);
    write_data(y0 & 0xFF);
    write_data(y1 >> 8);
    write_data(y1 & 0xFF);
}

void Display::begin_write() {
    _spi->beginTransaction(SPISettings(Disp::SPI_FREQUENCY, MSBFIRST, SPI_MODE0));
}

void Display::end_write() {
    _spi->endTransaction();
}

} // namespace TDeck
} // namespace Hardware

#endif // ARDUINO
