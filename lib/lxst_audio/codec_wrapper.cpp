// Copyright (c) 2024 LXST contributors
// SPDX-License-Identifier: MPL-2.0

#include "codec_wrapper.h"
#include <codec2.h>
#include <cstring>

#ifdef ARDUINO
#include <esp_log.h>
static const char* TAG = "LXST:Codec2";
#define LOGI(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) ESP_LOGE(TAG, fmt, ##__VA_ARGS__)
#else
#include <cstdio>
#define LOGI(fmt, ...) printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOGW(fmt, ...) printf("[WARN] " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) printf("[ERR]  " fmt "\n", ##__VA_ARGS__)
#endif

Codec2Wrapper::Codec2Wrapper() = default;

Codec2Wrapper::~Codec2Wrapper() {
    destroy();
}

bool Codec2Wrapper::create(int libraryMode) {
    destroy();

    codec2_ = codec2_create(libraryMode);
    if (!codec2_) {
        LOGE("Codec2 create failed for library mode %d", libraryMode);
        return false;
    }

    libraryMode_ = libraryMode;
    samplesPerFrame_ = codec2_samples_per_frame(codec2_);
    bytesPerFrame_ = codec2_bytes_per_frame(codec2_);
    modeHeader_ = libraryModeToHeader(libraryMode);

    LOGI("Codec2 created: libMode=%d header=0x%02x samples/frame=%d bytes/frame=%d",
         libraryMode, modeHeader_, samplesPerFrame_, bytesPerFrame_);
    return true;
}

void Codec2Wrapper::destroy() {
    if (codec2_) {
        codec2_destroy(codec2_);
        codec2_ = nullptr;
    }
    samplesPerFrame_ = 0;
    bytesPerFrame_ = 0;
    modeHeader_ = 0;
    libraryMode_ = 0;
}

int Codec2Wrapper::decode(const uint8_t* encoded, int encodedBytes,
                          int16_t* output, int maxOutputSamples) {
    if (!codec2_ || encodedBytes < 1) return -1;

    // First byte is mode header -- check if mode changed
    uint8_t header = encoded[0];
    if (header != modeHeader_) {
        int newMode = headerToLibraryMode(header);
        if (newMode >= 0) {
            LOGI("Codec2 mode switch: header 0x%02x -> libMode %d", header, newMode);
            codec2_destroy(codec2_);
            codec2_ = codec2_create(newMode);
            if (!codec2_) {
                LOGE("Codec2 mode switch failed");
                return -1;
            }
            libraryMode_ = newMode;
            samplesPerFrame_ = codec2_samples_per_frame(codec2_);
            bytesPerFrame_ = codec2_bytes_per_frame(codec2_);
            modeHeader_ = header;
        } else {
            LOGW("Unknown Codec2 header: 0x%02x", header);
            return -1;
        }
    }

    // Skip header byte, decode remaining sub-frames
    const uint8_t* data = encoded + 1;
    int dataLen = encodedBytes - 1;
    int numFrames = dataLen / bytesPerFrame_;
    int totalSamples = numFrames * samplesPerFrame_;

    if (totalSamples > maxOutputSamples) {
        LOGW("Codec2 decode: output buffer too small (%d > %d)",
             totalSamples, maxOutputSamples);
        return -1;
    }

    for (int i = 0; i < numFrames; i++) {
        codec2_decode(codec2_,
                      output + i * samplesPerFrame_,
                      data + i * bytesPerFrame_);
    }

    return totalSamples;
}

int Codec2Wrapper::encode(const int16_t* pcm, int pcmSamples,
                          uint8_t* output, int maxOutputBytes) {
    if (!codec2_) return -1;

    int numFrames = pcmSamples / samplesPerFrame_;
    int encodedSize = 1 + numFrames * bytesPerFrame_;

    if (encodedSize > maxOutputBytes) {
        LOGW("Codec2 encode: output buffer too small (%d > %d)",
             encodedSize, maxOutputBytes);
        return -1;
    }

    // Prepend mode header byte
    output[0] = modeHeader_;

    for (int i = 0; i < numFrames; i++) {
        codec2_encode(codec2_,
                      output + 1 + i * bytesPerFrame_,
                      const_cast<int16_t*>(pcm + i * samplesPerFrame_));
    }

    return encodedSize;
}

// Wire format mapping (matches Python LXST and LXST-kt Codec2.kt):
//   header 0x00 = 700C  -> library mode 8
//   header 0x01 = 1200  -> library mode 5
//   header 0x02 = 1300  -> library mode 4
//   header 0x03 = 1400  -> library mode 3
//   header 0x04 = 1600  -> library mode 2
//   header 0x05 = 2400  -> library mode 1
//   header 0x06 = 3200  -> library mode 0

int Codec2Wrapper::headerToLibraryMode(uint8_t header) {
    switch (header) {
        case 0x00: return 8;  // 700C
        case 0x01: return 5;  // 1200
        case 0x02: return 4;  // 1300
        case 0x03: return 3;  // 1400
        case 0x04: return 2;  // 1600
        case 0x05: return 1;  // 2400
        case 0x06: return 0;  // 3200
        default:   return -1;
    }
}

uint8_t Codec2Wrapper::libraryModeToHeader(int libraryMode) {
    switch (libraryMode) {
        case 8:  return 0x00;  // 700C
        case 5:  return 0x01;  // 1200
        case 4:  return 0x02;  // 1300
        case 3:  return 0x03;  // 1400
        case 2:  return 0x04;  // 1600
        case 1:  return 0x05;  // 2400
        case 0:  return 0x06;  // 3200
        default: return 0xFF;
    }
}
