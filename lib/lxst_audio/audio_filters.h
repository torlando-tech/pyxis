// Copyright (c) 2024 LXST contributors
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>

/**
 * Native voice filter chain for LXST audio capture.
 *
 * Ported from LXST-kt native layer (native_audio_filters.h/cpp).
 * Filter order: HighPass (300Hz) -> LowPass (3400Hz) -> AGC
 *
 * Processes int16 samples in-place. Internally converts to float
 * for filter math and back to int16 on output.
 */
class VoiceFilterChain {
public:
    /**
     * @param channels    Number of audio channels (1=mono for Codec2)
     * @param hpCutoff    High-pass cutoff frequency (Hz), typically 300
     * @param lpCutoff    Low-pass cutoff frequency (Hz), typically 3400
     * @param agcTargetDb AGC target level in dBFS, typically -12
     * @param agcMaxGain  AGC maximum gain in dB, typically 12
     */
    VoiceFilterChain(int channels, float hpCutoff, float lpCutoff,
                     float agcTargetDb, float agcMaxGain);
    ~VoiceFilterChain();

    VoiceFilterChain(const VoiceFilterChain&) = delete;
    VoiceFilterChain& operator=(const VoiceFilterChain&) = delete;

    /**
     * Process audio samples through the filter chain (in-place).
     *
     * @param samples    int16 PCM samples (modified in-place)
     * @param numSamples Total number of samples (frames * channels)
     * @param sampleRate Sample rate in Hz
     */
    void process(int16_t* samples, int numSamples, int sampleRate);

private:
    struct HighPassState {
        float* filterStates;
        float* lastInputs;
        float alpha = 0;
        int sampleRate = 0;
    };

    struct LowPassState {
        float* filterStates;
        float alpha = 0;
        int sampleRate = 0;
    };

    struct AGCState {
        float* currentGain;
        int holdCounter = 0;
        int sampleRate = 0;
        float attackCoeff = 0;
        float releaseCoeff = 0;
        int holdSamples = 0;
    };

    void applyHighPass(float* samples, int numFrames);
    void applyLowPass(float* samples, int numFrames);
    void applyAGC(float* samples, int numFrames);

    int channels_;
    float hpCutoff_;
    float lpCutoff_;
    float agcTargetDb_;
    float agcMaxGain_;

    HighPassState hp_;
    LowPassState lp_;
    AGCState agc_;

    float* workBuffer_;
    int workBufferSize_ = 0;
};
