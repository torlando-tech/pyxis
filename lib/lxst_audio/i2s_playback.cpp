// Copyright (c) 2024 LXST contributors
// SPDX-License-Identifier: MPL-2.0

#include "i2s_playback.h"

#ifdef ARDUINO
#include <driver/i2s.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <Hardware/TDeck/Config.h>
#include "codec_wrapper.h"
#include "packet_ring_buffer.h"

using namespace Hardware::TDeck;

static const char* TAG = "LXST:Playback";

I2SPlayback::I2SPlayback() = default;

I2SPlayback::~I2SPlayback() {
    stop();
    destroyDecoder();
}

bool I2SPlayback::configureDecoder(int codec2Mode) {
    destroyDecoder();

    decoder_ = new Codec2Wrapper();
    if (!decoder_->create(codec2Mode)) {
        ESP_LOGE(TAG, "Failed to create Codec2 decoder mode %d", codec2Mode);
        delete decoder_;
        decoder_ = nullptr;
        return false;
    }

    frameSamples_ = decoder_->samplesPerFrame();

    // PCM ring buffer in PSRAM
    pcmRing_ = new PacketRingBuffer(PCM_RING_FRAMES, frameSamples_);

    // Decode buffer in PSRAM
    decodeBufSize_ = frameSamples_ * 2;  // Extra room for mode switches
    decodeBuf_ = static_cast<int16_t*>(
        heap_caps_malloc(sizeof(int16_t) * decodeBufSize_, MALLOC_CAP_SPIRAM));

    // Drop buffer (for ring overflow discard)
    dropBuf_ = static_cast<int16_t*>(
        heap_caps_malloc(sizeof(int16_t) * frameSamples_, MALLOC_CAP_SPIRAM));

    ESP_LOGI(TAG, "Decoder configured: Codec2 mode %d, %d samples/frame",
             codec2Mode, frameSamples_);
    return true;
}

bool I2SPlayback::start() {
    if (!decoder_ || playing_.load()) return false;

    // Caller (LXSTAudio) is responsible for calling tone_deinit() first.
    // Defensively uninstall in case it wasn't done.
    i2s_driver_uninstall(I2S_NUM_0);

    // Configure I2S_NUM_0 for voice playback
    i2s_config_t i2s_config = {};
    i2s_config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
    i2s_config.sample_rate = SAMPLE_RATE;
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_config.dma_buf_count = 8;
    i2s_config.dma_buf_len = 64;
    i2s_config.use_apll = false;
    i2s_config.tx_desc_auto_clear = true;
    i2s_config.fixed_mclk = 0;

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S_NUM_0 driver install failed: %d", err);
        return false;
    }

    i2s_pin_config_t pin_config = {};
    pin_config.mck_io_num = I2S_PIN_NO_CHANGE;
    pin_config.bck_io_num = Audio::I2S_BCK;
    pin_config.ws_io_num = Audio::I2S_WS;
    pin_config.data_out_num = Audio::I2S_DOUT;
    pin_config.data_in_num = I2S_PIN_NO_CHANGE;

    err = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S_NUM_0 pin config failed: %d", err);
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }

    i2sInitialized_ = true;

    // Reset ring and prebuffer state
    if (pcmRing_) pcmRing_->reset();

    playing_.store(true, std::memory_order_relaxed);

    BaseType_t ret = xTaskCreatePinnedToCore(
        playbackTask, "lxst_play", PLAYBACK_TASK_STACK, this,
        PLAYBACK_TASK_PRIORITY, reinterpret_cast<TaskHandle_t*>(&taskHandle_),
        PLAYBACK_TASK_CORE);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create playback task");
        playing_.store(false, std::memory_order_relaxed);
        i2s_driver_uninstall(I2S_NUM_0);
        i2sInitialized_ = false;
        return false;
    }

    ESP_LOGI(TAG, "Playback started");
    return true;
}

