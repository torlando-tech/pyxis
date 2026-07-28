#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "Telemetry/LocationStateRecord.h"

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

Telemetry::PeerId peer(uint8_t seed) {
    Telemetry::PeerId id{};
    for (std::size_t i = 0; i < Telemetry::PEER_ID_SIZE; ++i) {
        id.bytes[i] = static_cast<uint8_t>(seed + i);
    }
    return id;
}

Telemetry::LocationStateSnapshot sample() {
    Telemetry::LocationStateSnapshot state{};
    state.session_count = 1;
    state.sessions[0].peer = peer(0);
    auto& session = state.sessions[0].record;
    session.cadence_millis = 60000;
    session.approx_radius_meters = 250;
    session.has_expiry = true;
    session.expires_at_millis = 1700000900000ULL;
    session.cease_pending = true;

    state.location_count = 1;
    auto& record = state.locations[0];
    record.peer = peer(16);
    record.location.latitude_e6 = 37774900;
    record.location.longitude_e6 = -122419400;
    record.location.altitude_cm = 12345;
    record.location.speed_centi_kmh = 1234;
    record.location.bearing_cdeg = 27000;
    record.location.accuracy_cm = 500;
    record.location.timestamp_seconds = 1700000000ULL;
    record.location.sensor_timestamp_seconds = 1699999999ULL;
    record.source_timestamp_millis = 1700000000123ULL;
    record.received_at_millis = 1700000001000ULL;
    record.has_expiry = true;
    record.expires_at_millis = 1700000900000ULL;
    record.approx_radius_meters = 250;
    return state;
}

uint32_t crc32(const uint8_t* data, std::size_t size) {
    uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0
                      ? (crc >> 1U) ^ 0xedb88320U
                      : crc >> 1U;
        }
    }
    return crc ^ 0xffffffffU;
}

void writeU16(uint8_t* output, uint16_t value) {
    output[0] = static_cast<uint8_t>(value >> 8U);
    output[1] = static_cast<uint8_t>(value);
}

void writeU32(uint8_t* output, uint32_t value) {
    output[0] = static_cast<uint8_t>(value >> 24U);
    output[1] = static_cast<uint8_t>(value >> 16U);
    output[2] = static_cast<uint8_t>(value >> 8U);
    output[3] = static_cast<uint8_t>(value);
}

void repairCrc(uint8_t* bytes, std::size_t size) {
    writeU32(bytes + size - 4, crc32(bytes, size - 4));
}

bool sameState(
    const Telemetry::LocationStateSnapshot& left,
    const Telemetry::LocationStateSnapshot& right) {
    return std::memcmp(&left, &right, sizeof(left)) == 0;
}

void matchesIndependentGoldenBytes() {
    static const uint8_t expected[] = {
        0x50,0x59,0x4c,0x53,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x78,0x00,0x01,0x00,0x01,
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
        0x03,0x00,0x00,0x00,0xea,0x60,0x00,0x00,0x00,0xfa,0x00,0x00,0x01,0x8b,0xcf,0xf3,
        0x23,0xa0,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,
        0x1e,0x1f,0x01,0x00,0x02,0x40,0x66,0x34,0xf8,0xb4,0x07,0x38,0x00,0x00,0x30,0x39,
        0x00,0x00,0x04,0xd2,0x00,0x00,0x69,0x78,0x01,0xf4,0x00,0x00,0x00,0x00,0x00,0x00,
        0x65,0x53,0xf1,0x00,0x00,0x00,0x00,0x00,0x65,0x53,0xf0,0xff,0x00,0x00,0x01,0x8b,
        0xcf,0xe5,0x68,0x7b,0x00,0x00,0x01,0x8b,0xcf,0xe5,0x6b,0xe8,0x00,0x00,0x01,0x8b,
        0xcf,0xf3,0x23,0xa0,0x00,0x00,0x00,0xfa,0xa4,0x54,0x8e,0x49,
    };
    uint8_t encoded[Telemetry::MAX_LOCATION_STATE_RECORD_BYTES]{};
    std::size_t written = 0;
    CHECK(Telemetry::encodeLocationStateRecord(
              sample(), encoded, sizeof(encoded), written) ==
          Telemetry::LocationStateRecordResult::OK);
    CHECK(written == sizeof(expected));
    CHECK(std::memcmp(encoded, expected, sizeof(expected)) == 0);

    Telemetry::LocationStateSnapshot decoded{};
    CHECK(Telemetry::decodeLocationStateRecord(
              expected, sizeof(expected), decoded) ==
          Telemetry::LocationStateRecordResult::OK);
    CHECK(sameState(decoded, sample()));
}

