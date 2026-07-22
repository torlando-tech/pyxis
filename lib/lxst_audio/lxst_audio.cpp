// Copyright (c) 2024 LXST contributors
// SPDX-License-Identifier: MPL-2.0

#include "lxst_audio.h"

#ifdef ARDUINO
#include <esp_log.h>
#include <esp_system.h>
#include <Hardware/TDeck/Config.h>
#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>

#include "es7210.h"
#include "i2s_capture.h"
#include "i2s_playback.h"
#include "codec_wrapper.h"
#include "Tone.h"

using namespace Hardware::TDeck;

static const char* TAG = "LXST:Audio";
extern "C" void pyxis_log(const char* msg);

static void log_audio_heap(const char* stage) {
    char buf[112];
    snprintf(buf, sizeof(buf), "[AUDIO] %s internal=%u largest=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    pyxis_log(buf);
}


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
        cfg.i2s_iface.samples = AUDIO_HAL_16K_SAMPLES;  // EXACT-LilyGO test: 16kHz (es7210 256Fs); i2s_capture decimates 2:1 to Codec2's 8kHz
        cfg.i2s_iface.bits = AUDIO_HAL_BIT_LENGTH_16BITS;

        uint32_t ret_val = ESP_OK;
        ret_val |= es7210_adc_init(&Wire, &cfg);
        ret_val |= es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
        // ROOT-CAUSE FIX: was (es7210_gain_value_t)micGain (default 7 = 21dB), which
        // SATURATED the ES7210 ADC to the negative rail (0x8000 spikes) + a huge noise
        // floor (rms 7003 in silence) + spectral-peak shift that LOOKED like a +17-25%
        // pitch warp. At 0dB the capture is pristine (tones land at ratio 1.000, conc 0.90)
        // but too quiet. 12dB = a clean middle: real signal level, well below saturation.
        ret_val |= es7210_adc_set_gain(
            (es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2 |
                                  ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4),
            GAIN_12DB);
        ret_val |= es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);

        if (ret_val != ESP_OK) {
            Serial.printf("[AUDIO] ES7210 init warning: ret=%lu\n", (unsigned long)ret_val);
        }
    }
    // Verify ES7210 configuration by reading back key registers
    {
        int reg00 = es7210_read_reg(ES7210_RESET_REG00);
        int reg01 = es7210_read_reg(ES7210_CLOCK_OFF_REG01);
        int reg06 = es7210_read_reg(ES7210_POWER_DOWN_REG06);
        int reg08 = es7210_read_reg(ES7210_MODE_CONFIG_REG08);
        int reg43 = es7210_read_reg(ES7210_MIC1_GAIN_REG43);
        int reg47 = es7210_read_reg(ES7210_MIC1_POWER_REG47);
        int reg4b = es7210_read_reg(ES7210_MIC12_POWER_REG4B);
        Serial.printf("[AUDIO] ES7210 regs: R00=0x%02X R01=0x%02X R06=0x%02X R08=0x%02X "
                      "GAIN1=0x%02X PWR1=0x%02X PWR12=0x%02X\n",
                      reg00, reg01, reg06, reg08, reg43, reg47, reg4b);
        // Expected: R00=0x41 (normal), R06=0x00 (powered up), R08=0x00 (slave mode)
        // PWR1=0x00, PWR12=0x00 (mics powered on)
        if (reg06 != 0x00) {
            Serial.printf("[AUDIO] WARNING: ES7210 POWER_DOWN=0x%02X (expected 0x00)\n", reg06);
        }
        if (reg47 != 0x00 || reg4b != 0x00) {
            Serial.printf("[AUDIO] WARNING: ES7210 mic power not active! R47=0x%02X R4B=0x%02X\n", reg47, reg4b);
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

    // Re-issue ES7210 start with clocks now running.
    // In slave mode, the ES7210 needs MCLK/BCLK/LRCK from the ESP32 I2S master
    // to properly start its ADC — the initial start above ran before clocks were
    // available.  This second call ensures the ADC powers up correctly.
    {
        audio_hal_codec_config_t cfg2 = {};
        cfg2.codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE;
        es7210_adc_ctrl_state(cfg2.codec_mode, AUDIO_HAL_CTRL_START);
        Serial.println("[AUDIO] ES7210 re-started with I2S clocks active");
    }

    // Keep Codec2 in internal RAM: its tight DSP loops become watchdog-prone
    // when the state is placed in PSRAM. One shared state is sufficient for
    // full duplex because Codec2Wrapper serializes encode/decode with a mutex,
    // and LXST negotiates one profile for both directions.
    encodeCodec_ = new Codec2Wrapper();
    if (!encodeCodec_ || !encodeCodec_->create(codec2Mode)) {
        Serial.println("[AUDIO] Codec2 create FAILED");
        delete capture_;
        capture_ = nullptr;
        delete encodeCodec_;
        encodeCodec_ = nullptr;
        return false;
    }
    decodeCodec_ = encodeCodec_;
    Serial.printf("[AUDIO] Shared Codec2 created (heap=%lu)\n", (unsigned long)esp_get_free_heap_size());

    if (!capture_->configureEncoder(encodeCodec_, true)) {
        Serial.println("[AUDIO] Capture encoder config FAILED");
        delete capture_;
        capture_ = nullptr;
        encodeCodec_->destroy();
        delete encodeCodec_;
        encodeCodec_ = nullptr;
        decodeCodec_ = nullptr;
        return false;
    }

    playback_ = new I2SPlayback();
    if (!playback_ || !playback_->configureDecoder(decodeCodec_)) {
        ESP_LOGE(TAG, "Playback decoder config failed");
        delete capture_;
        capture_ = nullptr;
        delete playback_;
        playback_ = nullptr;
        encodeCodec_->destroy();
        delete encodeCodec_;
        encodeCodec_ = nullptr;
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

    // encodeCodec_ and decodeCodec_ intentionally alias one mutex-protected
    // Codec2 state; destroy it exactly once.
    decodeCodec_ = nullptr;
    if (encodeCodec_) {
        encodeCodec_->destroy();
        delete encodeCodec_;
        encodeCodec_ = nullptr;
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
    log_audio_heap("after tone release");

    // Start capture first. Its larger contiguous stack must be allocated before
    // playback fragments the remaining internal heap. Codec2 encoding itself is
    // deferred to loopTask, so the capture task only needs 8 KiB.
    if (!capture_->isCapturing()) {
        ESP_LOGI(TAG, "Starting capture (heap=%lu)...", (unsigned long)esp_get_free_heap_size());
        if (!capture_->start()) {
            ESP_LOGE(TAG, "Failed to start capture for full-duplex");
            log_audio_heap("capture start FAILED");
            return false;
        }
        log_audio_heap("capture started");
        ESP_LOGI(TAG, "Capture started (heap=%lu)", (unsigned long)esp_get_free_heap_size());
    }

    // Start playback (speaker) second — I2S_NUM_0.
    if (!playback_->isPlaying()) {
        ESP_LOGI(TAG, "Starting playback (heap=%lu)...", (unsigned long)esp_get_free_heap_size());
        if (!playback_->start()) {
            ESP_LOGE(TAG, "Failed to start playback for full-duplex");
            log_audio_heap("playback start FAILED");
            capture_->stop();
            capture_->init();
            capture_->configureEncoder(encodeCodec_, true);
            return false;
        }
        log_audio_heap("playback started");
        ESP_LOGI(TAG, "Playback started (heap=%lu)", (unsigned long)esp_get_free_heap_size());
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

uint32_t LXSTAudio::playbackDecodeOk() const {
    if (!playback_) return 0;
    return playback_->decodeOkCount();
}

uint32_t LXSTAudio::playbackDecodeFail() const {
    if (!playback_) return 0;
    return playback_->decodeFailCount();
}

void LXSTAudio::playbackResetCounters() {
    if (playback_) playback_->resetCounters();
}

uint32_t LXSTAudio::playbackPcmSampleCount() const {
    if (!playback_) return 0;
    return playback_->pcmSampleCount();
}

uint64_t LXSTAudio::playbackPcmSumSquares() const {
    if (!playback_) return 0;
    return playback_->pcmSumSquares();
}

void LXSTAudio::captureSetInjectSine(bool enabled, int freq, float amp) {
    if (capture_) capture_->setInjectSine(enabled, freq, amp);
}

#endif // ARDUINO
