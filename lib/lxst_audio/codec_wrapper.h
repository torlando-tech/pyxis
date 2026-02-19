// Copyright (c) 2024 LXST contributors
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>

struct CODEC2;

/**
 * Codec2 wrapper for LXST voice streaming on ESP32.
 *
 * Ported from LXST-kt CodecWrapper (codec_wrapper.h/cpp), stripped to
 * Codec2-only (no Opus) for the ESP32-S3 resource budget.
 *
 * Handles the LXST wire format:
 *   Encoded packet = [1-byte mode header] + [N codec2 sub-frames]
 *
 * Wire-compatible with LXST-kt (Android/Columba) and Python LXST.
 *
 * Mode header mapping (matches Codec2.kt and Python LXST):
 *   0x00 = 700C  (lib mode 8)  - 700bps,  40ms frames, 7 bytes/frame
 *   0x04 = 1600  (lib mode 2)  - 1600bps, 20ms frames, 8 bytes/frame
 *   0x06 = 3200  (lib mode 0)  - 3200bps, 20ms frames, 8 bytes/frame
 */
class Codec2Wrapper {
public:
    Codec2Wrapper();
    ~Codec2Wrapper();

    Codec2Wrapper(const Codec2Wrapper&) = delete;
    Codec2Wrapper& operator=(const Codec2Wrapper&) = delete;

    /**
     * Create a Codec2 encoder+decoder instance.
     * @param libraryMode Codec2 library mode (0=3200, 2=1600, 8=700C)
     * @return true on success
     */
    bool create(int libraryMode);

    /** Destroy the codec and release all resources. */
    void destroy();

    /**
     * Decode encoded bytes to PCM int16.
     * Strips mode header byte, loops over sub-frames.
     * Handles dynamic mode switching if header changes.
     *
     * @param encoded         Encoded data (with mode header byte)
     * @param encodedBytes    Length of encoded data
     * @param output          Output PCM int16 buffer
     * @param maxOutputSamples Maximum samples that fit in output buffer
     * @return Decoded sample count, or -1 on error
     */
    int decode(const uint8_t* encoded, int encodedBytes,
               int16_t* output, int maxOutputSamples);

    /**
     * Encode PCM int16 to encoded bytes.
     * Prepends mode header byte, loops over sub-frames.
     *
     * @param pcm             Input PCM int16 samples (8kHz mono)
     * @param pcmSamples      Number of input samples
     * @param output          Output buffer for encoded data
     * @param maxOutputBytes  Maximum bytes that fit in output buffer
     * @return Encoded byte count, or -1 on error
     */
    int encode(const int16_t* pcm, int pcmSamples,
               uint8_t* output, int maxOutputBytes);

    bool isCreated() const { return codec2_ != nullptr; }
    int samplesPerFrame() const { return samplesPerFrame_; }
    int bytesPerFrame() const { return bytesPerFrame_; }
    uint8_t modeHeader() const { return modeHeader_; }
    int libraryMode() const { return libraryMode_; }

private:
    struct CODEC2* codec2_ = nullptr;
    int samplesPerFrame_ = 0;
    int bytesPerFrame_ = 0;
    uint8_t modeHeader_ = 0;
    int libraryMode_ = 0;

    static int headerToLibraryMode(uint8_t header);
    static uint8_t libraryModeToHeader(int libraryMode);
};