void rejectsEveryTruncationTransactionally() {
    uint8_t encoded[Telemetry::MAX_LOCATION_STATE_RECORD_BYTES]{};
    std::size_t written = 0;
    CHECK(Telemetry::encodeLocationStateRecord(
              sample(), encoded, sizeof(encoded), written) ==
          Telemetry::LocationStateRecordResult::OK);
    for (std::size_t size = 0; size < written; ++size) {
        Telemetry::LocationStateSnapshot output = sample();
        output.sessions[0].record.cadence_millis = 1234;
        const auto before = output;
        CHECK(Telemetry::decodeLocationStateRecord(encoded, size, output) !=
              Telemetry::LocationStateRecordResult::OK);
        CHECK(sameState(output, before));
    }
}

void rejectsCorruptionAndUnsupportedHeadersTransactionally() {
    uint8_t encoded[Telemetry::MAX_LOCATION_STATE_RECORD_BYTES]{};
    std::size_t written = 0;
    CHECK(Telemetry::encodeLocationStateRecord(
              sample(), encoded, sizeof(encoded), written) ==
          Telemetry::LocationStateRecordResult::OK);
    Telemetry::LocationStateSnapshot sentinel = sample();
    sentinel.location_count = 0;
    const auto before = sentinel;

    encoded[70] ^= 1U;
    CHECK(Telemetry::decodeLocationStateRecord(encoded, written, sentinel) ==
          Telemetry::LocationStateRecordResult::CRC_MISMATCH);
    CHECK(sameState(sentinel, before));
    encoded[70] ^= 1U;

    encoded[0] = 'X';
    repairCrc(encoded, written);
    CHECK(Telemetry::decodeLocationStateRecord(encoded, written, sentinel) ==
          Telemetry::LocationStateRecordResult::BAD_MAGIC);
    CHECK(sameState(sentinel, before));
    encoded[0] = 'P';

    writeU16(encoded + 4, 2);
    repairCrc(encoded, written);
    CHECK(Telemetry::decodeLocationStateRecord(encoded, written, sentinel) ==
          Telemetry::LocationStateRecordResult::UNSUPPORTED_VERSION);
    CHECK(sameState(sentinel, before));
}

