// Copyright (c) 2024 LXST contributors
// SPDX-License-Identifier: MPL-2.0

#include "es7210.h"

#ifdef ARDUINO
#include <Wire.h>
#include <esp_log.h>

static const char* TAG = "ES7210";

// Clock coefficient table for 8kHz with MCLK = 4.096MHz (256 * 16kHz)
// From ESPHome/Espressif ES7210 driver
// Fields: mclk, lrclk, ss_ds, adc_div, dll, doubler, osr, mclk_src, lrck_h, lrck_l
struct ClockCoeff {
    uint32_t mclk;
    uint32_t lrclk;
    uint8_t adc_div;
    uint8_t dll;
    uint8_t doubler;
    uint8_t osr;
    uint8_t lrck_h;
    uint8_t lrck_l;
};

// 8kHz with MCLK = 4.096MHz = 512 * 8kHz
static constexpr ClockCoeff CLOCK_8K = {
    .mclk = 4096000, .lrclk = 8000,
    .adc_div = 0x01, .dll = 0x01, .doubler = 0x00,
    .osr = 0x20, .lrck_h = 0x02, .lrck_l = 0x00
};

static bool writeReg(uint8_t addr, uint8_t reg, uint8_t val) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    uint8_t err = Wire.endTransmission();
    if (err != 0) {
        ESP_LOGE(TAG, "I2C write failed: reg=0x%02x val=0x%02x err=%d", reg, val, err);
        return false;
    }
    return true;
}

static bool readReg(uint8_t addr, uint8_t reg, uint8_t* val) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(addr, (uint8_t)1) != 1) return false;
    *val = Wire.read();
    return true;
}

static bool updateRegBit(uint8_t addr, uint8_t reg, uint8_t mask, uint8_t data) {
    uint8_t regv;
    if (!readReg(addr, reg, &regv)) return false;
    regv = (regv & (~mask)) | (mask & data);
    return writeReg(addr, reg, regv);
}

namespace ES7210 {

bool init(uint8_t i2cAddr, MicGain gain) {
    ESP_LOGI(TAG, "Initializing ES7210 at 0x%02x, gain=%d", i2cAddr, gain);

    // Software reset
    if (!writeReg(i2cAddr, REG_RESET, 0xFF)) return false;
    if (!writeReg(i2cAddr, REG_RESET, 0x32)) return false;
    if (!writeReg(i2cAddr, REG_CLOCK_OFF, 0x3F)) return false;

    // Set initialization and power-up timing
    if (!writeReg(i2cAddr, REG_TIME_CTRL0, 0x30)) return false;
    if (!writeReg(i2cAddr, REG_TIME_CTRL1, 0x30)) return false;

    // Configure high-pass filters for all ADC channels
    if (!writeReg(i2cAddr, REG_ADC12_HPF2, 0x2A)) return false;
    if (!writeReg(i2cAddr, REG_ADC12_HPF1, 0x0A)) return false;
    if (!writeReg(i2cAddr, REG_ADC34_HPF2, 0x0A)) return false;
    if (!writeReg(i2cAddr, REG_ADC34_HPF1, 0x2A)) return false;

    // Secondary (slave) I2S mode — ESP32 provides clocks
    if (!updateRegBit(i2cAddr, REG_MODE_CONFIG, 0x01, 0x00)) return false;

    // Configure analog power
    if (!writeReg(i2cAddr, REG_ANALOG, 0xC3)) return false;

    // Set mic bias to 2.87V (0x70)
    if (!writeReg(i2cAddr, REG_MIC12_BIAS, 0x70)) return false;
    if (!writeReg(i2cAddr, REG_MIC34_BIAS, 0x70)) return false;

    // Configure I2S format: 16-bit, standard I2S
    // Bits per sample: 0x60 = 16-bit
    if (!writeReg(i2cAddr, REG_SDP_IFACE1, 0x60)) return false;
    // Normal mode (not TDM): mic1&2 on SDOUT1, mic3&4 on SDOUT2
    if (!writeReg(i2cAddr, REG_SDP_IFACE2, 0x00)) return false;

    // Configure clock for 8kHz with MCLK = 4.096MHz
    {
        uint8_t regv = CLOCK_8K.adc_div
                     | (CLOCK_8K.doubler << 6)
                     | (CLOCK_8K.dll << 7);
        if (!writeReg(i2cAddr, REG_MAINCLK, regv)) return false;
        if (!writeReg(i2cAddr, REG_OSR, CLOCK_8K.osr)) return false;
        if (!writeReg(i2cAddr, REG_LRCK_DIVH, CLOCK_8K.lrck_h)) return false;
        if (!writeReg(i2cAddr, REG_LRCK_DIVL, CLOCK_8K.lrck_l)) return false;
    }

    // Power on mic channels
    if (!writeReg(i2cAddr, REG_MIC1_POWER, 0x08)) return false;
    if (!writeReg(i2cAddr, REG_MIC2_POWER, 0x08)) return false;
    if (!writeReg(i2cAddr, REG_MIC3_POWER, 0x08)) return false;
    if (!writeReg(i2cAddr, REG_MIC4_POWER, 0x08)) return false;

    // Power down DLL
    if (!writeReg(i2cAddr, REG_POWER_DOWN, 0x04)) return false;

    // Power on MIC bias & ADC & PGA
    if (!writeReg(i2cAddr, REG_MIC12_POWER, 0x0F)) return false;
    if (!writeReg(i2cAddr, REG_MIC34_POWER, 0x0F)) return false;

    // Set mic gain
    if (!setGain(i2cAddr, gain)) return false;

    // Enable device
    if (!writeReg(i2cAddr, REG_RESET, 0x71)) return false;
    if (!writeReg(i2cAddr, REG_RESET, 0x41)) return false;

    ESP_LOGI(TAG, "ES7210 initialized successfully");
    return true;
}

bool setGain(uint8_t i2cAddr, MicGain gain) {
    uint8_t regv = static_cast<uint8_t>(gain);

    // Clear PGA gain for all mics
    for (uint8_t i = 0; i < 4; ++i) {
        if (!updateRegBit(i2cAddr, REG_MIC1_GAIN + i, 0x10, 0x00)) return false;
    }

    // Disable ADC power temporarily
    if (!writeReg(i2cAddr, REG_MIC12_POWER, 0xFF)) return false;
    if (!writeReg(i2cAddr, REG_MIC34_POWER, 0xFF)) return false;

    // Configure each mic gain
    for (uint8_t i = 0; i < 4; ++i) {
        if (!updateRegBit(i2cAddr, REG_CLOCK_OFF, 0x0B, 0x00)) return false;
        if (i < 2) {
            if (!writeReg(i2cAddr, REG_MIC12_POWER, 0x00)) return false;
        } else {
            if (!writeReg(i2cAddr, REG_MIC34_POWER, 0x00)) return false;
        }
        if (!updateRegBit(i2cAddr, REG_MIC1_GAIN + i, 0x10, 0x10)) return false;
        if (!updateRegBit(i2cAddr, REG_MIC1_GAIN + i, 0x0F, regv)) return false;
    }

    ESP_LOGI(TAG, "Mic gain set to %d", gain);
    return true;
}

} // namespace ES7210

#endif // ARDUINO
