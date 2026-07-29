#include "UI/LXMF/LocationShareControlModel.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

using UI::LXMF::LocationShareControlModel;
using UI::LXMF::LocationShareControlStatus;
using UI::LXMF::LocationShareControlError;
using UI::LXMF::LocationShareDuration;
using UI::LXMF::LocationShareCadence;
using UI::LXMF::LocationSharePrecision;

static int passed = 0;
static int failed = 0;

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(#condition); } while (0)
#define RUN(test) do { try { test(); ++passed; std::printf("PASS %s\n", #test); } catch (const std::exception& e) { ++failed; std::printf("FAIL %s: %s\n", #test, e.what()); } } while (0)

static void fillPeer(uint8_t peer[16], uint8_t seed) {
    for (std::size_t i = 0; i < 16; ++i) peer[i] = static_cast<uint8_t>(seed + i);
}

static void defaults_are_off_and_bounded() {
    LocationShareControlModel model;
    CHECK(model.status() == LocationShareControlStatus::OFF);
    CHECK(model.duration() == LocationShareDuration::MINUTES_15);
    CHECK(model.cadence() == LocationShareCadence::MINUTE_1);
    CHECK(model.precision() == LocationSharePrecision::EXACT);
    CHECK(!model.confirmationPending());
    CHECK(model.durationMillis() == 15ULL * 60ULL * 1000ULL);
    CHECK(model.cadenceMillis() == 60U * 1000U);
    CHECK(!model.hasApproximation());
    CHECK(model.approximationMeters() == 0);
}

static void only_allowlisted_choices_are_accepted() {
    LocationShareControlModel model;
    CHECK(model.selectDuration(LocationShareDuration::HOUR_1));
    CHECK(model.durationMillis() == 60ULL * 60ULL * 1000ULL);
    CHECK(model.selectDuration(LocationShareDuration::HOURS_4));
    CHECK(model.durationMillis() == 4ULL * 60ULL * 60ULL * 1000ULL);
    CHECK(!model.selectDuration(static_cast<LocationShareDuration>(99)));
    CHECK(model.duration() == LocationShareDuration::HOURS_4);

    CHECK(model.selectCadence(LocationShareCadence::MINUTES_5));
    CHECK(model.cadenceMillis() == 5U * 60U * 1000U);
    CHECK(model.selectCadence(LocationShareCadence::MINUTES_15));
    CHECK(model.cadenceMillis() == 15U * 60U * 1000U);
    CHECK(!model.selectCadence(static_cast<LocationShareCadence>(99)));

    CHECK(model.selectPrecision(LocationSharePrecision::METERS_100));
    CHECK(model.approximationMeters() == 100);
    CHECK(model.selectPrecision(LocationSharePrecision::KILOMETER_1));
    CHECK(model.approximationMeters() == 1000);
    CHECK(model.selectPrecision(LocationSharePrecision::KILOMETERS_10));
    CHECK(model.approximationMeters() == 10000);
    CHECK(!model.selectPrecision(static_cast<LocationSharePrecision>(99)));
}

static void peer_must_be_exactly_sixteen_bytes() {
    LocationShareControlModel model;
    uint8_t peer[17] = {};
    CHECK(!model.openForPeer(peer, 15));
    CHECK(model.error() == LocationShareControlError::INVALID);
    CHECK(!model.hasPeer());
    CHECK(!model.openForPeer(peer, 17));
    CHECK(!model.hasPeer());
    CHECK(model.openForPeer(peer, 16));
    CHECK(model.hasPeer());
    CHECK(std::memcmp(model.peer(), peer, 16) == 0);
}

static void opening_never_starts_and_confirmation_is_explicit() {
    LocationShareControlModel model;
    uint8_t peer[16]; fillPeer(peer, 0x20);
    CHECK(model.openForPeer(peer, sizeof(peer)));
    CHECK(model.status() == LocationShareControlStatus::OFF);
    CHECK(model.requestConfirmation());
    CHECK(model.confirmationPending());
    char summary[192] = {};
    CHECK(model.formatConfirmation(summary, sizeof(summary)));
    CHECK(std::strstr(summary, "202122232425262728292a2b2c2d2e2f") != NULL);
    CHECK(std::strstr(summary, "15 min") != NULL);
    CHECK(std::strstr(summary, "1 min") != NULL);
    CHECK(std::strstr(summary, "Exact") != NULL);
    model.cancelConfirmation();
    CHECK(!model.confirmationPending());
    CHECK(model.status() == LocationShareControlStatus::OFF);
}

static void active_and_stopping_sessions_are_rendered() {
    LocationShareControlModel model;
    uint8_t peer[16]; fillPeer(peer, 1);
    CHECK(model.openForPeer(peer, sizeof(peer)));
    model.applyActive(1700000900000ULL, 300000U, true, 1000);
    CHECK(model.status() == LocationShareControlStatus::ACTIVE);
    CHECK(model.expiresAtMillis() == 1700000900000ULL);
    CHECK(model.cadenceMillis() == 300000U);
    CHECK(model.approximationMeters() == 1000);
    char state[160] = {};
    CHECK(model.formatState(state, sizeof(state)));
    CHECK(std::strstr(state, "ACTIVE") != NULL);
    CHECK(std::strstr(state, "5 min") != NULL);
    CHECK(std::strstr(state, "1 km") != NULL);
    model.markStopping();
    CHECK(model.status() == LocationShareControlStatus::STOPPING);
    model.applyOff();
    CHECK(model.status() == LocationShareControlStatus::OFF);
}

static void errors_are_actionable_and_do_not_enable_sharing() {
    struct Case { LocationShareControlError error; const char* text; } cases[] = {
        {LocationShareControlError::CLOCK_UNAVAILABLE, "clock"},
        {LocationShareControlError::STORAGE_FAILURE, "storage"},
        {LocationShareControlError::CAPACITY, "capacity"},
        {LocationShareControlError::BUSY, "busy"},
        {LocationShareControlError::INVALID, "invalid"},
    };
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        LocationShareControlModel model;
        model.applyError(cases[i].error);
        CHECK(model.status() == LocationShareControlStatus::OFF);
        CHECK(std::strstr(model.errorText(), cases[i].text) != NULL);
    }
}

int main() {
    RUN(defaults_are_off_and_bounded);
    RUN(only_allowlisted_choices_are_accepted);
    RUN(peer_must_be_exactly_sixteen_bytes);
    RUN(opening_never_starts_and_confirmation_is_explicit);
    RUN(active_and_stopping_sessions_are_rendered);
    RUN(errors_are_actionable_and_do_not_enable_sharing);
    std::printf("location share control model: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
