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
     * Configure the decoder with a shared Codec2 instance.
     * @param codec Shared Codec2Wrapper (not owned — caller manages lifecycle)
     * @return true on success
     */
    bool configureDecoder(Codec2Wrapper* codec);

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

    /**
     * Decode counters for QoS validation. Each writeEncodedPacket call
     * increments exactly one: decodeOk on a successful Codec2 decode,
     * decodeFail otherwise. Together they're the wire-level audio
     * fidelity metric — a peer sending malformed/corrupted Codec2
     * frames shows up as a high decodeFail rate. Reset on resetCounters().
     */
    uint32_t decodeOkCount() const { return decodeOkCount_.load(std::memory_order_relaxed); }
    uint32_t decodeFailCount() const { return decodeFailCount_.load(std::memory_order_relaxed); }

    /**
     * PCM energy on the decoded audio. pcmSampleCount() = total int16
     * samples produced by the decoder; pcmSumSquares() = sum of each
     * sample squared (uint64). The harness divides + sqrts to get
     * RMS. The peer is expected to send a 1kHz sine via its
     * setInjectSine path; pyxis's RMS should match the expected sine
     * energy ≈ peak / sqrt(2). For peak=16384 expected RMS ≈ 11585.
     */
    uint32_t pcmSampleCount() const { return pcmSampleCount_.load(std::memory_order_relaxed); }
    uint64_t pcmSumSquares() const { return pcmSumSquares_.load(std::memory_order_relaxed); }

    void resetCounters() {
        decodeOkCount_.store(0, std::memory_order_relaxed);
        decodeFailCount_.store(0, std::memory_order_relaxed);
        pcmSampleCount_.store(0, std::memory_order_relaxed);
        pcmSumSquares_.store(0, std::memory_order_relaxed);
    }

    /** Release playback buffers (does NOT destroy the shared codec). */
    void releaseBuffers();

private:
    static void playbackTask(void* param);
    void playbackLoop();

    bool i2sInitialized_ = false;
    std::atomic<bool> playing_{false};
    std::atomic<bool> muted_{false};
    void* taskHandle_ = nullptr;

    Codec2Wrapper* codec_ = nullptr;  // Shared, not owned
    PacketRingBuffer* pcmRing_ = nullptr;

    // QoS counters incremented on each writeEncodedPacket call.
    std::atomic<uint32_t> decodeOkCount_{0};
    std::atomic<uint32_t> decodeFailCount_{0};
    // PCM energy accumulators — fed from the decoded buffer after
    // each successful decode. Used by the harness to compute RMS.
    std::atomic<uint32_t> pcmSampleCount_{0};
    std::atomic<uint64_t> pcmSumSquares_{0};

    // Decode buffer for incoming encoded packets
    int16_t* decodeBuf_ = nullptr;
    int decodeBufSize_ = 0;
    int frameSamples_ = 0;

    // Drop buffer for ring overflow
    int16_t* dropBuf_ = nullptr;

    static constexpr int SAMPLE_RATE = 8000;
    static constexpr int PCM_RING_FRAMES = 50;
    static constexpr int PREBUFFER_FRAMES = 15;
    static constexpr int PLAYBACK_TASK_STACK = 8192;
    static constexpr int PLAYBACK_TASK_PRIORITY = 5;
    static constexpr int PLAYBACK_TASK_CORE = 0;
};
