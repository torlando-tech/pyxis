#include "LocationStateRecord.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace Telemetry {
namespace {

constexpr uint8_t MAGIC[4] = {'P', 'Y', 'L', 'S'};
constexpr uint8_t SESSION_HAS_EXPIRY = 0x01;
constexpr uint8_t SESSION_CEASE_PENDING = 0x02;
constexpr uint8_t SESSION_HAS_APPROX_RADIUS = 0x04;
constexpr uint8_t LOCATION_HAS_EXPIRY = 0x01;
constexpr uint8_t LOCATION_HAS_APPROX_RADIUS = 0x02;

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8U) |
        static_cast<uint16_t>(data[1]));
}

uint32_t readU32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24U) |
           (static_cast<uint32_t>(data[1]) << 16U) |
           (static_cast<uint32_t>(data[2]) << 8U) |
           static_cast<uint32_t>(data[3]);
}

uint64_t readU64(const uint8_t* data) {
    uint64_t value = 0;
    for (uint8_t index = 0; index < 8; ++index) {
        value = (value << 8U) | data[index];
    }
    return value;
}

int32_t readI32(const uint8_t* data) {
    const uint32_t raw = readU32(data);
    if (raw <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return static_cast<int32_t>(raw);
    }
    return -1 - static_cast<int32_t>(
                    std::numeric_limits<uint32_t>::max() - raw);
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

void writeI32(uint8_t* output, int32_t value) {
    writeU32(output, static_cast<uint32_t>(value));
}

void writeU64(uint8_t* output, uint64_t value) {
    for (int index = 7; index >= 0; --index) {
        output[index] = static_cast<uint8_t>(value);
        value >>= 8U;
    }
}

uint32_t crc32(const uint8_t* data, std::size_t size) {
    uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0
                      ? (crc >> 1U) ^ 0xedb88320U
                      : crc >> 1U;
        }
    }
    return crc ^ 0xffffffffU;
}

bool peerEquals(const PeerId& left, const PeerId& right) {
    return std::memcmp(left.bytes, right.bytes, PEER_ID_SIZE) == 0;
}

bool validSession(const ShareRestoreEntry& entry) {
    const ShareRestoreRecord& record = entry.record;
    return record.cadence_millis >= MIN_SHARE_CADENCE_MILLIS &&
           record.cadence_millis <= MAX_SHARE_CADENCE_MILLIS &&
           record.approx_radius_meters >= 0 &&
           (record.has_expiry || record.expires_at_millis == 0) &&
           record.expires_at_millis <=
               static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
}

bool validLocation(const PeerLocationRecord& record) {
    return record.location.latitude_e6 >= -90000000 &&
           record.location.latitude_e6 <= 90000000 &&
           record.location.longitude_e6 >= -180000000 &&
           record.location.longitude_e6 <= 180000000 &&
           (record.has_expiry || record.expires_at_millis == 0) &&
           record.expires_at_millis <=
               static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) &&
           record.approx_radius_meters <=
               static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
}

bool duplicateSession(
    const LocationStateSnapshot& state,
    std::size_t index) {
    for (std::size_t prior = 0; prior < index; ++prior) {
        if (peerEquals(state.sessions[prior].peer, state.sessions[index].peer)) {
            return true;
        }
    }
    return false;
}

bool duplicateLocation(
    const LocationStateSnapshot& state,
    std::size_t index) {
    for (std::size_t prior = 0; prior < index; ++prior) {
        if (peerEquals(state.locations[prior].peer, state.locations[index].peer)) {
            return true;
        }
    }
    return false;
}

void encodeSession(const ShareRestoreEntry& entry, uint8_t* output) {
    std::memcpy(output, entry.peer.bytes, PEER_ID_SIZE);
    output[16] = static_cast<uint8_t>(
        (entry.record.has_expiry ? SESSION_HAS_EXPIRY : 0U) |
        (entry.record.cease_pending ? SESSION_CEASE_PENDING : 0U) |
        (entry.record.has_approx_radius ? SESSION_HAS_APPROX_RADIUS : 0U));
    output[17] = 0;
    writeU32(output + 18, entry.record.cadence_millis);
    writeI32(output + 22, entry.record.approx_radius_meters);
    writeU64(output + 26, entry.record.expires_at_millis);
}

ShareRestoreEntry decodeSession(const uint8_t* data) {
    ShareRestoreEntry entry{};
    std::memcpy(entry.peer.bytes, data, PEER_ID_SIZE);
    entry.record.has_expiry = (data[16] & SESSION_HAS_EXPIRY) != 0;
    entry.record.cease_pending = (data[16] & SESSION_CEASE_PENDING) != 0;
    entry.record.cadence_millis = readU32(data + 18);
    entry.record.approx_radius_meters = readI32(data + 22);
    entry.record.has_approx_radius =
        (data[16] & SESSION_HAS_APPROX_RADIUS) != 0;
    entry.record.expires_at_millis = readU64(data + 26);
    return entry;
}

