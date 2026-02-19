// Copyright (c) 2024 LXST contributors
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <atomic>

class PacketRingBuffer;
class Codec2Wrapper;

/**
 * ESP32 I2S speaker playback engine for LXST voice streaming.
 *
 * Shares I2S_NUM_0 with the tone generator (Tone.cpp). When voice playback
 * starts, it takes ownership of I2S_NUM_0 and reconfigures it for voice.
 * When stopped, I2S_NUM_0 is released so tones can reclaim it.
 *
 * The tone generator must be stopped before starting voice playback.
 *
 * Audio flow:
 *   Network -> writeEncodedPacket() -> decode -> PCM ring buffer -> I2S DMA
 */
class I2SPlayback {
public:
    I2SPlayback();
    ~I2SPlayback();

    I2SPlayback(const I2SPlayback&) = delete;
    I2SPlayback& operator=(const I2SPlayback&) = delete;

    /**
     * Configure the decoder for a specific Codec2 mode.
     * @param codec2Mode Codec2 library mode (0=3200, 2=1600, 8=700C)
     * @return true on success
     */
    bool configureDecoder(int codec2Mode);

    /**
     * Start voice playback. Takes over I2S_NUM_0.
     * Tone generator must be stopped first.
     * @return true on success
     */
    bool start();

    /** Stop voice playback and release I2S_NUM_0. */
    void stop();

    /**
     * Write an encoded packet for playback.
     * Called by the network layer. Decodes to PCM and queues.
     *
     * @param data    Encoded packet (with LXST mode header byte)
     * @param length  Packet length in bytes
     * @return true on success
     */
    bool writeEncodedPacket(const uint8_t* data, int length);

    /** Mute/unmute playback (outputs silence but keeps consuming data). */
    void setMute(bool muted) { muted_.store(muted, std::memory_order_relaxed); }
    bool isMuted() const { return muted_.load(std::memory_order_relaxed); }

    bool isPlaying() const { return playing_.load(std::memory_order_relaxed); }

    /** Number of decoded PCM frames buffered. */
    int bufferedFrames() const;

    /** Destroy the decoder and release codec resources. */
    void destroyDecoder();

private:
    static void playbackTask(void* param);
    void playbackLoop();

    bool i2sInitialized_ = false;
    std::atomic<bool> playing_{false};
    std::atomic<bool> muted_{false};
    void* taskHandle_ = nullptr;

    Codec2Wrapper* decoder_ = nullptr;
    PacketRingBuffer* pcmRing_ = nullptr;

    // Decode buffer for incoming encoded packets
    int16_t* decodeBuf_ = nullptr;
    int decodeBufSize_ = 0;
    int frameSamples_ = 0;

    // Drop buffer for ring overflow
    int16_t* dropBuf_ = nullptr;

    static constexpr int SAMPLE_RATE = 8000;
    static constexpr int PCM_RING_FRAMES = 16;
    static constexpr int PREBUFFER_FRAMES = 3;
    static constexpr int PLAYBACK_TASK_STACK = 4096;
    static constexpr int PLAYBACK_TASK_PRIORITY = 5;
    static constexpr int PLAYBACK_TASK_CORE = 0;
};
