// Copyright (c) 2024 LXST contributors
// SPDX-License-Identifier: MPL-2.0

#include "encoded_ring_buffer.h"

#include <cstdlib>     // malloc, free — needed on Linux clang; macOS leaks via header transitivity
#include <cstring>

#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

EncodedRingBuffer::EncodedRingBuffer(int maxSlots, int maxBytesPerSlot)
    : maxSlots_(maxSlots),
      maxBytesPerSlot_(maxBytesPerSlot),
      slotSize_(static_cast<int>(sizeof(int32_t)) + maxBytesPerSlot) {
    size_t bytes = maxSlots * slotSize_;
#ifdef BOARD_HAS_PSRAM
    buffer_ = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
#else
    buffer_ = static_cast<uint8_t*>(malloc(bytes));
#endif
    if (buffer_) {
        memset(buffer_, 0, bytes);
    }
}

EncodedRingBuffer::~EncodedRingBuffer() {
    free(buffer_);
}

bool EncodedRingBuffer::write(const uint8_t* data, int length) {
    if (length <= 0 || length > maxBytesPerSlot_ || !buffer_) return false;

    int w = writeIndex_.load(std::memory_order_relaxed);
    int r = readIndex_.load(std::memory_order_acquire);

    int nextW = (w + 1) % maxSlots_;
    if (nextW == r) return false;

    uint8_t* slot = buffer_ + w * slotSize_;
    memcpy(slot, &length, sizeof(int32_t));
    memcpy(slot + sizeof(int32_t), data, length);

    writeIndex_.store(nextW, std::memory_order_release);
    return true;
}

bool EncodedRingBuffer::read(uint8_t* dest, int maxLength, int* actualLength) {
    if (!buffer_) return false;

    int r = readIndex_.load(std::memory_order_relaxed);
    int w = writeIndex_.load(std::memory_order_acquire);

    if (r == w) return false;

    uint8_t* slot = buffer_ + r * slotSize_;
    int32_t length;
    memcpy(&length, slot, sizeof(int32_t));

    if (length > maxLength) {
        readIndex_.store((r + 1) % maxSlots_, std::memory_order_release);
        *actualLength = 0;
        return false;
    }

    memcpy(dest, slot + sizeof(int32_t), length);
    *actualLength = length;

    readIndex_.store((r + 1) % maxSlots_, std::memory_order_release);
    return true;
}

int EncodedRingBuffer::availableSlots() const {
    int w = writeIndex_.load(std::memory_order_acquire);
    int r = readIndex_.load(std::memory_order_acquire);
    int avail = w - r;
    if (avail < 0) avail += maxSlots_;
    return avail;
}

void EncodedRingBuffer::reset() {
    writeIndex_.store(0, std::memory_order_relaxed);
    readIndex_.store(0, std::memory_order_relaxed);
}