void I2SPlayback::stop() {
    if (!playing_.load()) return;

    playing_.store(false, std::memory_order_relaxed);

    if (taskHandle_) {
        vTaskDelay(pdMS_TO_TICKS(50));
        taskHandle_ = nullptr;
    }

    if (i2sInitialized_) {
        // Write silence to flush DMA
        int16_t silence[128] = {0};
        size_t written;
        i2s_write(I2S_NUM_0, silence, sizeof(silence), &written, pdMS_TO_TICKS(100));

        i2s_stop(I2S_NUM_0);
        i2s_driver_uninstall(I2S_NUM_0);
        i2sInitialized_ = false;
    }

    // Re-init Tone.cpp's I2S driver so notification tones work again
    // The next call to tone_play() will re-initialize via tone_init()

    ESP_LOGI(TAG, "Playback stopped");
}

void I2SPlayback::destroyDecoder() {
    delete decoder_;
    decoder_ = nullptr;
    delete pcmRing_;
    pcmRing_ = nullptr;
    free(decodeBuf_);
    decodeBuf_ = nullptr;
    free(dropBuf_);
    dropBuf_ = nullptr;
    decodeBufSize_ = 0;
    frameSamples_ = 0;
}

bool I2SPlayback::writeEncodedPacket(const uint8_t* data, int length) {
    if (!decoder_ || !pcmRing_ || !decodeBuf_) return false;

    int decodedSamples = decoder_->decode(data, length, decodeBuf_, decodeBufSize_);
    if (decodedSamples <= 0) return false;

    // Write decoded PCM to ring buffer
    if (!pcmRing_->write(decodeBuf_, decodedSamples)) {
        // Ring full — drop oldest frame, then write
        if (dropBuf_) {
            pcmRing_->read(dropBuf_, decodedSamples);
        }
        pcmRing_->write(decodeBuf_, decodedSamples);
    }

    return true;
}

int I2SPlayback::bufferedFrames() const {
    if (!pcmRing_) return 0;
    return pcmRing_->availableFrames();
}

void I2SPlayback::playbackTask(void* param) {
    auto* self = static_cast<I2SPlayback*>(param);
    self->playbackLoop();
    vTaskDelete(NULL);
}

void I2SPlayback::playbackLoop() {
    ESP_LOGI(TAG, "Playback task running on core %d", xPortGetCoreID());

    // Wait for prebuffer
    bool prebuffered = false;

    // Frame buffer for reading from ring
    int16_t* frameBuf = static_cast<int16_t*>(
        heap_caps_malloc(sizeof(int16_t) * frameSamples_, MALLOC_CAP_SPIRAM));
    if (!frameBuf) {
        ESP_LOGE(TAG, "Failed to allocate frame buffer");
        return;
    }

    // Silence frame for underruns
    int16_t* silenceFrame = static_cast<int16_t*>(
        heap_caps_calloc(frameSamples_, sizeof(int16_t), MALLOC_CAP_SPIRAM));

    while (playing_.load(std::memory_order_relaxed)) {
        // Prebuffer: wait until we have enough frames before starting playback
        if (!prebuffered) {
            if (pcmRing_ && pcmRing_->availableFrames() >= PREBUFFER_FRAMES) {
                prebuffered = true;
                ESP_LOGI(TAG, "Prebuffer complete, starting playback");
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        }

        // Read a frame from the ring buffer
        bool hasFrame = pcmRing_ && pcmRing_->read(frameBuf, frameSamples_);

        int16_t* outputData;
        if (!hasFrame) {
            // Underrun — output silence
            outputData = silenceFrame;
        } else if (muted_.load(std::memory_order_relaxed)) {
            // Muted — output silence but keep consuming
            outputData = silenceFrame;
        } else {
            outputData = frameBuf;
        }

        // Write to I2S DMA
        size_t bytesWritten;
        esp_err_t err = i2s_write(I2S_NUM_0, outputData,
                                  frameSamples_ * sizeof(int16_t),
                                  &bytesWritten, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S write error: %d", err);
        }
    }

    free(frameBuf);
    free(silenceFrame);

    ESP_LOGI(TAG, "Playback task exiting");
}

#endif // ARDUINO
