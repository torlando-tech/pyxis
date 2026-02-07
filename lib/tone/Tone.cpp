// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "Tone.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <driver/i2s.h>
#include <Hardware/TDeck/Config.h>

using namespace Hardware::TDeck;

namespace Notification {

// I2S configuration
static const uint32_t SAMPLE_RATE = 8000;
static const i2s_port_t I2S_PORT = I2S_NUM_0;

// Tone state
static bool _initialized = false;
static bool _playing = false;
static uint32_t _tone_end_time = 0;
static uint16_t _current_freq = 0;
static uint16_t _current_amplitude = 0;
static uint32_t _sample_counter = 0;

void tone_init() {
    if (_initialized) return;

    // Configure I2S
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[TONE] Failed to install I2S driver: %d\n", err);
        return;
    }

    // Configure I2S pins
    i2s_pin_config_t pin_config = {
        .bck_io_num = Audio::I2S_BCK,
        .ws_io_num = Audio::I2S_WS,
        .data_out_num = Audio::I2S_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[TONE] Failed to set I2S pins: %d\n", err);
        return;
    }

    _initialized = true;
    Serial.println("[TONE] Audio initialized");
}

void tone_play(uint16_t frequency, uint16_t duration_ms, uint8_t volume) {
    if (!_initialized) {
        tone_init();
    }

    // Calculate amplitude from volume (0-100 -> 0-32767)
    _current_amplitude = (volume * 327);  // Max ~32700
    _current_freq = frequency;
    _sample_counter = 0;
    _tone_end_time = millis() + duration_ms;
    _playing = true;

    // Generate and play the tone
    const uint32_t samples_per_cycle = SAMPLE_RATE / frequency;
    const uint32_t total_samples = (SAMPLE_RATE * duration_ms) / 1000;

    int16_t samples[128];
    size_t bytes_written;
    uint32_t samples_written = 0;

    while (samples_written < total_samples) {
        for (int i = 0; i < 128 && samples_written < total_samples; i++) {
            // Generate square wave
            if ((_sample_counter % samples_per_cycle) < (samples_per_cycle / 2)) {
                samples[i] = _current_amplitude;
            } else {
                samples[i] = -_current_amplitude;
            }
            _sample_counter++;
            samples_written++;
        }
        esp_err_t err = i2s_write(I2S_PORT, samples, sizeof(samples), &bytes_written, pdMS_TO_TICKS(2000));
        if (err != ESP_OK) {
            Serial.printf("[TONE] I2S write timeout/error: %d\n", err);
            break;  // Abort tone on error
        }
    }

    // Write silence to flush DMA buffers
    int16_t silence[128] = {0};
    // Silence flush - timeout OK to ignore here
    i2s_write(I2S_PORT, silence, sizeof(silence), &bytes_written, pdMS_TO_TICKS(2000));

    _playing = false;
}

void tone_stop() {
    if (!_initialized) return;

    _playing = false;

    // Write silence
    int16_t silence[128] = {0};
    size_t bytes_written;
    // Silence on stop - timeout OK to ignore
    i2s_write(I2S_PORT, silence, sizeof(silence), &bytes_written, pdMS_TO_TICKS(2000));
}

bool tone_is_playing() {
    return _playing;
}

} // namespace Notification

#endif // ARDUINO
