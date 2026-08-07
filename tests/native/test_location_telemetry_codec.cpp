#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#include "Telemetry/LocationTelemetryCodec.h"
#include "location_telemetry_vectors_generated.h"

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

void validatesEveryCommittedSidebandVector() {
    for (std::size_t index = 0; index < Fixture::VECTOR_COUNT; ++index) {
        const auto& vector = Fixture::VECTORS[index];
        Telemetry::LocationTelemetry expected{};
        expected.latitude_e6 = vector.latitude_e6;
        expected.longitude_e6 = vector.longitude_e6;
        expected.altitude_cm = vector.altitude_cm;
        expected.speed_centi_kmh = vector.speed_centi_kmh;
        expected.bearing_cdeg = vector.bearing_cdeg;
        expected.accuracy_cm = vector.accuracy_cm;
        expected.timestamp_seconds = vector.location_timestamp_seconds;
        expected.sensor_timestamp_seconds = vector.sensor_timestamp_seconds;

        Telemetry::LocationTelemetry decoded{};
        CHECK(Telemetry::decodeLocationTelemetry(
                  vector.packed, vector.packed_size, decoded) ==
              Telemetry::DecodeResult::OK);
        CHECK(equalLocation(decoded, expected));

        uint8_t encoded[128]{};
        std::size_t written = 0;
        CHECK(Telemetry::encodeLocationTelemetry(
                  expected, encoded, sizeof(encoded), written) ==
              Telemetry::EncodeResult::OK);
        CHECK(written == vector.packed_size);
        CHECK(std::memcmp(encoded, vector.packed, written) == 0);
    }
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

void coversEveryBinaryHeaderBoundaryAndOverlap() {
    const std::size_t sizes[] = {255, 256, 65535, 65536};
    const uint8_t markers[] = {0xc4, 0xc5, 0xc5, 0xc6};
    const std::size_t header_sizes[] = {2, 3, 3, 5};
    for (std::size_t test = 0; test < 4; ++test) {
        std::vector<uint8_t> payload(sizes[test]);
        for (std::size_t index = 0; index < payload.size(); ++index) {
            payload[index] = static_cast<uint8_t>(index);
        }
        std::vector<uint8_t> wrapped(payload.size() + 5);
        std::size_t written = 0;
        CHECK(Telemetry::wrapLxmfBinaryFieldValue(
                  payload.data(), payload.size(), wrapped.data(), wrapped.size(),
                  written) == Telemetry::FieldValueResult::OK);
        CHECK(wrapped[0] == markers[test]);
        CHECK(written == payload.size() + header_sizes[test]);

        Telemetry::BinaryView view{};
        CHECK(Telemetry::unwrapLxmfBinaryFieldValue(
                  wrapped.data(), written, view) ==
              Telemetry::FieldValueResult::OK);
        CHECK(view.size == payload.size());
        CHECK(std::memcmp(view.data, payload.data(), payload.size()) == 0);
    }

    // An in-place prepend is a natural constrained-memory operation. The
    // payload and destination overlap and must remain defined.
    std::vector<uint8_t> in_place(sizeof(CANONICAL) + 5);
    std::memcpy(in_place.data() + 5, CANONICAL, sizeof(CANONICAL));
    std::size_t written = 0;
    CHECK(Telemetry::wrapLxmfBinaryFieldValue(
              in_place.data() + 5, sizeof(CANONICAL), in_place.data(),
              in_place.size(), written) == Telemetry::FieldValueResult::OK);
    CHECK(written == sizeof(CANONICAL) + 2);
    CHECK(std::memcmp(in_place.data() + 2, CANONICAL, sizeof(CANONICAL)) == 0);
}

void rejectsEveryBoundedParserLimitAndOversizedLength() {
    constexpr uint8_t too_many_map_entries[] = {0xde, 0x00, 0x21};
    constexpr uint8_t too_many_location_elements[] = {
        0x81, 0x02, 0xdc, 0x00, 0x11,
    };
    constexpr uint8_t excessive_depth[] = {
        0x82, 0x01, 0x00, 0x20,
        0x91, 0x91, 0x91, 0x91, 0x91,
        0x91, 0x91, 0x91, 0x91, 0x91, 0xc0,
    };
    std::vector<uint8_t> excessive_items = {
        0x82, 0x01, 0x00, 0x20, 0xdc, 0x00, 0x41,
    };
    excessive_items.insert(excessive_items.end(), 65, 0xc0);
    constexpr uint8_t oversized_binary[] = {
        0x81, 0x02, 0x97, 0xc6, 0xff, 0xff, 0xff, 0xff,
    };
    std::vector<uint8_t> trailing(CANONICAL, CANONICAL + sizeof(CANONICAL));
    trailing.push_back(0xc0);

    struct Case {
        const uint8_t* data;
        std::size_t size;
    };
    const Case cases[] = {
        {too_many_map_entries, sizeof(too_many_map_entries)},
        {too_many_location_elements, sizeof(too_many_location_elements)},
        {excessive_depth, sizeof(excessive_depth)},
        {excessive_items.data(), excessive_items.size()},
        {oversized_binary, sizeof(oversized_binary)},
        {trailing.data(), trailing.size()},
    };
    const auto sentinel = expectedLocation();
    for (const auto& item : cases) {
        auto output = sentinel;
        CHECK(Telemetry::decodeLocationTelemetry(item.data, item.size, output) !=
              Telemetry::DecodeResult::OK);
        CHECK(equalLocation(output, sentinel));
    }
}

}  // namespace

int main() {
    validatesEveryCommittedSidebandVector();
    decodesCanonicalSidebandLocation();
    emitsCanonicalSidebandLocation();
    unwrapsAndWrapsCurrentMicroLxmfFieldValue();
    preservesOutputOnEveryTruncation();
    acceptsUnknownNestedSensorAndReorderedKeys();
    rejectsMalformedOuterFieldWithoutMutatingView();
    coversEveryBinaryHeaderBoundaryAndOverlap();
    rejectsEveryBoundedParserLimitAndOversizedLength();
    std::cout << "location telemetry codec: " << passed << " passed, "
              << failures << " failed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
