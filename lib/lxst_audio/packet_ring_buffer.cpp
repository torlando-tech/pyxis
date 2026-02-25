// Copyright (c) 2024 LXST contributors
// SPDX-License-Identifier: MPL-2.0

#include "packet_ring_buffer.h"

#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

PacketRingBuffer::PacketRingBuffer(int maxFrames, int frameSamples)
    : maxFrames_(maxFrames), frameSamples_(frameSamples) {
    size_t bytes = sizeof(int16_t) * maxFrames * frameSamples;
#ifdef BOARD_HAS_PSRAM
    buffer_ = static_cast<int16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
#else
    buffer_ = static_cast<int16_t*>(malloc(bytes));
#endif
    if (buffer_) {
        memset(buffer_, 0, bytes);
    }
}

PacketRingBuffer::~PacketRingBuffer() {
    free(buffer_);
}

bool PacketRingBuffer::write(const int16_t* samples, int count) {
    if (count != frameSamples_ || !buffer_) return false;

    int w = writeIndex_.load(std::memory_order_relaxed);
    int r = readIndex_.load(std::memory_order_acquire);

    int nextW = (w + 1) % maxFrames_;
    if (nextW == r) return false;

    memcpy(buffer_ + w * frameSamples_, samples, sizeof(int16_t) * frameSamples_);
    writeIndex_.store(nextW, std::memory_order_release);
    return true;
}

bool PacketRingBuffer::read(int16_t* dest, int count) {
    if (count != frameSamples_ || !buffer_) return false;

    int r = readIndex_.load(std::memory_order_relaxed);
    int w = writeIndex_.load(std::memory_order_acquire);

    if (r == w) return false;

    memcpy(dest, buffer_ + r * frameSamples_, sizeof(int16_t) * frameSamples_);
    readIndex_.store((r + 1) % maxFrames_, std::memory_order_release);
    return true;
}

int PacketRingBuffer::availableFrames() const {
    int w = writeIndex_.load(std::memory_order_acquire);
    int r = readIndex_.load(std::memory_order_acquire);
    int avail = w - r;
    if (avail < 0) avail += maxFrames_;
    return avail;
}

void PacketRingBuffer::reset() {
    writeIndex_.store(0, std::memory_order_relaxed);
    readIndex_.store(0, std::memory_order_relaxed);
}
