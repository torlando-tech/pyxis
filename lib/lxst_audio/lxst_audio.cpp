// Copyright (c) 2024 LXST contributors
// SPDX-License-Identifier: MPL-2.0

#include "lxst_audio.h"

#ifdef ARDUINO
#include <esp_log.h>
#include <esp_system.h>
#include <Hardware/TDeck/Config.h>
#include <Arduino.h>
#include <driver/i2s.h>
#include "es7210.h"
#include "i2s_capture.h"
#include "i2s_playback.h"
#include "codec_wrapper.h"
#include "Tone.h"

using namespace Hardware::TDeck;

static const char* TAG = "LXST:Audio";

LXSTAudio::LXSTAudio() = default;

LXSTAudio::~LXSTAudio() {
    deinit();
}

bool LXSTAudio::init(int codec2Mode, uint8_t micGain) {
    if (initialized_) {
        ESP_LOGW(TAG, "Already initialized");
        return true;
    }

    codec2Mode_ = codec2Mode;

    Serial.printf("[AUDIO] Init starting (heap=%lu)...\n", (unsigned long)esp_get_free_heap_size());

    // Initialize ES7210 using LilyGO library — exact same calls as their Microphone example
    // SLAVE mode: ES7210 receives MCLK/BCLK/LRCK from ESP32 I2S master
    Serial.println("[AUDIO] Initializing ES7210 via LilyGO library...");
    {
        audio_hal_codec_config_t cfg = {};
        cfg.adc_input = AUDIO_HAL_ADC_INPUT_ALL;
        cfg.codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE;
        cfg.i2s_iface.mode = AUDIO_HAL_MODE_SLAVE;
        cfg.i2s_iface.fmt = AUDIO_HAL_I2S_NORMAL;
        cfg.i2s_iface.samples = AUDIO_HAL_16K_SAMPLES;
        cfg.i2s_iface.bits = AUDIO_HAL_BIT_LENGTH_16BITS;

        uint32_t ret_val = ESP_OK;
        ret_val |= es7210_adc_init(&Wire, &cfg);
        ret_val |= es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
        ret_val |= es7210_adc_set_gain(
            (es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2 |
                                  ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4),
            (es7210_gain_value_t)micGain);
        ret_val |= es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);

        if (ret_val != ESP_OK) {
            Serial.printf("[AUDIO] ES7210 init warning: ret=%lu\n", (unsigned long)ret_val);
        }
    }
    Serial.println("[AUDIO] ES7210 initialized OK");

    // I2S capture init
    capture_ = new I2SCapture();
    if (!capture_->init()) {
        Serial.println("[AUDIO] I2S capture init FAILED");
        delete capture_;
        capture_ = nullptr;
        return false;
    }
    Serial.println("[AUDIO] I2S capture initialized (MCLK now running)");

    // Create separate Codec2 instances for encode and decode to avoid mutex
    // contention during full-duplex calls (capture task + main thread decode)
    encodeCodec_ = new Codec2Wrapper();
    if (!encodeCodec_->create(codec2Mode)) {
        Serial.println("[AUDIO] Codec2 encoder create FAILED");
        delete capture_;
        capture_ = nullptr;
        delete encodeCodec_;
        encodeCodec_ = nullptr;
        return false;
    }
    Serial.printf("[AUDIO] Codec2 encoder created (heap=%lu)\n", (unsigned long)esp_get_free_heap_size());

    decodeCodec_ = new Codec2Wrapper();
    if (!decodeCodec_->create(codec2Mode)) {
        Serial.println("[AUDIO] Codec2 decoder create FAILED");
        delete capture_;
        capture_ = nullptr;
        encodeCodec_->destroy();
        delete encodeCodec_;
        encodeCodec_ = nullptr;
        delete decodeCodec_;
        decodeCodec_ = nullptr;
        return false;
    }
    Serial.printf("[AUDIO] Codec2 decoder created (heap=%lu)\n", (unsigned long)esp_get_free_heap_size());

    // Configure encoder on capture side (owns its codec instance)
    if (!capture_->configureEncoder(encodeCodec_, true)) {
        Serial.println("[AUDIO] Capture encoder config FAILED");
        delete capture_;
        capture_ = nullptr;
        encodeCodec_->destroy();
        delete encodeCodec_;
        encodeCodec_ = nullptr;
        decodeCodec_->destroy();
        delete decodeCodec_;
        decodeCodec_ = nullptr;
        return false;
    }

    // Create playback engine with its own codec instance
    playback_ = new I2SPlayback();
    if (!playback_->configureDecoder(decodeCodec_)) {
        ESP_LOGE(TAG, "Playback decoder config failed");
        delete capture_;
        capture_ = nullptr;
        delete playback_;
        playback_ = nullptr;
        encodeCodec_->destroy();
        delete encodeCodec_;
        encodeCodec_ = nullptr;
        decodeCodec_->destroy();
        delete decodeCodec_;
        decodeCodec_ = nullptr;
        return false;
    }

    initialized_ = true;
    state_ = State::IDLE;

    ESP_LOGI(TAG, "LXST Audio initialized: Codec2 mode %d (heap=%lu)",
             codec2Mode, (unsigned long)esp_get_free_heap_size());
    return true;
}

