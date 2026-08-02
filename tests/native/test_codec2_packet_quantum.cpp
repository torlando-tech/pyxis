#include <iostream>

#include "../../lib/lxst_audio/ULBWVoiceProfilePolicy.h"

namespace {
int passed = 0;
int failed = 0;
void expect(bool condition, const char* name) {
    if (condition) ++passed;
    else { ++failed; std::cerr << "FAIL: " << name << '\n'; }
}
}

int main() {
    expect(ULBWVoiceProfilePolicy::isSupportedProfile(0x10), "ULBW supported");
    expect(!ULBWVoiceProfilePolicy::isSupportedProfile(0x20), "VLBW rejected");
    expect(!ULBWVoiceProfilePolicy::isSupportedProfile(0x30), "LBW rejected");
    expect(ULBWVoiceProfilePolicy::codecModeForProfile(0x10) == 8, "ULBW maps to Codec2 700C");
    expect(ULBWVoiceProfilePolicy::codecModeForProfile(0x20) == -1, "VLBW has no codec mapping");
    expect(ULBWVoiceProfilePolicy::pcmSamplesPerPacket() == 3200, "ULBW packet is 400ms at 8kHz");
    expect(ULBWVoiceProfilePolicy::framesPerPacket(320) == 10, "ULBW packet has ten frames");
    expect(ULBWVoiceProfilePolicy::framesPerPacket(0) == 0, "zero frame size rejected");
    expect(ULBWVoiceProfilePolicy::outerBatchBytes(320, 4) == 42, "ULBW outer batch is 42 bytes");
    expect(ULBWVoiceProfilePolicy::acceptsCodec2ModeHeader(0x00), "ULBW media header accepted");
    expect(!ULBWVoiceProfilePolicy::acceptsCodec2ModeHeader(0x04), "VLBW media header rejected");
    expect(!ULBWVoiceProfilePolicy::acceptsCodec2ModeHeader(0x06), "LBW media header rejected");
    expect(ULBWVoiceProfilePolicy::preferredProfileSignal() == 0x10F,
           "profile response always requests ULBW");

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