void encodeLocation(const PeerLocationRecord& record, uint8_t* output) {
    std::memcpy(output, record.peer.bytes, PEER_ID_SIZE);
    output[16] = static_cast<uint8_t>(
        (record.has_expiry ? LOCATION_HAS_EXPIRY : 0U) |
        (record.has_approx_radius ? LOCATION_HAS_APPROX_RADIUS : 0U));
    output[17] = 0;
    writeI32(output + 18, record.location.latitude_e6);
    writeI32(output + 22, record.location.longitude_e6);
    writeI32(output + 26, record.location.altitude_cm);
    writeU32(output + 30, record.location.speed_centi_kmh);
    writeI32(output + 34, record.location.bearing_cdeg);
    writeU16(output + 38, record.location.accuracy_cm);
    writeU16(output + 40, 0);
    writeU64(output + 42, record.location.timestamp_seconds);
    writeU64(output + 50, record.location.sensor_timestamp_seconds);
    writeU64(output + 58, record.source_timestamp_millis);
    writeU64(output + 66, record.received_at_millis);
    writeU64(output + 74, record.expires_at_millis);
    writeU32(output + 82, record.approx_radius_meters);
}

PeerLocationRecord decodeLocation(const uint8_t* data) {
    PeerLocationRecord record{};
    std::memcpy(record.peer.bytes, data, PEER_ID_SIZE);
    record.has_expiry = (data[16] & LOCATION_HAS_EXPIRY) != 0;
    record.has_approx_radius =
        (data[16] & LOCATION_HAS_APPROX_RADIUS) != 0;
    record.location.latitude_e6 = readI32(data + 18);
    record.location.longitude_e6 = readI32(data + 22);
    record.location.altitude_cm = readI32(data + 26);
    record.location.speed_centi_kmh = readU32(data + 30);
    record.location.bearing_cdeg = readI32(data + 34);
    record.location.accuracy_cm = readU16(data + 38);
    record.location.timestamp_seconds = readU64(data + 42);
    record.location.sensor_timestamp_seconds = readU64(data + 50);
    record.source_timestamp_millis = readU64(data + 58);
    record.received_at_millis = readU64(data + 66);
    record.expires_at_millis = readU64(data + 74);
    record.approx_radius_meters = readU32(data + 82);
    return record;
}

LocationStateRecordResult validateEncodedRecords(
    const uint8_t* payload,
    std::size_t session_count,
    std::size_t location_count) {
    for (std::size_t index = 0; index < session_count; ++index) {
        const uint8_t* current = payload + index * LOCATION_STATE_SESSION_BYTES;
        if ((current[16] & ~0x07U) != 0 || current[17] != 0) {
            return LocationStateRecordResult::MALFORMED;
        }
        const ShareRestoreEntry entry = decodeSession(current);
        if (!validSession(entry)) return LocationStateRecordResult::OUT_OF_RANGE;
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (std::memcmp(payload + prior * LOCATION_STATE_SESSION_BYTES,
                            current, PEER_ID_SIZE) == 0) {
                return LocationStateRecordResult::DUPLICATE_PEER;
            }
        }
    }

    const uint8_t* locations =
        payload + session_count * LOCATION_STATE_SESSION_BYTES;
    for (std::size_t index = 0; index < location_count; ++index) {
        const uint8_t* current = locations + index * LOCATION_STATE_LOCATION_BYTES;
        if ((current[16] & ~0x03U) != 0 || current[17] != 0 ||
            readU16(current + 40) != 0) {
            return LocationStateRecordResult::MALFORMED;
        }
        const PeerLocationRecord record = decodeLocation(current);
        if (!validLocation(record)) return LocationStateRecordResult::OUT_OF_RANGE;
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (std::memcmp(locations + prior * LOCATION_STATE_LOCATION_BYTES,
                            current, PEER_ID_SIZE) == 0) {
                return LocationStateRecordResult::DUPLICATE_PEER;
            }
        }
    }
    return LocationStateRecordResult::OK;
}

}  // namespace

