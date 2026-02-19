// Copyright (c) 2024 LXST contributors
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

/**
 * Lock-free Single-Producer Single-Consumer (SPSC) ring buffer for int16 audio.
 *
 * Ported from LXST-kt native layer. Uses acquire/release memory ordering
 * on read/write indices for correct cross-thread visibility without mutexes.
 *
 * On ESP32-S3, the buffer is allocated in PSRAM to conserve internal RAM.
 */
class PacketRingBuffer {
public:
    PacketRingBuffer(int maxFrames, int frameSamples);
    ~PacketRingBuffer();

    PacketRingBuffer(const PacketRingBuffer&) = delete;
    PacketRingBuffer& operator=(const PacketRingBuffer&) = delete;

    bool write(const int16_t* samples, int count);
    bool read(int16_t* dest, int count);
    int availableFrames() const;
    int capacity() const { return maxFrames_; }
    int frameSamples() const { return frameSamples_; }
    void reset();

private:
    const int maxFrames_;
    const int frameSamples_;
    int16_t* buffer_;

    std::atomic<int> writeIndex_{0};
    std::atomic<int> readIndex_{0};
};