void LXSTAudio::deinit() {
    stopCapture();
    stopPlayback();

    if (capture_) {
        capture_->releaseBuffers();
        delete capture_;
        capture_ = nullptr;
    }

    if (playback_) {
        playback_->releaseBuffers();
        delete playback_;
        playback_ = nullptr;
    }

    if (encodeCodec_) {
        encodeCodec_->destroy();
        delete encodeCodec_;
        encodeCodec_ = nullptr;
    }

    if (decodeCodec_) {
        decodeCodec_->destroy();
        delete decodeCodec_;
        decodeCodec_ = nullptr;
    }

    initialized_ = false;
    state_ = State::IDLE;

    ESP_LOGI(TAG, "LXST Audio deinitialized");
}

bool LXSTAudio::startCapture() {
    if (!initialized_ || !capture_) return false;

    // Half-duplex: stop playback first
    if (state_ == State::PLAYING) {
        stopPlayback();
    }

    if (state_ == State::CAPTURING || state_ == State::FULL_DUPLEX) return true;

    if (!capture_->start()) {
        ESP_LOGE(TAG, "Failed to start capture");
        return false;
    }

    state_ = State::CAPTURING;
    ESP_LOGI(TAG, "Capture started (TX mode)");
    return true;
}

void LXSTAudio::stopCapture() {
    if (!capture_) return;
    if (state_ != State::CAPTURING && state_ != State::FULL_DUPLEX) return;

    capture_->stop();

    // Re-init capture I2S for next use
    capture_->init();
    capture_->configureEncoder(encodeCodec_, true);

    if (state_ == State::FULL_DUPLEX) {
        state_ = State::PLAYING;
        ESP_LOGI(TAG, "Capture stopped (still playing)");
    } else {
        state_ = State::IDLE;
        ESP_LOGI(TAG, "Capture stopped");
    }
}

bool LXSTAudio::startPlayback() {
    if (!initialized_ || !playback_) return false;

    // Half-duplex: stop capture first
    if (state_ == State::CAPTURING) {
        stopCapture();
    }

    if (state_ == State::PLAYING || state_ == State::FULL_DUPLEX) return true;

    // Release I2S_NUM_0 from tone generator
    Notification::tone_deinit();

    if (!playback_->start()) {
        ESP_LOGE(TAG, "Failed to start playback");
        return false;
    }

    state_ = State::PLAYING;
    ESP_LOGI(TAG, "Playback started (RX mode)");
    return true;
}

void LXSTAudio::stopPlayback() {
    if (!playback_) return;
    if (state_ != State::PLAYING && state_ != State::FULL_DUPLEX) return;

    playback_->stop();

    // Re-configure decoder for next use
    playback_->configureDecoder(decodeCodec_);

    if (state_ == State::FULL_DUPLEX) {
        state_ = State::CAPTURING;
        ESP_LOGI(TAG, "Playback stopped (still capturing)");
    } else {
        state_ = State::IDLE;
        ESP_LOGI(TAG, "Playback stopped");
    }

    // Tone.cpp will re-init its I2S on next tone_play() call
    // (tone_init() checks _initialized flag)
}

bool LXSTAudio::startFullDuplex() {
    if (!initialized_ || !capture_ || !playback_) return false;

    if (state_ == State::FULL_DUPLEX) return true;

    ESP_LOGI(TAG, "Starting full-duplex (heap=%lu)...", (unsigned long)esp_get_free_heap_size());

    // Release I2S_NUM_0 from tone generator
    Notification::tone_deinit();

    // Start playback (speaker) first — I2S_NUM_0
    if (!playback_->isPlaying()) {
        ESP_LOGI(TAG, "Starting playback (heap=%lu)...", (unsigned long)esp_get_free_heap_size());
        if (!playback_->start()) {
            ESP_LOGE(TAG, "Failed to start playback for full-duplex");
            return false;
        }
        ESP_LOGI(TAG, "Playback started (heap=%lu)", (unsigned long)esp_get_free_heap_size());
    }

    // Start capture (mic) — I2S_NUM_1
    if (!capture_->isCapturing()) {
        ESP_LOGI(TAG, "Starting capture (heap=%lu)...", (unsigned long)esp_get_free_heap_size());
        if (!capture_->start()) {
            ESP_LOGE(TAG, "Failed to start capture for full-duplex");
            playback_->stop();
            playback_->configureDecoder(decodeCodec_);
            return false;
        }
        ESP_LOGI(TAG, "Capture started (heap=%lu)", (unsigned long)esp_get_free_heap_size());
    }

    state_ = State::FULL_DUPLEX;
    ESP_LOGI(TAG, "Full-duplex started (TX+RX, heap=%lu)", (unsigned long)esp_get_free_heap_size());
    return true;
}

bool LXSTAudio::readEncodedPacket(uint8_t* dest, int maxLength, int* actualLength) {
    if (!capture_ || !isCapturing()) return false;
    return capture_->readEncodedPacket(dest, maxLength, actualLength);
}

bool LXSTAudio::writeEncodedPacket(const uint8_t* data, int length) {
    if (!playback_ || !isPlaying()) return false;
    return playback_->writeEncodedPacket(data, length);
}

void LXSTAudio::setCaptureMute(bool muted) {
    if (capture_) capture_->setMute(muted);
}

void LXSTAudio::setPlaybackMute(bool muted) {
    if (playback_) playback_->setMute(muted);
}

int LXSTAudio::capturePacketsAvailable() const {
    if (!capture_) return 0;
    return capture_->availablePackets();
}

int LXSTAudio::playbackFramesBuffered() const {
    if (!playback_) return 0;
    return playback_->bufferedFrames();
}

#endif // ARDUINO
