#pragma once

// Production Pyxis voice policy. LoRa bandwidth is a hard product constraint,
// so only LXST ULBW (Codec2-700C) is accepted or advertised.
struct ULBWVoiceProfilePolicy {
    static constexpr int PROFILE_ULBW = 0x10;
    static constexpr int PREFERRED_PROFILE = 0xFF;
    static constexpr int CODEC2_MODE_700C_VALUE = 8;
    static constexpr int PCM_SAMPLES_PER_PACKET = 3200;  // 400 ms at 8 kHz

    static constexpr bool isSupportedProfile(int profile) {
        return profile == PROFILE_ULBW;
    }

    static constexpr int codecModeForProfile(int profile) {
        return isSupportedProfile(profile) ? CODEC2_MODE_700C_VALUE : -1;
    }

    static constexpr bool acceptsCodec2ModeHeader(unsigned char header) {
        return header == 0x00;
    }

    static constexpr int preferredProfileSignal() {
        return PREFERRED_PROFILE + PROFILE_ULBW;
    }

    static constexpr int pcmSamplesPerPacket() {
        return PCM_SAMPLES_PER_PACKET;
    }

    static constexpr int framesPerPacket(int samplesPerFrame) {
        return samplesPerFrame > 0 &&
                       PCM_SAMPLES_PER_PACKET % samplesPerFrame == 0
                   ? PCM_SAMPLES_PER_PACKET / samplesPerFrame
                   : 0;
    }

    static constexpr int outerBatchBytes(int samplesPerFrame, int bytesPerFrame) {
        return framesPerPacket(samplesPerFrame) > 0 && bytesPerFrame > 0
                   ? 2 + framesPerPacket(samplesPerFrame) * bytesPerFrame
                   : 0;
    }
};
