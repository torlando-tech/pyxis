#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "Telemetry/LocationTelemetryCodec.h"

namespace {

int passed = 0;
int failures = 0;

#define CHECK(expr)                                                                  \
    do {                                                                             \
        if (expr) {                                                                  \
            ++passed;                                                                \
        } else {                                                                     \
            ++failures;                                                              \
            std::cerr << "FAIL line " << __LINE__ << ": " #expr << '\n';          \
        }                                                                            \
    } while (false)

constexpr uint8_t CANONICAL[] = {
    0x82, 0x01, 0xce, 0x65, 0x53, 0xf1, 0x00, 0x02, 0x97,
    0xc4, 0x04, 0x02, 0x40, 0x66, 0x34,  // 37.774900 degrees
    0xc4, 0x04, 0xf8, 0xb4, 0x07, 0x38,  // -122.419400 degrees
    0xc4, 0x04, 0x00, 0x00, 0x06, 0x40,  // 16.00 m
    0xc4, 0x04, 0x00, 0x00, 0x04, 0xd2,  // 12.34 km/h
    0xc4, 0x04, 0x00, 0x00, 0x10, 0x68,  // 42.00 degrees
    0xc4, 0x02, 0x01, 0x5e,              // 3.50 m
    0xce, 0x65, 0x53, 0xf1, 0x00,
};

Telemetry::LocationTelemetry expectedLocation() {
    Telemetry::LocationTelemetry expected{};
    expected.latitude_e6 = 37774900;
    expected.longitude_e6 = -122419400;
    expected.altitude_cm = 1600;
    expected.speed_centi_kmh = 1234;
    expected.bearing_cdeg = 4200;
    expected.accuracy_cm = 350;
    expected.timestamp_seconds = 1700000000ULL;
    expected.sensor_timestamp_seconds = 1700000000ULL;
    return expected;
}

bool equalLocation(const Telemetry::LocationTelemetry& left,
                   const Telemetry::LocationTelemetry& right) {
    return left.latitude_e6 == right.latitude_e6 &&
           left.longitude_e6 == right.longitude_e6 &&
           left.altitude_cm == right.altitude_cm &&
           left.speed_centi_kmh == right.speed_centi_kmh &&
           left.bearing_cdeg == right.bearing_cdeg &&
           left.accuracy_cm == right.accuracy_cm &&
           left.timestamp_seconds == right.timestamp_seconds &&
           left.sensor_timestamp_seconds == right.sensor_timestamp_seconds;
}

void decodesCanonicalSidebandLocation() {
    Telemetry::LocationTelemetry output{};
    CHECK(Telemetry::decodeLocationTelemetry(
              CANONICAL, sizeof(CANONICAL), output) ==
          Telemetry::DecodeResult::OK);
    CHECK(equalLocation(output, expectedLocation()));
}

void emitsCanonicalSidebandLocation() {
    uint8_t encoded[128]{};
    std::size_t written = 99;
    CHECK(Telemetry::encodeLocationTelemetry(
              expectedLocation(), encoded, sizeof(encoded), written) ==
          Telemetry::EncodeResult::OK);
    CHECK(written == sizeof(CANONICAL));
    CHECK(std::memcmp(encoded, CANONICAL, sizeof(CANONICAL)) == 0);
}

void unwrapsAndWrapsCurrentMicroLxmfFieldValue() {
    uint8_t field_value[sizeof(CANONICAL) + 2]{};
    field_value[0] = 0xc4;  // MessagePack bin8
    field_value[1] = static_cast<uint8_t>(sizeof(CANONICAL));
    std::memcpy(field_value + 2, CANONICAL, sizeof(CANONICAL));

    Telemetry::BinaryView inner{};
    CHECK(Telemetry::unwrapLxmfBinaryFieldValue(
              field_value, sizeof(field_value), inner) ==
          Telemetry::FieldValueResult::OK);
    CHECK(inner.data == field_value + 2);
    CHECK(inner.size == sizeof(CANONICAL));

    uint8_t wrapped[128]{};
    std::size_t written = 99;
    CHECK(Telemetry::wrapLxmfBinaryFieldValue(
              CANONICAL, sizeof(CANONICAL), wrapped, sizeof(wrapped), written) ==
          Telemetry::FieldValueResult::OK);
    CHECK(written == sizeof(field_value));
    CHECK(std::memcmp(wrapped, field_value, sizeof(field_value)) == 0);
}

void preservesOutputOnEveryTruncation() {
    const auto sentinel = [] {
        auto value = expectedLocation();
        value.latitude_e6 = 123;
        return value;
    }();

    for (std::size_t size = 0; size < sizeof(CANONICAL); ++size) {
        auto output = sentinel;
        CHECK(Telemetry::decodeLocationTelemetry(CANONICAL, size, output) !=
              Telemetry::DecodeResult::OK);
        CHECK(equalLocation(output, sentinel));
    }
}

void acceptsUnknownNestedSensorAndReorderedKeys() {
    // {32: [1, {"x": true}], 2: location, 1: time}. Sideband ignores unknown
    // sensor IDs and dictionary order is not semantically significant.
    constexpr uint8_t packed[] = {
        0x83,
        0x20, 0x92, 0x01, 0x81, 0xa1, 0x78, 0xc3,
        0x02, 0x97,
        0xc4, 0x04, 0x02, 0x40, 0x66, 0x34,
        0xc4, 0x04, 0xf8, 0xb4, 0x07, 0x38,
        0xc4, 0x04, 0x00, 0x00, 0x06, 0x40,
        0xc4, 0x04, 0x00, 0x00, 0x04, 0xd2,
        0xc4, 0x04, 0x00, 0x00, 0x10, 0x68,
        0xc4, 0x02, 0x01, 0x5e,
        0xce, 0x65, 0x53, 0xf1, 0x00,
        0x01, 0xce, 0x65, 0x53, 0xf1, 0x00,
    };
    Telemetry::LocationTelemetry output{};
    CHECK(Telemetry::decodeLocationTelemetry(packed, sizeof(packed), output) ==
          Telemetry::DecodeResult::OK);
    CHECK(equalLocation(output, expectedLocation()));
}

void rejectsMalformedOuterFieldWithoutMutatingView() {
    constexpr uint8_t not_binary[] = {0x81, 0x01, 0x02};
    constexpr uint8_t truncated[] = {0xc4, 0x04, 0x01, 0x02};
    const uint8_t sentinel_byte = 0;
    const Telemetry::BinaryView sentinel{&sentinel_byte, 77};

    auto output = sentinel;
    CHECK(Telemetry::unwrapLxmfBinaryFieldValue(
              not_binary, sizeof(not_binary), output) ==
          Telemetry::FieldValueResult::NOT_BINARY);
    CHECK(output.data == sentinel.data && output.size == sentinel.size);

    output = sentinel;
    CHECK(Telemetry::unwrapLxmfBinaryFieldValue(
              truncated, sizeof(truncated), output) ==
          Telemetry::FieldValueResult::MALFORMED);
    CHECK(output.data == sentinel.data && output.size == sentinel.size);
}

}  // namespace

int main() {
    decodesCanonicalSidebandLocation();
    emitsCanonicalSidebandLocation();
    unwrapsAndWrapsCurrentMicroLxmfFieldValue();
    preservesOutputOnEveryTruncation();
    acceptsUnknownNestedSensorAndReorderedKeys();
    rejectsMalformedOuterFieldWithoutMutatingView();
    std::cout << "location telemetry codec: " << passed << " passed, "
              << failures << " failed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
