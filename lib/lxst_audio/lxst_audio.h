// Copyright (c) 2024 LXST contributors
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <functional>

class I2SCapture;
class I2SPlayback;
class Codec2Wrapper;

/**
 * LXST Audio Pipeline Controller for ESP32-S3 T-Deck Plus.
 *
 * Top-level API that coordinates:
 *   - ES7210 microphone initialization
 *   - I2S capture (mic -> encode -> ring buffer)
 *   - I2S playback (ring buffer -> decode -> speaker)
 *   - Codec2 codec lifecycle
 *   - Tone.cpp coexistence (releases I2S_NUM_0 for tones when idle)
 *
 * Supports half-duplex (push-to-talk) and full-duplex modes.
 * I2S_NUM_0 (speaker) and I2S_NUM_1 (mic) are independent peripherals.
 *
 * Usage (full-duplex voice call):
 *   LXSTAudio audio;
 *   audio.init(CODEC2_MODE_1600);
 *   audio.startFullDuplex();   // Both mic + speaker active
 *
 *   // TX: read encoded mic data and send over network
 *   if (audio.readEncodedPacket(buf, sizeof(buf), &len)) { ... }
 *
 *   // RX: write received encoded data for speaker playback
 *   audio.writeEncodedPacket(data, len);
 *
 *   audio.stopCapture();
 *   audio.stopPlayback();
 */

// Codec2 library mode constants (from codec2.h)
#ifndef CODEC2_MODE_3200
#define CODEC2_MODE_3200  0
#define CODEC2_MODE_2400  1
#define CODEC2_MODE_1600  2
#define CODEC2_MODE_700C  8
#endif

class LXSTAudio {
public:
    enum class State {
        IDLE,         // No audio activity
        CAPTURING,    // Microphone active, encoding
        PLAYING,      // Speaker active, decoding
        FULL_DUPLEX,  // Both mic + speaker active
    };

    LXSTAudio();
    ~LXSTAudio();

    LXSTAudio(const LXSTAudio&) = delete;
    LXSTAudio& operator=(const LXSTAudio&) = delete;

    /**
     * Initialize the audio pipeline.
     * Sets up ES7210 mic array and configures codec.
     * Does NOT start capture or playback.
     *
     * @param codec2Mode  Codec2 library mode (default 1600)
     * @param micGain     ES7210 mic gain (0-14, default 8 = 24dB)
     * @return true on success
     */
    bool init(int codec2Mode = CODEC2_MODE_1600, uint8_t micGain = 5);

    /** Tear down everything and release all resources. */
    void deinit();

    /**
     * Start microphone capture only (TX).
     * In half-duplex mode, stops playback if active.
     * @return true on success
     */
    bool startCapture();

    /** Stop microphone capture. */
    void stopCapture();

    /**
     * Start speaker playback only (RX).
     * In half-duplex mode, stops capture if active.
     * Tone.cpp must not be playing.
     * @return true on success
     */
    bool startPlayback();

    /** Stop speaker playback. Releases I2S_NUM_0 for tone generator. */
    void stopPlayback();

    /**
     * Start full-duplex audio (both mic capture + speaker playback).
     * Uses I2S_NUM_1 for mic and I2S_NUM_0 for speaker simultaneously.
     * @return true on success
     */
    bool startFullDuplex();

    /**
     * Read the next encoded packet from the capture pipeline.
     * Called by the network layer during TX.
     *
     * @param dest        Output buffer
     * @param maxLength   Buffer size
     * @param actualLength [out] Actual packet size
     * @return true if a packet was read
     */
    bool readEncodedPacket(uint8_t* dest, int maxLength, int* actualLength);

    /**
     * Write an encoded packet into the playback pipeline.
     * Called by the network layer during RX.
     *
     * @param data   Encoded packet (with LXST mode header)
     * @param length Packet length
     * @return true on success
     */
    bool writeEncodedPacket(const uint8_t* data, int length);

    /** Mute/unmute the microphone (sends silence). */
    void setCaptureMute(bool muted);

    /** Mute/unmute the speaker. */
    void setPlaybackMute(bool muted);

    /** Current pipeline state. */
    State state() const { return state_; }

    /** Whether capture is active (CAPTURING or FULL_DUPLEX). */
    bool isCapturing() const { return state_ == State::CAPTURING || state_ == State::FULL_DUPLEX; }

    /** Whether playback is active (PLAYING or FULL_DUPLEX). */
    bool isPlaying() const { return state_ == State::PLAYING || state_ == State::FULL_DUPLEX; }

    /** Whether init() has been called successfully. */
    bool isInitialized() const { return initialized_; }

    /** Number of encoded packets available from capture. */
    int capturePacketsAvailable() const;

    /** Number of decoded frames buffered for playback. */
    int playbackFramesBuffered() const;

private:
    I2SCapture* capture_ = nullptr;
    I2SPlayback* playback_ = nullptr;
    Codec2Wrapper* encodeCodec_ = nullptr;  // Encoder codec (capture task)
    Codec2Wrapper* decodeCodec_ = nullptr;  // Decoder codec (main thread)
    State state_ = State::IDLE;
    bool initialized_ = false;
    int codec2Mode_ = CODEC2_MODE_1600;
};