LocationStateRecordResult encodeLocationStateRecord(
    const LocationStateSnapshot& input,
    uint8_t* output,
    std::size_t capacity,
    std::size_t& written_or_required) {
    written_or_required = 0;
    if (input.session_count > MAX_SHARE_SESSIONS ||
        input.location_count > MAX_PEER_LOCATIONS) {
        return LocationStateRecordResult::OUT_OF_RANGE;
    }
    for (std::size_t index = 0; index < input.session_count; ++index) {
        if (!validSession(input.sessions[index])) {
            return LocationStateRecordResult::OUT_OF_RANGE;
        }
        if (duplicateSession(input, index)) {
            return LocationStateRecordResult::DUPLICATE_PEER;
        }
    }
    for (std::size_t index = 0; index < input.location_count; ++index) {
        if (!validLocation(input.locations[index])) {
            return LocationStateRecordResult::OUT_OF_RANGE;
        }
        if (duplicateLocation(input, index)) {
            return LocationStateRecordResult::DUPLICATE_PEER;
        }
    }

    const std::size_t payload_size =
        input.session_count * LOCATION_STATE_SESSION_BYTES +
        input.location_count * LOCATION_STATE_LOCATION_BYTES;
    const std::size_t required =
        LOCATION_STATE_HEADER_BYTES + payload_size + LOCATION_STATE_CRC_BYTES;
    written_or_required = required;
    if (capacity < required) return LocationStateRecordResult::BUFFER_TOO_SMALL;
    if (output == nullptr) return LocationStateRecordResult::INVALID_ARGUMENT;

    std::memcpy(output, MAGIC, sizeof(MAGIC));
    writeU16(output + 4, LOCATION_STATE_SCHEMA_VERSION);
    writeU16(output + 6, 0);
    writeU32(output + 8, static_cast<uint32_t>(payload_size));
    writeU16(output + 12, static_cast<uint16_t>(input.session_count));
    writeU16(output + 14, static_cast<uint16_t>(input.location_count));
    uint8_t* cursor = output + LOCATION_STATE_HEADER_BYTES;
    for (std::size_t index = 0; index < input.session_count; ++index) {
        encodeSession(input.sessions[index], cursor);
        cursor += LOCATION_STATE_SESSION_BYTES;
    }
    for (std::size_t index = 0; index < input.location_count; ++index) {
        encodeLocation(input.locations[index], cursor);
        cursor += LOCATION_STATE_LOCATION_BYTES;
    }
    writeU32(cursor, crc32(output, required - LOCATION_STATE_CRC_BYTES));
    return LocationStateRecordResult::OK;
}

LocationStateRecordResult validateLocationStateRecord(
    const uint8_t* data,
    std::size_t size) {
    if (data == nullptr) return LocationStateRecordResult::INVALID_ARGUMENT;
    if (size < LOCATION_STATE_HEADER_BYTES + LOCATION_STATE_CRC_BYTES) {
        return LocationStateRecordResult::MALFORMED;
    }
    if (std::memcmp(data, MAGIC, sizeof(MAGIC)) != 0) {
        return LocationStateRecordResult::BAD_MAGIC;
    }
    if (readU16(data + 4) != LOCATION_STATE_SCHEMA_VERSION) {
        return LocationStateRecordResult::UNSUPPORTED_VERSION;
    }
    if (readU16(data + 6) != 0) return LocationStateRecordResult::MALFORMED;

    const std::size_t payload_size = readU32(data + 8);
    const std::size_t session_count = readU16(data + 12);
    const std::size_t location_count = readU16(data + 14);
    if (session_count > MAX_SHARE_SESSIONS ||
        location_count > MAX_PEER_LOCATIONS) {
        return LocationStateRecordResult::OUT_OF_RANGE;
    }
    const std::size_t expected_payload =
        session_count * LOCATION_STATE_SESSION_BYTES +
        location_count * LOCATION_STATE_LOCATION_BYTES;
    if (payload_size != expected_payload) {
        return LocationStateRecordResult::MALFORMED;
    }
    const std::size_t expected_size =
        LOCATION_STATE_HEADER_BYTES + expected_payload + LOCATION_STATE_CRC_BYTES;
    if (size != expected_size) return LocationStateRecordResult::MALFORMED;
    if (readU32(data + size - LOCATION_STATE_CRC_BYTES) !=
        crc32(data, size - LOCATION_STATE_CRC_BYTES)) {
        return LocationStateRecordResult::CRC_MISMATCH;
    }

    const uint8_t* payload = data + LOCATION_STATE_HEADER_BYTES;
    return validateEncodedRecords(payload, session_count, location_count);
}

LocationStateRecordResult decodeLocationStateRecord(
    const uint8_t* data,
    std::size_t size,
    LocationStateSnapshot& output) {
    const LocationStateRecordResult validation =
        validateLocationStateRecord(data, size);
    if (validation != LocationStateRecordResult::OK) return validation;

    const std::size_t session_count = readU16(data + 12);
    const std::size_t location_count = readU16(data + 14);
    const uint8_t* payload = data + LOCATION_STATE_HEADER_BYTES;

    output.session_count = 0;
    output.location_count = 0;
    for (std::size_t index = 0; index < MAX_SHARE_SESSIONS; ++index) {
        output.sessions[index] = ShareRestoreEntry{};
    }
    for (std::size_t index = 0; index < MAX_PEER_LOCATIONS; ++index) {
        output.locations[index] = PeerLocationRecord{};
    }
    output.session_count = session_count;
    output.location_count = location_count;
    const uint8_t* cursor = payload;
    for (std::size_t index = 0; index < session_count; ++index) {
        output.sessions[index] = decodeSession(cursor);
        cursor += LOCATION_STATE_SESSION_BYTES;
    }
    for (std::size_t index = 0; index < location_count; ++index) {
        output.locations[index] = decodeLocation(cursor);
        cursor += LOCATION_STATE_LOCATION_BYTES;
    }
    return LocationStateRecordResult::OK;
}

}  // namespace Telemetry
