#include "LocationFixAdapter.h"

#include <cmath>
#include <limits>

namespace Telemetry {
namespace {

template <typename Integer>
Integer roundedClamped(double value) {
    const double minimum = static_cast<double>(std::numeric_limits<Integer>::min());
    const double maximum = static_cast<double>(std::numeric_limits<Integer>::max());
    if (value <= minimum) return std::numeric_limits<Integer>::min();
    if (value >= maximum) return std::numeric_limits<Integer>::max();
    return static_cast<Integer>(std::llround(value));
}

template <typename Integer>
Integer roundedUnsignedClamped(double value) {
    if (value <= 0.0) return 0;
    const double maximum = static_cast<double>(std::numeric_limits<Integer>::max());
    if (value >= maximum) return std::numeric_limits<Integer>::max();
    return static_cast<Integer>(std::llround(value));
}

bool finiteInRange(double value, double minimum, double maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

}  // namespace

bool locationTelemetryFromGpsFix(
    const GpsFixSample& sample,
    uint64_t wall_now_millis,
    LocationTelemetry& output) {
    if (!sample.location_valid ||
        sample.location_age_millis > MAX_GPS_FIX_AGE_MILLIS ||
        wall_now_millis == 0 ||
        !finiteInRange(sample.latitude_degrees, -90.0, 90.0) ||
        !finiteInRange(sample.longitude_degrees, -180.0, 180.0)) {
        return false;
    }
    if ((sample.altitude_valid && !std::isfinite(sample.altitude_meters)) ||
        (sample.speed_valid &&
         (!std::isfinite(sample.speed_kilometers_per_hour) ||
          sample.speed_kilometers_per_hour < 0.0)) ||
        (sample.bearing_valid &&
         (!std::isfinite(sample.bearing_degrees) ||
          sample.bearing_degrees < 0.0 || sample.bearing_degrees >= 360.0)) ||
        (sample.hdop_valid && (!std::isfinite(sample.hdop) || sample.hdop < 0.0))) {
        return false;
    }

    LocationTelemetry candidate{};
    candidate.latitude_e6 = roundedClamped<int32_t>(sample.latitude_degrees * 1000000.0);
    candidate.longitude_e6 = roundedClamped<int32_t>(sample.longitude_degrees * 1000000.0);
    if (sample.altitude_valid) {
        candidate.altitude_cm = roundedClamped<int32_t>(sample.altitude_meters * 100.0);
    }
    if (sample.speed_valid) {
        candidate.speed_centi_kmh = roundedUnsignedClamped<uint32_t>(
            sample.speed_kilometers_per_hour * 100.0);
    }
    if (sample.bearing_valid) {
        candidate.bearing_cdeg = roundedUnsignedClamped<uint32_t>(
            sample.bearing_degrees * 100.0);
    }
    if (sample.hdop_valid) {
        // TinyGPS++ exposes dimensionless HDOP. Preserve the existing Pyxis
        // approximation of horizontal accuracy as HDOP * 5 metres.
        candidate.accuracy_cm = roundedUnsignedClamped<uint16_t>(sample.hdop * 500.0);
    }
    candidate.timestamp_seconds = wall_now_millis / 1000ULL;
    candidate.sensor_timestamp_seconds = candidate.timestamp_seconds;
    if (candidate.timestamp_seconds == 0) return false;

    output = candidate;
    return true;
}

}  // namespace Telemetry
