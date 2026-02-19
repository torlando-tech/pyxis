// Copyright (c) 2024 LXST contributors
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>

/**
 * ES7210 four-channel audio ADC I2C driver for T-Deck Plus.
 *
 * Adapted from ESPHome es7210 component and Espressif ESP-BSP driver.
 * Handles only I2C configuration registers — audio data flows over I2S.
 *
 * After init(), the ES7210 outputs 8kHz 16-bit audio on its I2S SDOUT1
 * pin (mics 1&2). The caller is responsible for I2S port setup.
 */
namespace ES7210 {

// ES7210 register addresses
static constexpr uint8_t REG_RESET         = 0x00;
static constexpr uint8_t REG_CLOCK_OFF     = 0x01;
static constexpr uint8_t REG_MAINCLK       = 0x02;
static constexpr uint8_t REG_MASTER_CLK    = 0x03;
static constexpr uint8_t REG_LRCK_DIVH     = 0x04;
static constexpr uint8_t REG_LRCK_DIVL     = 0x05;
static constexpr uint8_t REG_POWER_DOWN    = 0x06;
static constexpr uint8_t REG_OSR           = 0x07;
static constexpr uint8_t REG_MODE_CONFIG   = 0x08;
static constexpr uint8_t REG_TIME_CTRL0    = 0x09;
static constexpr uint8_t REG_TIME_CTRL1    = 0x0A;
static constexpr uint8_t REG_SDP_IFACE1    = 0x11;
static constexpr uint8_t REG_SDP_IFACE2    = 0x12;
static constexpr uint8_t REG_ADC_AUTOMUTE  = 0x13;
static constexpr uint8_t REG_ADC34_HPF2    = 0x20;
static constexpr uint8_t REG_ADC34_HPF1    = 0x21;
static constexpr uint8_t REG_ADC12_HPF1    = 0x22;
static constexpr uint8_t REG_ADC12_HPF2    = 0x23;
static constexpr uint8_t REG_ANALOG        = 0x40;
static constexpr uint8_t REG_MIC12_BIAS    = 0x41;
static constexpr uint8_t REG_MIC34_BIAS    = 0x42;
static constexpr uint8_t REG_MIC1_GAIN     = 0x43;
static constexpr uint8_t REG_MIC2_GAIN     = 0x44;
static constexpr uint8_t REG_MIC3_GAIN     = 0x45;
static constexpr uint8_t REG_MIC4_GAIN     = 0x46;
static constexpr uint8_t REG_MIC1_POWER    = 0x47;
static constexpr uint8_t REG_MIC2_POWER    = 0x48;
static constexpr uint8_t REG_MIC3_POWER    = 0x49;
static constexpr uint8_t REG_MIC4_POWER    = 0x4A;
static constexpr uint8_t REG_MIC12_POWER   = 0x4B;
static constexpr uint8_t REG_MIC34_POWER   = 0x4C;

// Mic gain in dB (0 to 37.5 in 3dB steps)
enum MicGain : uint8_t {
    GAIN_0DB   = 0,
    GAIN_3DB   = 1,
    GAIN_6DB   = 2,
    GAIN_9DB   = 3,
    GAIN_12DB  = 4,
    GAIN_15DB  = 5,
    GAIN_18DB  = 6,
    GAIN_21DB  = 7,
    GAIN_24DB  = 8,
    GAIN_27DB  = 9,
    GAIN_30DB  = 10,
    GAIN_33DB  = 11,
    GAIN_34_5DB = 12,
    GAIN_36DB  = 13,
    GAIN_37_5DB = 14,
};

/**
 * Initialize the ES7210 for 8kHz 16-bit I2S capture.
 *
 * Uses the shared Wire bus (must already be initialized).
 * Configures mic1 channel with specified gain.
 *
 * @param i2cAddr  I2C address (default 0x40)
 * @param gain     Microphone gain setting (default 24dB)
 * @return true if all I2C writes succeeded
 */
bool init(uint8_t i2cAddr = 0x40, MicGain gain = GAIN_24DB);

/**
 * Set microphone gain for all channels.
 * Can be called after init() to adjust gain dynamically.
 *
 * @param i2cAddr  I2C address
 * @param gain     New gain setting
 * @return true on success
 */
bool setGain(uint8_t i2cAddr, MicGain gain);

} // namespace ES7210
