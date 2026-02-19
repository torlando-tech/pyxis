// Copyright (c) 2024 LXST contributors
// SPDX-License-Identifier: MPL-2.0

#include "i2s_capture.h"

#ifdef ARDUINO
#include <cstring>
#include <driver/i2s.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <Hardware/TDeck/Config.h>
#include "codec_wrapper.h"
#include "audio_filters.h"
#include "encoded_ring_buffer.h"

using namespace Hardware::TDeck;

static const char* TAG = "LXST:Capture";

I2SCapture::I2SCapture() = default;

I2SCapture::~I2SCapture() {
    stop();
    destroyEncoder();
}

bool I2SCapture::init() {
    if (i2sInitialized_) return true;

    // Configure I2S_NUM_1 for mic capture from ES7210
    i2s_config_t i2s_config = {};
    i2s_config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
    i2s_config.sample_rate = SAMPLE_RATE;
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    // ES7210 in normal mode outputs stereo (mic1=L, mic2=R) on SDOUT1
    // We capture both channels and extract mic1 (left channel only)
    i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_config.dma_buf_count = 4;
    i2s_config.dma_buf_len = 128;
    i2s_config.use_apll = false;
    i2s_config.tx_desc_auto_clear = false;
    i2s_config.fixed_mclk = 4096000;  // 4.096MHz MCLK for ES7210 at 8kHz

    esp_err_t err = i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S_NUM_1 driver install failed: %d", err);
        return false;
    }

    i2s_pin_config_t pin_config = {};
    pin_config.mck_io_num = Audio::MIC_MCLK;
    pin_config.bck_io_num = Audio::MIC_SCK;
    pin_config.ws_io_num = Audio::MIC_LRCK;
    pin_config.data_in_num = Audio::MIC_DIN;
    pin_config.data_out_num = I2S_PIN_NO_CHANGE;

    err = i2s_set_pin(I2S_NUM_1, &pin_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S_NUM_1 pin config failed: %d", err);
        i2s_driver_uninstall(I2S_NUM_1);
        return false;
    }

    i2sInitialized_ = true;
    ESP_LOGI(TAG, "I2S capture initialized: %dHz 16-bit mono, MCLK=4.096MHz", SAMPLE_RATE);
    return true;
}

bool I2SCapture::configureEncoder(int codec2Mode, bool enableFilters) {
    destroyEncoder();

    encoder_ = new Codec2Wrapper();
    if (!encoder_->create(codec2Mode)) {
        ESP_LOGE(TAG, "Failed to create Codec2 encoder mode %d", codec2Mode);
        delete encoder_;
        encoder_ = nullptr;
        return false;
    }

    frameSamples_ = encoder_->samplesPerFrame();
    filtersEnabled_ = enableFilters;

    // Allocate ring buffer in PSRAM
    encodedRing_ = new EncodedRingBuffer(ENCODED_RING_SLOTS, ENCODED_RING_MAX_BYTES);

    // Allocate accumulation buffer in PSRAM
    accumBuffer_ = static_cast<int16_t*>(
        heap_caps_malloc(sizeof(int16_t) * frameSamples_, MALLOC_CAP_SPIRAM));
    accumCount_ = 0;

    // Silence buffer for mute
    silenceBuf_ = static_cast<int16_t*>(
        heap_caps_calloc(frameSamples_, sizeof(int16_t), MALLOC_CAP_SPIRAM));

    // Filter chain: 1 channel (mono), voice band 300-3400Hz, AGC -12dB target, 12dB max
    if (enableFilters) {
        filterChain_ = new VoiceFilterChain(1, 300.0f, 3400.0f, -12.0f, 12.0f);
    }

    ESP_LOGI(TAG, "Encoder configured: Codec2 mode %d, %d samples/frame, %d bytes/frame, filters=%d",
             codec2Mode, frameSamples_, encoder_->bytesPerFrame(), enableFilters);
    return true;
}

