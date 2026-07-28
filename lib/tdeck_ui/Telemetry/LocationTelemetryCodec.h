#ifndef PYXIS_TELEMETRY_LOCATION_TELEMETRY_CODEC_H
#define PYXIS_TELEMETRY_LOCATION_TELEMETRY_CODEC_H

#include <cstddef>
#include <cstdint>

namespace Telemetry {

constexpr uint8_t FIELD_TELEMETRY = 0x02;
constexpr uint8_t FIELD_ICON_APPEARANCE = 0x04;
constexpr uint8_t FIELD_CUSTOM_META = 0xFD;
constexpr uint8_t SID_TIME = 0x01;
constexpr uint8_t SID_LOCATION = 0x02;

struct LocationTelemetry {
    int32_t latitude_e6 = 0;
    int32_t longitude_e6 = 0;
    int32_t altitude_cm = 0;
    uint32_t speed_cms = 0;
    int32_t bearing_cdeg = 0;
    uint16_t accuracy_cm = 0;
    uint64_t timestamp_seconds = 0;
    uint64_t sensor_timestamp_seconds = 0;
};

enum class DecodeResult : uint8_t {
    OK,
    INVALID_ARGUMENT,
    MALFORMED,
    MISSING_LOCATION,
};

DecodeResult decodeLocationTelemetry(
    const uint8_t* data,
    std::size_t size,
    LocationTelemetry& output);

}  // namespace Telemetry

#endif  // PYXIS_TELEMETRY_LOCATION_TELEMETRY_CODEC_H
