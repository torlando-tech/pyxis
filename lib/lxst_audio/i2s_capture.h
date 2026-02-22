// Copyright (c) 2024 LXST contributors
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <atomic>

class EncodedRingBuffer;
class VoiceFilterChain;
class Codec2Wrapper;

/**
 * ESP32 I2S microphone capture engine for LXST voice streaming.
 *
 * Uses I2S_NUM_1 to capture audio from the ES7210 mic array.
 * Runs a FreeRTOS task that reads I2S DMA, applies voice filters,
 * encodes with Codec2, and writes to an EncodedRingBuffer for
 * the network layer to consume.
 *
 * Audio flow:
 *   I2S DMA -> accumulate to frame -> filter -> encode -> EncodedRingBuffer
 */
class I2SCapture {
public:
    I2SCapture();
    ~I2SCapture();

    I2SCapture(const I2SCapture&) = delete;
    I2SCapture& operator=(const I2SCapture&) = delete;

    /**
     * Initialize the I2S capture port.
     * Does NOT start capturing — call start() after init.
     * @return true on success
     */
    bool init();

    /**
     * Configure the encoder. Must be called before start().
     * @param codec Shared Codec2Wrapper (not owned — caller manages lifecycle)
     * @param enableFilters Whether to apply HPF+LPF+AGC filter chain
     * @return true on success
     */
    bool configureEncoder(Codec2Wrapper* codec, bool enableFilters = true);

    /** Start the capture task. Returns immediately. */
    bool start();

    /** Stop the capture task and release I2S resources. */
    void stop();

    /** Mute/unmute the microphone (sends silence when muted). */
    void setMute(bool muted) { muted_.store(muted, std::memory_order_relaxed); }
    bool isMuted() const { return muted_.load(std::memory_order_relaxed); }

    /** Check if currently capturing. */
    bool isCapturing() const { return capturing_.load(std::memory_order_relaxed); }

    /**
     * Read the next encoded packet from the ring buffer.
     * Called by the network layer.
     *
     * @param dest        Output buffer for encoded packet
     * @param maxLength   Size of output buffer
     * @param actualLength [out] Actual packet size
     * @return true if a packet was read
     */
    bool readEncodedPacket(uint8_t* dest, int maxLength, int* actualLength);

    /** Number of encoded packets waiting in the ring buffer. */
    int availablePackets() const;

    /** Release capture buffers (does NOT destroy the shared codec). */
    void releaseBuffers();

private:
    static void captureTask(void* param);
    void captureLoop();

    bool i2sInitialized_ = false;
    std::atomic<bool> capturing_{false};
    std::atomic<bool> muted_{false};
    void* taskHandle_ = nullptr;

    // Audio pipeline components
    Codec2Wrapper* codec_ = nullptr;  // Shared, not owned
    VoiceFilterChain* filterChain_ = nullptr;
    EncodedRingBuffer* encodedRing_ = nullptr;

    // Accumulation buffer: I2S delivers variable bursts, we need fixed-size frames
    int16_t* accumBuffer_ = nullptr;
    int accumCount_ = 0;
    int frameSamples_ = 0;  // Codec2 samples per frame (e.g., 320 for 700C, 160 for 1600/3200)

    // Pre-allocated encode output buffer
    uint8_t encodeBuf_[256];

    // Silence buffer for mute
    int16_t* silenceBuf_ = nullptr;

    bool filtersEnabled_ = true;

    static constexpr int I2S_SAMPLE_RATE = 16000;  // I2S runs at 16kHz (matches T-Deck Plus reference)
    static constexpr int CODEC_SAMPLE_RATE = 8000; // Codec2 expects 8kHz — we downsample 2:1
    static constexpr int ENCODED_RING_SLOTS = 32;
    static constexpr int ENCODED_RING_MAX_BYTES = 256;
    static constexpr int CAPTURE_TASK_STACK = 16384;
    static constexpr int CAPTURE_TASK_PRIORITY = 5;
    static constexpr int CAPTURE_TASK_CORE = 0;
};