bool I2SCapture::start() {
    if (!i2sInitialized_ || !encoder_ || capturing_.load()) return false;

    // Set capturing BEFORE starting task to avoid race (same pattern as LXST-kt)
    capturing_.store(true, std::memory_order_relaxed);

    BaseType_t ret = xTaskCreatePinnedToCore(
        captureTask, "lxst_cap", CAPTURE_TASK_STACK, this,
        CAPTURE_TASK_PRIORITY, reinterpret_cast<TaskHandle_t*>(&taskHandle_),
        CAPTURE_TASK_CORE);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create capture task");
        capturing_.store(false, std::memory_order_relaxed);
        return false;
    }

    ESP_LOGI(TAG, "Capture started");
    return true;
}

void I2SCapture::stop() {
    if (!capturing_.load()) return;

    capturing_.store(false, std::memory_order_relaxed);

    // Wait for task to exit
    if (taskHandle_) {
        vTaskDelay(pdMS_TO_TICKS(50));
        taskHandle_ = nullptr;
    }

    if (i2sInitialized_) {
        i2s_stop(I2S_NUM_1);
        i2s_driver_uninstall(I2S_NUM_1);
        i2sInitialized_ = false;
    }

    ESP_LOGI(TAG, "Capture stopped");
}

void I2SCapture::destroyEncoder() {
    delete encoder_;
    encoder_ = nullptr;
    delete filterChain_;
    filterChain_ = nullptr;
    delete encodedRing_;
    encodedRing_ = nullptr;
    free(accumBuffer_);
    accumBuffer_ = nullptr;
    free(silenceBuf_);
    silenceBuf_ = nullptr;
    accumCount_ = 0;
}

void I2SCapture::captureTask(void* param) {
    auto* self = static_cast<I2SCapture*>(param);
    self->captureLoop();
    vTaskDelete(NULL);
}

void I2SCapture::captureLoop() {
    // I2S read buffer: read in chunks smaller than a codec frame
    static constexpr int READ_SAMPLES = 128;
    int16_t readBuf[READ_SAMPLES];
    size_t bytesRead = 0;

    ESP_LOGI(TAG, "Capture task running on core %d", xPortGetCoreID());

    while (capturing_.load(std::memory_order_relaxed)) {
        // Read samples from I2S DMA
        esp_err_t err = i2s_read(I2S_NUM_1, readBuf, sizeof(readBuf), &bytesRead,
                                 pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytesRead == 0) continue;

        int samplesRead = bytesRead / sizeof(int16_t);

        // Accumulate into frame-sized buffer
        int offset = 0;
        while (offset < samplesRead && capturing_.load(std::memory_order_relaxed)) {
            int needed = frameSamples_ - accumCount_;
            int available = samplesRead - offset;
            int toCopy = (available < needed) ? available : needed;

            memcpy(accumBuffer_ + accumCount_, readBuf + offset, toCopy * sizeof(int16_t));
            accumCount_ += toCopy;
            offset += toCopy;

            if (accumCount_ == frameSamples_) {
                // Full frame ready — process it
                int16_t* frameData = muted_.load(std::memory_order_relaxed)
                    ? silenceBuf_ : accumBuffer_;

                // Apply voice filters
                if (filtersEnabled_ && filterChain_ && !muted_.load(std::memory_order_relaxed)) {
                    filterChain_->process(frameData, frameSamples_, SAMPLE_RATE);
                }

                // Encode
                int encodedLen = encoder_->encode(frameData, frameSamples_,
                                                  encodeBuf_, sizeof(encodeBuf_));
                if (encodedLen > 0 && encodedRing_) {
                    if (!encodedRing_->write(encodeBuf_, encodedLen)) {
                        // Ring full — drop oldest, then write
                        uint8_t discard[256];
                        int discardLen;
                        encodedRing_->read(discard, sizeof(discard), &discardLen);
                        encodedRing_->write(encodeBuf_, encodedLen);
                    }
                }

                accumCount_ = 0;
            }
        }
    }

    ESP_LOGI(TAG, "Capture task exiting");
}

bool I2SCapture::readEncodedPacket(uint8_t* dest, int maxLength, int* actualLength) {
    if (!encodedRing_) return false;
    return encodedRing_->read(dest, maxLength, actualLength);
}

int I2SCapture::availablePackets() const {
    if (!encodedRing_) return 0;
    return encodedRing_->availableSlots();
}

#endif // ARDUINO
