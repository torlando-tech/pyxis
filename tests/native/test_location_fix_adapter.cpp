#include "Telemetry/LocationFixAdapter.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {
int failures = 0;
#define CHECK(expr) do { if (!(expr)) { ++failures; std::cerr << "FAIL line " << __LINE__ << ": " #expr "\n"; } } while (false)

Telemetry::GpsFixSample validSample() {
    Telemetry::GpsFixSample sample{};
    sample.location_valid = true;
    sample.location_age_millis = 250;
    sample.latitude_degrees = 37.7749;
    sample.longitude_degrees = -122.4194;
    sample.altitude_valid = true;
    sample.altitude_meters = 16.25;
    sample.speed_valid = true;
    sample.speed_kilometers_per_hour = 12.34;
    sample.bearing_valid = true;
    sample.bearing_degrees = 42.0;

    return sample;
}

void convertsFreshFixWithExplicitUnits() {
    const auto sample = validSample();
    Telemetry::LocationTelemetry output{};
    CHECK(Telemetry::locationTelemetryFromGpsFix(sample, 1700000000123ULL, output));
    CHECK(output.latitude_e6 == 37774900);
    CHECK(output.longitude_e6 == -122419400);
    CHECK(output.altitude_cm == 1625);
    CHECK(output.speed_centi_kmh == 1234U);
    CHECK(output.bearing_cdeg == 4200U);
    CHECK(output.accuracy_cm == 0U);
    CHECK(output.timestamp_seconds == 1699999999ULL);
    CHECK(output.sensor_timestamp_seconds == 1699999999ULL);
}

void rejectsUnavailableStaleAndInvalidFixesTransactionally() {
    Telemetry::LocationTelemetry sentinel{};
    sentinel.latitude_e6 = 123;
    auto sample = validSample();
    sample.location_valid = false;
    CHECK(!Telemetry::locationTelemetryFromGpsFix(sample, 1700000000123ULL, sentinel));
    CHECK(sentinel.latitude_e6 == 123);

    sample = validSample();
    sample.location_age_millis = Telemetry::MAX_GPS_FIX_AGE_MILLIS + 1U;
    CHECK(!Telemetry::locationTelemetryFromGpsFix(sample, 1700000000123ULL, sentinel));
    CHECK(sentinel.latitude_e6 == 123);

    sample = validSample();
    sample.latitude_degrees = 91.0;
    CHECK(!Telemetry::locationTelemetryFromGpsFix(sample, 1700000000123ULL, sentinel));
    sample = validSample();
    CHECK(!Telemetry::locationTelemetryFromGpsFix(sample, 0, sentinel));
    CHECK(!Telemetry::locationTelemetryFromGpsFix(
        sample, Telemetry::MIN_VALID_LOCATION_WALL_MILLIS - 1U, sentinel));
}

void defaultsMissingOptionalSensorsAndClampsRepresentableValues() {
    auto sample = validSample();
    sample.altitude_valid = false;
    sample.speed_valid = false;
    sample.bearing_valid = false;

    Telemetry::LocationTelemetry output{};
    CHECK(Telemetry::locationTelemetryFromGpsFix(sample, 1700000000123ULL, output));
    CHECK(output.altitude_cm == 0);
    CHECK(output.speed_centi_kmh == 0U);
    CHECK(output.bearing_cdeg == 0U);
    CHECK(output.accuracy_cm == 0U);

    sample = validSample();
    sample.altitude_meters = 1.0e20;
    sample.speed_kilometers_per_hour = 1.0e20;

    CHECK(Telemetry::locationTelemetryFromGpsFix(sample, 1700000000123ULL, output));
    CHECK(output.altitude_cm == std::numeric_limits<int32_t>::max());
    CHECK(output.speed_centi_kmh == std::numeric_limits<uint32_t>::max());
    CHECK(output.accuracy_cm == 0U);
}

void rejectsNonFiniteAndInvalidOptionalDomains() {
    auto sample = validSample();
    Telemetry::LocationTelemetry output{};
    sample.longitude_degrees = std::numeric_limits<double>::quiet_NaN();
    CHECK(!Telemetry::locationTelemetryFromGpsFix(sample, 1700000000123ULL, output));
    sample = validSample();
    sample.speed_kilometers_per_hour = -1.0;
    CHECK(!Telemetry::locationTelemetryFromGpsFix(sample, 1700000000123ULL, output));
    sample = validSample();
    sample.bearing_degrees = 360.0;
    CHECK(!Telemetry::locationTelemetryFromGpsFix(sample, 1700000000123ULL, output));

}
}

int main() {
    convertsFreshFixWithExplicitUnits();
    rejectsUnavailableStaleAndInvalidFixesTransactionally();
    defaultsMissingOptionalSensorsAndClampsRepresentableValues();
    rejectsNonFiniteAndInvalidOptionalDomains();
    if (failures != 0) return 1;
    std::cout << "location fix adapter tests passed\n";
    return 0;
}
