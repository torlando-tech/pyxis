// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

/**
 * Simple I2S tone generator for T-Deck Plus speaker
 *
 * Based on LilyGo SimpleTone example.
 * Uses native ESP32 I2S driver - no external libraries needed.
 */

namespace Notification {

/**
 * Initialize the I2S audio driver
 * Must be called once at startup before using tone_play()
 */
void tone_init();

/**
 * Play a tone at the specified frequency
 * @param frequency Frequency in Hz (e.g., 1000 for 1kHz)
 * @param duration_ms Duration in milliseconds
 * @param volume Volume level 0-100 (default 50)
 */
void tone_play(uint16_t frequency, uint16_t duration_ms, uint8_t volume = 50);

/**
 * Stop any currently playing tone
 */
void tone_stop();

/**
 * Check if a tone is currently playing
 * @return true if playing, false if silent
 */
bool tone_is_playing();

} // namespace Notification
