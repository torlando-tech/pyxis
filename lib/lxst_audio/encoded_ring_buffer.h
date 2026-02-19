// Copyright (c) 2024 LXST contributors
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <atomic>
#include <cstdint>

/**
 * Lock-free SPSC ring buffer for variable-length encoded audio packets.
 *
 * Ported from LXST-kt native layer. Each slot has a fixed max size but
 * tracks actual length. Lock-free protocol identical to PacketRingBuffer.
 *
 * Slot layout: [int32 length][uint8 data[maxBytesPerSlot]] x maxSlots
 */
class EncodedRingBuffer {
public:
    EncodedRingBuffer(int maxSlots, int maxBytesPerSlot);
    ~EncodedRingBuffer();

    EncodedRingBuffer(const EncodedRingBuffer&) = delete;
    EncodedRingBuffer& operator=(const EncodedRingBuffer&) = delete;

    bool write(const uint8_t* data, int length);
    bool read(uint8_t* dest, int maxLength, int* actualLength);
    int availableSlots() const;
    void reset();

private:
    const int maxSlots_;
    const int maxBytesPerSlot_;
    const int slotSize_;

    uint8_t* buffer_;

    std::atomic<int> writeIndex_{0};
    std::atomic<int> readIndex_{0};
};
