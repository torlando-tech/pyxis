#include <cstddef>
#include <cstdint>
#include <iostream>

#include "../../lib/lxst_audio/LXSTSignalParser.h"

namespace {
int passed = 0;
int failed = 0;
void expect(bool condition, const char* name) {
    if (condition) ++passed;
    else { ++failed; std::cerr << "FAIL: " << name << '\n'; }
}
}

int main() {
    int signals[LXSTSignalParser::MAX_SIGNALS] = {};
    expect(LXSTSignalParser::MIN_SENTINEL_QUEUE_SIZE ==
               LXSTSignalParser::MAX_SIGNALS + 1,
           "sentinel queue can hold one maximum signalling array");

    const uint8_t legacy[] = {0x81, 0x00, 0x91, 0xCD, 0x01, 0x0F};
    size_t count = LXSTSignalParser::parse(legacy, sizeof(legacy), signals,
                                           LXSTSignalParser::MAX_SIGNALS);
    expect(count == 1, "legacy one-signal packet accepted");
    expect(signals[0] == 0x10F, "legacy ULBW profile decoded");

    // Authoritative LXST 0.5.1 caller vector:
    // {FIELD_SIGNALLING: [PREFERRED_PROFILE+MQ, PREFERRED_MODE+FDX]}
    const uint8_t current[] = {
        0x81, 0x00, 0x92, 0xCD, 0x01, 0x3F, 0xCC, 0xF1
    };
    count = LXSTSignalParser::parse(current, sizeof(current), signals,
                                    LXSTSignalParser::MAX_SIGNALS);
    expect(count == 2, "current LXST profile+mode packet accepted");
    expect(signals[0] == 0x13F, "MQ profile decoded");
    expect(signals[1] == 0xF1, "full-duplex mode decoded");

    const uint8_t array16[] = {
        0x81, 0x00, 0xDC, 0x00, 0x02, 0xCC, 0xF1, 0xCD, 0x01, 0x0F
    };
    count = LXSTSignalParser::parse(array16, sizeof(array16), signals,
                                    LXSTSignalParser::MAX_SIGNALS);
    expect(count == 2, "array16 signalling accepted");
    expect(signals[0] == 0xF1 && signals[1] == 0x10F,
           "array16 values decoded in order");

    const uint8_t truncated[] = {0x81, 0x00, 0x92, 0xCD, 0x01, 0x3F, 0xCC};
    expect(LXSTSignalParser::parse(truncated, sizeof(truncated), signals,
                                   LXSTSignalParser::MAX_SIGNALS) == 0,
           "truncated list rejected atomically");

    const uint8_t noncanonical_uint32[] = {0x81, 0x00, 0x91, 0xCE, 0, 0, 1, 0};
    count = LXSTSignalParser::parse(noncanonical_uint32,
                                    sizeof(noncanonical_uint32), signals,
                                    LXSTSignalParser::MAX_SIGNALS);
    expect(count == 1, "noncanonical uint32 signal accepted");
    expect(signals[0] == 0x100, "noncanonical uint32 value decoded");

    const uint8_t noncanonical_int16[] = {0x81, 0x00, 0x91, 0xD1, 0x01, 0x0F};
    count = LXSTSignalParser::parse(noncanonical_int16,
                                    sizeof(noncanonical_int16), signals,
                                    LXSTSignalParser::MAX_SIGNALS);
    expect(count == 1, "noncanonical positive int16 signal accepted");
    expect(signals[0] == 0x10F, "noncanonical positive int16 decoded");

    expect(LXSTSignalParser::parse(current, sizeof(current), signals, 1) == 0,
           "caller capacity is enforced");

    const uint8_t too_many[] = {
        0x81, 0x00, 0x99, 0, 1, 2, 3, 4, 5, 6, 7, 8
    };
    expect(LXSTSignalParser::parse(too_many, sizeof(too_many), signals,
                                   LXSTSignalParser::MAX_SIGNALS) == 0,
           "peer-controlled signal count is bounded");

    const uint8_t trailing[] = {0x81, 0x00, 0x91, 0x04, 0x00};
    expect(LXSTSignalParser::parse(trailing, sizeof(trailing), signals,
                                   LXSTSignalParser::MAX_SIGNALS) == 0,
           "trailing bytes rejected");

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
