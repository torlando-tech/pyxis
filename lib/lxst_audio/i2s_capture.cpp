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
#include <Arduino.h>

using namespace Hardware::TDeck;

static const char* TAG = "LXST:Capture";

// Defined in main.cpp — sends to both Serial and UDP
extern "C" void pyxis_log(const char* msg);

I2SCapture::I2SCapture() = default;

I2SCapture::~I2SCapture() {
    stop();
    // Ensure I2S driver is released even if stop() skipped (not capturing)
    if (i2sInitialized_) {
        i2s_stop(I2S_NUM_1);
        i2s_driver_uninstall(I2S_NUM_1);
        i2sInitialized_ = false;
    }
    releaseBuffers();
}

bool I2SCapture::init() {
    if (i2sInitialized_) return true;

    // Defensively uninstall in case a previous session leaked the driver
    i2s_driver_uninstall(I2S_NUM_1);

    // Configure I2S_NUM_1 for mic capture from ES7210
    // Settings match official LilyGO T-Deck Plus Microphone example
    i2s_config_t i2s_config = {};
    i2s_config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
    i2s_config.sample_rate = I2S_SAMPLE_RATE;  // 8kHz — matches Codec2 directly
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_config.channel_format = I2S_CHANNEL_FMT_ALL_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    // At 8kHz × 2 TDM channels = 16ksps. Filter+encode burst for 1600 samples
    // takes ~20ms; 16 × 64 = 1024 samples = 64ms headroom prevents DMA overflow.
    i2s_config.dma_buf_count = 16;
    i2s_config.dma_buf_len = 64;
    i2s_config.use_apll = true;   // APLL gives accurate audio clocks (vs main PLL integer dividers)
    i2s_config.tx_desc_auto_clear = true;
    i2s_config.fixed_mclk = 4096000;                    // Force 4.096MHz MCLK (matches ES7210 coeff table for 8kHz)
    i2s_config.mclk_multiple = I2S_MCLK_MULTIPLE_256;   // Ignored when fixed_mclk is set
    i2s_config.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;
    // TDM channel mask — required for ES7210 on T-Deck Plus
    i2s_config.chan_mask = static_cast<i2s_channel_t>(I2S_TDM_ACTIVE_CH0 | I2S_TDM_ACTIVE_CH1);

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

    i2s_zero_dma_buffer(I2S_NUM_1);

    i2sInitialized_ = true;

    ESP_LOGI(TAG, "I2S capture initialized: %dHz 16-bit TDM, MCLK=4.096MHz", I2S_SAMPLE_RATE);
    return true;
}

bool I2SCapture::configureEncoder(Codec2Wrapper* codec, bool enableFilters) {
    releaseBuffers();

    if (!codec || !codec->isCreated()) {
        ESP_LOGE(TAG, "Invalid codec pointer");
        return false;
    }
    codec_ = codec;

    // Accumulate FRAMES_PER_BATCH codec frames before filter+encode.
    // Columba uses 200ms (1600 samples for Codec2 3200) so the AGC operates
    // on meaningful block sizes.  With only 160 samples (20ms) the AGC blocks
    // are 16 samples and gain-pump, producing buzzy audio.
    frameSamples_ = codec_->samplesPerFrame() * FRAMES_PER_BATCH;
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

    // Filter chain: 1 channel (mono), voice band 300-3400Hz, AGC -12dB target, 12dB max gain
    // PGA gain is 21dB; loud speech peaks around -6dBFS, quiet around -20dBFS.
    // AGC boosts quiet sections; 12dB max prevents noise pumping during silence.
    if (enableFilters) {
        filterChain_ = new VoiceFilterChain(1, 300.0f, 3400.0f, -12.0f, 12.0f);
    }

    ESP_LOGI(TAG, "Encoder configured: Codec2 mode %d, %d samples/batch (%d x %d), %d bytes/frame, filters=%d",
             codec_->libraryMode(), frameSamples_, FRAMES_PER_BATCH,
             codec_->samplesPerFrame(), codec_->bytesPerFrame(), enableFilters);
    return true;
}

