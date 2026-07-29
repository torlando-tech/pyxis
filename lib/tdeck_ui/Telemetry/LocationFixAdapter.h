#ifndef PYXIS_TELEMETRY_LOCATION_FIX_ADAPTER_H
#define PYXIS_TELEMETRY_LOCATION_FIX_ADAPTER_H

#include <cstdint>

#include "LocationTelemetryCodec.h"

namespace Telemetry {

constexpr uint32_t MAX_GPS_FIX_AGE_MILLIS = 10000U;

struct GpsFixSample {
    bool location_valid = false;
    uint32_t location_age_millis = 0;
    double latitude_degrees = 0.0;
    double longitude_degrees = 0.0;

    bool altitude_valid = false;
    double altitude_meters = 0.0;
    bool speed_valid = false;
    double speed_kilometers_per_hour = 0.0;
    bool bearing_valid = false;
    double bearing_degrees = 0.0;
    bool hdop_valid = false;
    double hdop = 0.0;
};

// Converts a fresh GPS sample into the fixed-unit telemetry contract. Output is
// modified only on success. wall_now_millis supplies the observation timestamp.
bool locationTelemetryFromGpsFix(
    const GpsFixSample& sample,
    uint64_t wall_now_millis,
    LocationTelemetry& output);

}  // namespace Telemetry

#endif
