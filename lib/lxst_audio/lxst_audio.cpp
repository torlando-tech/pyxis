// Copyright (c) 2024 LXST contributors
// SPDX-License-Identifier: MPL-2.0

#include "lxst_audio.h"

#ifdef ARDUINO
#include <esp_log.h>
#include <Hardware/TDeck/Config.h>
#include "es7210.h"
#include "i2s_capture.h"
#include "i2s_playback.h"
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

    // Initialize ES7210 mic array via I2C
    ESP_LOGI(TAG, "Initializing ES7210 mic array...");
    if (!ES7210::init(I2C::ES7210_ADDR, static_cast<ES7210::MicGain>(micGain))) {
        ESP_LOGE(TAG, "ES7210 init failed");
        return false;
    }

    // Create capture engine
    capture_ = new I2SCapture();
    if (!capture_->init()) {
        ESP_LOGE(TAG, "I2S capture init failed");
        delete capture_;
        capture_ = nullptr;
        return false;
    }

    // Configure encoder on capture side
    if (!capture_->configureEncoder(codec2Mode, true)) {
        ESP_LOGE(TAG, "Capture encoder config failed");
        delete capture_;
        capture_ = nullptr;
        return false;
    }

    // Create playback engine (doesn't init I2S yet — deferred to start())
    playback_ = new I2SPlayback();
    if (!playback_->configureDecoder(codec2Mode)) {
        ESP_LOGE(TAG, "Playback decoder config failed");
        delete capture_;
        capture_ = nullptr;
        delete playback_;
        playback_ = nullptr;
        return false;
    }

    initialized_ = true;
    state_ = State::IDLE;

    ESP_LOGI(TAG, "LXST Audio initialized: Codec2 mode %d", codec2Mode);
    return true;
}

void LXSTAudio::deinit() {
    stopCapture();
    stopPlayback();

    if (capture_) {
        capture_->destroyEncoder();
        delete capture_;
        capture_ = nullptr;
    }

    if (playback_) {
        playback_->destroyDecoder();
        delete playback_;
        playback_ = nullptr;
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

    if (state_ == State::CAPTURING) return true;  // Already capturing

    if (!capture_->start()) {
        ESP_LOGE(TAG, "Failed to start capture");
        return false;
    }

    state_ = State::CAPTURING;
    ESP_LOGI(TAG, "Capture started (TX mode)");
    return true;
}

void LXSTAudio::stopCapture() {
    if (!capture_ || state_ != State::CAPTURING) return;

    capture_->stop();

    // Re-init capture I2S for next use
    capture_->init();
    capture_->configureEncoder(codec2Mode_, true);

    state_ = State::IDLE;
    ESP_LOGI(TAG, "Capture stopped");
}

bool LXSTAudio::startPlayback() {
    if (!initialized_ || !playback_) return false;

    // Half-duplex: stop capture first
    if (state_ == State::CAPTURING) {
        stopCapture();
    }

    if (state_ == State::PLAYING) return true;  // Already playing

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
    if (!playback_ || state_ != State::PLAYING) return;

    playback_->stop();

    // Re-configure decoder for next use
    playback_->configureDecoder(codec2Mode_);

    state_ = State::IDLE;
    ESP_LOGI(TAG, "Playback stopped");

    // Tone.cpp will re-init its I2S on next tone_play() call
    // (tone_init() checks _initialized flag)
}

bool LXSTAudio::readEncodedPacket(uint8_t* dest, int maxLength, int* actualLength) {
    if (!capture_ || state_ != State::CAPTURING) return false;
    return capture_->readEncodedPacket(dest, maxLength, actualLength);
}

bool LXSTAudio::writeEncodedPacket(const uint8_t* data, int length) {
    if (!playback_ || state_ != State::PLAYING) return false;
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