bool I2SCapture::start() {
    if (!i2sInitialized_ || !codec_ || capturing_.load()) return false;

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

void I2SCapture::releaseBuffers() {
    codec_ = nullptr;  // Not owned — don't delete
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
    // I2S read buffer: TDM interleaved, 2 channels at 8kHz
    static constexpr int READ_SAMPLES = 256;
    int16_t readBuf[READ_SAMPLES];
    // CH0 mono after TDM deinterleave (÷2)
    int16_t ch0Buf[READ_SAMPLES / 2];
    size_t bytesRead = 0;

    {
        char logbuf[96];
        snprintf(logbuf, sizeof(logbuf), "[CAP] Capture task on core %d, I2S=%dHz, codec=%dHz, stack=%d",
                 xPortGetCoreID(), I2S_SAMPLE_RATE, CODEC_SAMPLE_RATE, CAPTURE_TASK_STACK);
        pyxis_log(logbuf);
    }
    uint32_t framesEncoded = 0;
    uint32_t totalSamples = 0;      // Total mono samples after deinterleave
    uint32_t rateCheckMs = millis(); // For sample rate measurement
    int16_t runningPeak = 0;        // Peak of mono samples per interval
    uint32_t ringDrops = 0;         // Ring buffer overflow counter

    while (capturing_.load(std::memory_order_relaxed)) {
        // Read samples from I2S DMA (at 8kHz, TDM 2-ch)
        esp_err_t err = i2s_read(I2S_NUM_1, readBuf, sizeof(readBuf), &bytesRead,
                                 pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytesRead == 0) continue;

        int samplesRead = bytesRead / sizeof(int16_t);

        // Dump first raw I2S samples on each capture start
        if (framesEncoded == 0 && samplesRead >= 16 && totalSamples == 0) {
            char rawdump[192];
            int pos = snprintf(rawdump, sizeof(rawdump),
                "[CAP] Raw I2S (%d read, %zu bytes): ", samplesRead, bytesRead);
            for (int d = 0; d < 16 && pos < 180; d++)
                pos += snprintf(rawdump + pos, sizeof(rawdump) - pos, "%d ", readBuf[d]);
            pyxis_log(rawdump);
        }

        // TDM deinterleave — extract CH0 (mic) at 8kHz.
        // readBuf is [CH0,CH1,CH0,CH1,...], CH0 at even indices.
        int ch0Count = samplesRead / 2;
        for (int i = 0; i < ch0Count; i++) {
            ch0Buf[i] = readBuf[i * 2];
            int16_t v = ch0Buf[i] < 0 ? -ch0Buf[i] : ch0Buf[i];
            if (v > runningPeak) runningPeak = v;
        }

        // Measure actual sample rate
        totalSamples += ch0Count;
        uint32_t now = millis();
        uint32_t elapsed = now - rateCheckMs;
        if (elapsed >= 2000) {
            uint32_t rate = (totalSamples * 1000) / elapsed;
            {
                char logbuf[128];
                snprintf(logbuf, sizeof(logbuf), "[CAP] rate=%luHz frames=%lu peak=%d ringDrops=%lu",
                         (unsigned long)rate, (unsigned long)framesEncoded,
                         runningPeak, (unsigned long)ringDrops);
                pyxis_log(logbuf);
            }
            totalSamples = 0;
            rateCheckMs = now;
            runningPeak = 0;
        }

        // Accumulate mono samples into frame-sized buffer
        int offset = 0;
        while (offset < ch0Count && capturing_.load(std::memory_order_relaxed)) {
            int needed = frameSamples_ - accumCount_;
            int available = ch0Count - offset;
            int toCopy = (available < needed) ? available : needed;

            memcpy(accumBuffer_ + accumCount_, ch0Buf + offset, toCopy * sizeof(int16_t));
            accumCount_ += toCopy;
            offset += toCopy;

            if (accumCount_ == frameSamples_) {
                // Full frame ready — process it
                int16_t* frameData = muted_.load(std::memory_order_relaxed)
                    ? silenceBuf_ : accumBuffer_;

                // Apply voice filters
                if (filtersEnabled_ && filterChain_ && !muted_.load(std::memory_order_relaxed)) {
                    filterChain_->process(frameData, frameSamples_, CODEC_SAMPLE_RATE);
                }

                // Log PCM levels for first few frames and periodically
                if (framesEncoded < 5 || (framesEncoded % 500 == 0)) {
                    int16_t maxVal = 0;
                    for (int s = 0; s < frameSamples_; s++) {
                        int16_t v = accumBuffer_[s] < 0 ? -accumBuffer_[s] : accumBuffer_[s];
                        if (v > maxVal) maxVal = v;
                    }
                    char logbuf[96];
                    snprintf(logbuf, sizeof(logbuf), "[CAP] PCM peak=%d (first=%d,%d,%d,%d)",
                             maxVal, accumBuffer_[0], accumBuffer_[1],
                             accumBuffer_[2], accumBuffer_[3]);
                    pyxis_log(logbuf);
                }

                // Encode
                int encodedLen = codec_->encode(frameData, frameSamples_,
                                                encodeBuf_, sizeof(encodeBuf_));
                if (encodedLen > 0) {
                    framesEncoded++;
                    if (framesEncoded <= 3 || (framesEncoded % 500 == 0)) {
                        char logbuf[128];
                        char hex[64];
                        int hpos = 0;
                        for (int h = 0; h < encodedLen && h < 20 && hpos < 60; h++)
                            hpos += snprintf(hex + hpos, 64 - hpos, "%02X ", encodeBuf_[h]);
                        snprintf(logbuf, sizeof(logbuf), "[CAP] Encoded #%lu: %d bytes: %s",
                                 (unsigned long)framesEncoded, encodedLen, hex);
                        pyxis_log(logbuf);
                    }
                }
                if (encodedLen > 0 && encodedRing_) {
                    if (!encodedRing_->write(encodeBuf_, encodedLen)) {
                        ringDrops++;
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