void rejectsCountOverflowDuplicatesAndInvalidCoordinates() {
    uint8_t encoded[Telemetry::MAX_LOCATION_STATE_RECORD_BYTES]{};
    std::size_t written = 0;
    CHECK(Telemetry::encodeLocationStateRecord(
              sample(), encoded, sizeof(encoded), written) ==
          Telemetry::LocationStateRecordResult::OK);
    Telemetry::LocationStateSnapshot output{};

    writeU16(encoded + 12, 33);
    repairCrc(encoded, written);
    CHECK(Telemetry::decodeLocationStateRecord(encoded, written, output) ==
          Telemetry::LocationStateRecordResult::OUT_OF_RANGE);

    auto two = sample();
    two.session_count = 2;
    two.sessions[1] = two.sessions[0];
    two.sessions[1].peer = peer(32);
    CHECK(Telemetry::encodeLocationStateRecord(
              two, encoded, sizeof(encoded), written) ==
          Telemetry::LocationStateRecordResult::OK);
    std::memcpy(encoded + Telemetry::LOCATION_STATE_HEADER_BYTES +
                    Telemetry::LOCATION_STATE_SESSION_BYTES,
                encoded + Telemetry::LOCATION_STATE_HEADER_BYTES,
                Telemetry::PEER_ID_SIZE);
    repairCrc(encoded, written);
    CHECK(Telemetry::decodeLocationStateRecord(encoded, written, output) ==
          Telemetry::LocationStateRecordResult::DUPLICATE_PEER);

    two = sample();
    two.location_count = 2;
    two.locations[1] = two.locations[0];
    two.locations[1].peer = peer(48);
    CHECK(Telemetry::encodeLocationStateRecord(
              two, encoded, sizeof(encoded), written) ==
          Telemetry::LocationStateRecordResult::OK);
    const std::size_t first_location =
        Telemetry::LOCATION_STATE_HEADER_BYTES +
        Telemetry::LOCATION_STATE_SESSION_BYTES;
    std::memcpy(encoded + first_location +
                    Telemetry::LOCATION_STATE_LOCATION_BYTES,
                encoded + first_location, Telemetry::PEER_ID_SIZE);
    repairCrc(encoded, written);
    CHECK(Telemetry::decodeLocationStateRecord(encoded, written, output) ==
          Telemetry::LocationStateRecordResult::DUPLICATE_PEER);

    CHECK(Telemetry::encodeLocationStateRecord(
              sample(), encoded, sizeof(encoded), written) ==
          Telemetry::LocationStateRecordResult::OK);
    writeU32(encoded + first_location + 18, 90000001U);
    repairCrc(encoded, written);
    CHECK(Telemetry::decodeLocationStateRecord(encoded, written, output) ==
          Telemetry::LocationStateRecordResult::OUT_OF_RANGE);

    CHECK(Telemetry::encodeLocationStateRecord(
              sample(), encoded, sizeof(encoded), written) ==
          Telemetry::LocationStateRecordResult::OK);
    encoded[Telemetry::LOCATION_STATE_HEADER_BYTES + 16] |= 0x80U;
    repairCrc(encoded, written);
    output = sample();
    output.sessions[0].record.cadence_millis = 12345;
    const auto malformed_before = output;
    CHECK(Telemetry::decodeLocationStateRecord(encoded, written, output) ==
          Telemetry::LocationStateRecordResult::MALFORMED);
    CHECK(sameState(output, malformed_before));

    auto duplicate = sample();
    duplicate.session_count = 2;
    duplicate.sessions[1] = duplicate.sessions[0];
    CHECK(Telemetry::encodeLocationStateRecord(
              duplicate, encoded, sizeof(encoded), written) ==
          Telemetry::LocationStateRecordResult::DUPLICATE_PEER);

    duplicate = sample();
    duplicate.location_count = 2;
    duplicate.locations[1] = duplicate.locations[0];
    CHECK(Telemetry::encodeLocationStateRecord(
              duplicate, encoded, sizeof(encoded), written) ==
          Telemetry::LocationStateRecordResult::DUPLICATE_PEER);

    auto invalid = sample();
    invalid.locations[0].location.latitude_e6 = 90000001;
    CHECK(Telemetry::encodeLocationStateRecord(
              invalid, encoded, sizeof(encoded), written) ==
          Telemetry::LocationStateRecordResult::OUT_OF_RANGE);
}

void encodingIsAtomicAndBounded() {
    uint8_t encoded[Telemetry::MAX_LOCATION_STATE_RECORD_BYTES];
    std::memset(encoded, 0xa5, sizeof(encoded));
    std::size_t required = 0;
    for (std::size_t capacity = 0; capacity < 140; ++capacity) {
        std::memset(encoded, 0xa5, sizeof(encoded));
        CHECK(Telemetry::encodeLocationStateRecord(
                  sample(), encoded, capacity, required) ==
              Telemetry::LocationStateRecordResult::BUFFER_TOO_SMALL);
        CHECK(required == 140);
        CHECK(encoded[0] == 0xa5);
    }

    auto maximum = sample();
    maximum.session_count = Telemetry::MAX_SHARE_SESSIONS;
    maximum.location_count = Telemetry::MAX_PEER_LOCATIONS;
    for (std::size_t i = 0; i < maximum.session_count; ++i) {
        maximum.sessions[i] = maximum.sessions[0];
        maximum.sessions[i].peer = peer(static_cast<uint8_t>(i));
    }
    for (std::size_t i = 0; i < maximum.location_count; ++i) {
        maximum.locations[i] = maximum.locations[0];
        maximum.locations[i].peer = peer(static_cast<uint8_t>(64 + i));
    }
    CHECK(Telemetry::encodeLocationStateRecord(
              maximum, encoded, sizeof(encoded), required) ==
          Telemetry::LocationStateRecordResult::OK);
    CHECK(required <= Telemetry::MAX_LOCATION_STATE_RECORD_BYTES);
    Telemetry::LocationStateSnapshot decoded{};
    CHECK(Telemetry::decodeLocationStateRecord(encoded, required, decoded) ==
          Telemetry::LocationStateRecordResult::OK);
    CHECK(sameState(decoded, maximum));
}

}  // namespace

int main() {
    matchesIndependentGoldenBytes();
    rejectsEveryTruncationTransactionally();
    rejectsCorruptionAndUnsupportedHeadersTransactionally();
    rejectsCountOverflowDuplicatesAndInvalidCoordinates();
    encodingIsAtomicAndBounded();
    std::cout << "location state record: " << passed << " passed, "
              << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
