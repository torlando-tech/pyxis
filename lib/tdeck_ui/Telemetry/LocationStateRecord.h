#ifndef PYXIS_TELEMETRY_LOCATION_STATE_RECORD_H
#define PYXIS_TELEMETRY_LOCATION_STATE_RECORD_H

#include <cstddef>
#include <cstdint>

#include "LocationShareScheduler.h"
#include "LocationShareState.h"

namespace Telemetry {

constexpr uint16_t LOCATION_STATE_SCHEMA_VERSION = 1;
constexpr std::size_t LOCATION_STATE_HEADER_BYTES = 16;
constexpr std::size_t LOCATION_STATE_SESSION_BYTES = 34;
constexpr std::size_t LOCATION_STATE_LOCATION_BYTES = 86;
constexpr std::size_t LOCATION_STATE_CRC_BYTES = 4;
constexpr std::size_t MAX_LOCATION_STATE_RECORD_BYTES =
    LOCATION_STATE_HEADER_BYTES +
    MAX_SHARE_SESSIONS * LOCATION_STATE_SESSION_BYTES +
    MAX_PEER_LOCATIONS * LOCATION_STATE_LOCATION_BYTES +
    LOCATION_STATE_CRC_BYTES;
static_assert(MAX_LOCATION_STATE_RECORD_BYTES <= 4096,
              "persistent location record must remain bounded to 4 KiB");

struct LocationStateSnapshot {
    std::size_t session_count = 0;
    ShareRestoreEntry sessions[MAX_SHARE_SESSIONS]{};
    std::size_t location_count = 0;
    PeerLocationRecord locations[MAX_PEER_LOCATIONS]{};
};

enum class LocationStateRecordResult : uint8_t {
    OK,
    INVALID_ARGUMENT,
    BUFFER_TOO_SMALL,
    BAD_MAGIC,
    UNSUPPORTED_VERSION,
    MALFORMED,
    CRC_MISMATCH,
    OUT_OF_RANGE,
    DUPLICATE_PEER,
};

LocationStateRecordResult encodeLocationStateRecord(
    const LocationStateSnapshot& input,
    uint8_t* output,
    std::size_t capacity,
    std::size_t& written_or_required);

LocationStateRecordResult decodeLocationStateRecord(
    const uint8_t* data,
    std::size_t size,
    LocationStateSnapshot& output);

LocationStateRecordResult validateLocationStateRecord(
    const uint8_t* data,
    std::size_t size);

}  // namespace Telemetry

#endif  // PYXIS_TELEMETRY_LOCATION_STATE_RECORD_H
