#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "Telemetry/LocationTelemetryCodec.h"

namespace {

int failures = 0;

#define CHECK(expr)                                                                  \
    do {                                                                             \
        if (!(expr)) {                                                               \
            ++failures;                                                              \
            std::cerr << "FAIL line " << __LINE__ << ": " #expr << '\n';          \
        }                                                                            \
    } while (false)

void decodes_canonical_sideband_location() {
    // MessagePack: {SID_TIME: 1700000000, SID_LOCATION: [fixed-width fields...]}
    const uint8_t packed[] = {
        0x82, 0x01, 0xce, 0x65, 0x53, 0xf1, 0x00, 0x02, 0x97,
        0xc4, 0x04, 0x02, 0x40, 0x66, 0x34,  // 37.774900 degrees
        0xc4, 0x04, 0xf8, 0xb4, 0x07, 0x38,  // -122.419400 degrees
        0xc4, 0x04, 0x00, 0x00, 0x06, 0x40,  // 16.00 m
        0xc4, 0x04, 0x00, 0x00, 0x00, 0x00,  // 0.00 m/s
        0xc4, 0x04, 0x00, 0x00, 0x10, 0x68,  // 42.00 degrees
        0xc4, 0x02, 0x01, 0x5e,              // 3.50 m
        0xce, 0x65, 0x53, 0xf1, 0x00,
    };

    Telemetry::LocationTelemetry output{};
    const auto result = Telemetry::decodeLocationTelemetry(
        packed, sizeof(packed), output);

    CHECK(result == Telemetry::DecodeResult::OK);
    CHECK(output.latitude_e6 == 37774900);
    CHECK(output.longitude_e6 == -122419400);
    CHECK(output.altitude_cm == 1600);
    CHECK(output.speed_cms == 0);
    CHECK(output.bearing_cdeg == 4200);
    CHECK(output.accuracy_cm == 350);
    CHECK(output.timestamp_seconds == 1700000000ULL);
}

}  // namespace

int main() {
    decodes_canonical_sideband_location();
    if (failures == 0) {
        std::cout << "location telemetry codec: 7 passed, 0 failed\n";
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
