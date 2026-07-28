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
    // Sideband Location.speed is kilometres/hour, scaled by 100 on wire.
    uint32_t speed_centi_kmh = 0;
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
    OUT_OF_RANGE,
};

enum class EncodeResult : uint8_t {
    OK,
    INVALID_ARGUMENT,
    BUFFER_TOO_SMALL,
    OUT_OF_RANGE,
};

enum class FieldValueResult : uint8_t {
    OK,
    INVALID_ARGUMENT,
    NOT_BINARY,
    MALFORMED,
    BUFFER_TOO_SMALL,
};

struct BinaryView {
    BinaryView() = default;
    BinaryView(const uint8_t* bytes, std::size_t length)
        : data(bytes), size(length) {}

    const uint8_t* data = nullptr;
    std::size_t size = 0;
};

FieldValueResult unwrapLxmfBinaryFieldValue(
    const uint8_t* raw_value,
    std::size_t raw_size,
    BinaryView& output);

FieldValueResult wrapLxmfBinaryFieldValue(
    const uint8_t* payload,
    std::size_t payload_size,
    uint8_t* output,
    std::size_t capacity,
    std::size_t& written);

DecodeResult decodeLocationTelemetry(
    const uint8_t* data,
    std::size_t size,
    LocationTelemetry& output);

EncodeResult encodeLocationTelemetry(
    const LocationTelemetry& input,
    uint8_t* output,
    std::size_t capacity,
    std::size_t& written);

}  // namespace Telemetry

#endif  // PYXIS_TELEMETRY_LOCATION_TELEMETRY_CODEC_H
