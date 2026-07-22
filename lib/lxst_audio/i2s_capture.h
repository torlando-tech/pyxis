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

    /**
     * Test injection: replace mic input with a synthesized 1kHz sine
     * wave. Bypasses both ES7210 capture and the voice filter chain
     * so the encoder sees pure samples. Used by the LXST harness to
     * validate audio quality across the call (peer's decoded RMS
     * matches expected sine energy).
     *
     * @param enabled True to inject, false to use mic
     * @param freq    Sine frequency in Hz (default 1000)
     * @param amp     Amplitude as fraction of int16 max (0.0–1.0,
     *                default 0.5 → ~16384 peak)
     */
    void setInjectSine(bool enabled, int freq = 1000, float amp = 0.5f) {
        injectFreq_.store(freq, std::memory_order_relaxed);
        int16_t peak = (int16_t)(32767.0f * (amp < 0.f ? 0.f : (amp > 1.f ? 1.f : amp)));
        injectPeak_.store(peak, std::memory_order_relaxed);
        injectSine_.store(enabled, std::memory_order_relaxed);
    }
    bool isInjectingSine() const { return injectSine_.load(std::memory_order_relaxed); }

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
    void* taskExited_ = nullptr;  // FreeRTOS binary semaphore; task signals before delete

    // Test injection (see setInjectSine). When injectSine_ is true,
    // the capture path replaces accumulated mic samples with a
    // synthetic speech-like signal: F1 sine at injectFreq_ Hz +
    // F2 at 1.5·F1 + F3 at 3.3·F1, modulated by a 120 Hz
    // amplitude envelope. Each formant has its own phase tracker
    // so the encoder never sees a discontinuity. The signal
    // approximates a voiced vowel — Codec2 retains far more RMS
    // on this than on a pure tone.
    std::atomic<bool> injectSine_{false};
    std::atomic<int> injectFreq_{730};        // F1 default ≈ "a" formant
    std::atomic<int16_t> injectPeak_{16384};
    float injectPhase_     = 0.0f;             // F1 phase
    float injectPhase2_    = 0.0f;             // F2 phase
    float injectPhase3_    = 0.0f;             // F3 phase
    float injectEnvPhase_  = 0.0f;             // 120Hz amplitude env phase

    // Audio pipeline components
    Codec2Wrapper* codec_ = nullptr;  // Shared, not owned
    VoiceFilterChain* filterChain_ = nullptr;
    // SPSC queue of filtered PCM batches. The capture task only performs I2S
    // and filtering; loopTask performs Codec2 encoding and transmission.
    EncodedRingBuffer* encodedRing_ = nullptr;

    // Accumulation buffer: I2S delivers variable bursts, we need fixed-size frames
    int16_t* accumBuffer_ = nullptr;
    int accumCount_ = 0;
    int frameSamples_ = 0;  // Codec2 samples per frame (e.g., 320 for 700C, 160 for 1600/3200)

    // PSRAM staging buffer used by loopTask while encoding queued PCM.
    int16_t* encodePcmBuffer_ = nullptr;

    // Silence buffer for mute
    int16_t* silenceBuf_ = nullptr;

    bool filtersEnabled_ = true;

    static constexpr int I2S_SAMPLE_RATE = 16000;  // EXACT-LilyGO test: 16kHz capture, decimated 2:1 to 8kHz. (ES7210 ADC warp unresolved -- see es7210.cpp.)
    static constexpr int CODEC_SAMPLE_RATE = 8000; // Codec2 expects 8kHz
    // Accumulate this many codec frames before filter+encode.
    // Matches Columba's 200ms batch (1600 samples for Codec2 3200).
    // The AGC needs large blocks for stable gain tracking.
    static constexpr int FRAMES_PER_BATCH = 10;
    static constexpr int PCM_RING_SLOTS = 8;
    // Codec2 no longer runs on this task. 8 KiB covers local I2S buffers,
    // filters and bounded diagnostic sendto without starving playback.
    static constexpr int CAPTURE_TASK_STACK = 8192;
    static constexpr int CAPTURE_TASK_PRIORITY = 5;
    static constexpr int CAPTURE_TASK_CORE = 0;
};
