#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

#include "Telemetry/LocationShareScheduler.h"

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
    for (std::size_t index = 0; index < Telemetry::PEER_ID_SIZE; ++index) {
        id.bytes[index] = static_cast<uint8_t>(seed + index);
    }
    return id;
}

Telemetry::SharePollResult poll(
    Telemetry::LocationShareScheduler& scheduler,
    uint64_t wall_millis,
    bool location_valid,
    Telemetry::ShareWork& work) {
    return scheduler.poll(wall_millis, wall_millis, location_valid, work);
}

Telemetry::ShareAckResult acknowledge(
    Telemetry::LocationShareScheduler& scheduler,
    const Telemetry::PeerId& id,
    uint64_t token,
    bool accepted,
    uint64_t wall_millis) {
    return scheduler.acknowledge(
        id, token, accepted, wall_millis, wall_millis);
}

Telemetry::ShareStartOptions options(
    Telemetry::ShareDuration duration = Telemetry::ShareDuration::MINUTES_15,
    uint32_t cadence = 60000) {
    Telemetry::ShareStartOptions value{};
    value.duration = duration;
    value.cadence_millis = cadence;
    value.approx_radius_meters = 0;
    return value;
}

void defaultsToNoSharing() {
    Telemetry::LocationShareScheduler scheduler;
    CHECK(scheduler.size() == 0);
    Telemetry::ShareWork work{};
    CHECK(poll(scheduler, 1000, true, work) == Telemetry::SharePollResult::NO_WORK);
    Telemetry::ShareSession session{};
    CHECK(!scheduler.get(peer(1), session));
}

void computesDurationAndMidnightBoundaries() {
    constexpr uint64_t now = 1700000000000ULL;
    struct Case {
        Telemetry::ShareDuration duration;
        uint64_t delta;
    };
    const Case cases[] = {
        {Telemetry::ShareDuration::MINUTES_15, 15ULL * 60ULL * 1000ULL},
        {Telemetry::ShareDuration::HOUR_1, 60ULL * 60ULL * 1000ULL},
        {Telemetry::ShareDuration::HOURS_4, 4ULL * 60ULL * 60ULL * 1000ULL},
    };
    for (const auto& item : cases) {
        Telemetry::LocationShareScheduler scheduler;
        CHECK(scheduler.start(peer(1), options(item.duration), now) ==
              Telemetry::ShareSessionResult::STARTED);
        Telemetry::ShareSession session{};
        CHECK(scheduler.get(peer(1), session));
        CHECK(session.has_expiry);
        CHECK(session.expires_at_millis == now + item.delta);
    }

    Telemetry::LocationShareScheduler indefinite;
    CHECK(indefinite.start(peer(2), options(Telemetry::ShareDuration::INDEFINITE), now) ==
          Telemetry::ShareSessionResult::STARTED);
    Telemetry::ShareSession session{};
    CHECK(indefinite.get(peer(2), session));
    CHECK(!session.has_expiry);

    uint64_t midnight = 0;
    CHECK(Telemetry::nextFixedOffsetMidnight(1704065400000ULL, 3600, midnight) ==
          Telemetry::MidnightResult::OK);
    CHECK(midnight == 1704150000000ULL);  // 2024-01-02 00:00 at UTC+1
    CHECK(Telemetry::nextFixedOffsetMidnight(midnight, 3600, midnight) ==
          Telemetry::MidnightResult::OK);
    CHECK(midnight == 1704236400000ULL);
    CHECK(Telemetry::nextFixedOffsetMidnight(0, 0, midnight) ==
          Telemetry::MidnightResult::CLOCK_UNAVAILABLE);
    CHECK(Telemetry::nextFixedOffsetMidnight(1000, 19 * 60 * 60, midnight) ==
          Telemetry::MidnightResult::INVALID_ARGUMENT);

    auto midnight_options = options(Telemetry::ShareDuration::LOCAL_MIDNIGHT);
    midnight_options.local_midnight_millis = 1704150000000ULL;
    Telemetry::LocationShareScheduler at_midnight;
    CHECK(at_midnight.start(peer(3), midnight_options, 1704065400000ULL) ==
          Telemetry::ShareSessionResult::STARTED);
    CHECK(at_midnight.get(peer(3), session));
    CHECK(session.expires_at_millis == midnight_options.local_midnight_millis);

    midnight_options.local_midnight_millis = 1704065400000ULL;
    CHECK(at_midnight.start(peer(4), midnight_options, 1704065400000ULL) ==
          Telemetry::ShareSessionResult::INVALID_ARGUMENT);
}

void requestsImmediateWorkAndAdvancesOnlyAfterAcceptance() {
    Telemetry::LocationShareScheduler scheduler;
    constexpr uint64_t now = 100000;
    CHECK(scheduler.start(peer(1), options(), now) ==
          Telemetry::ShareSessionResult::STARTED);

    Telemetry::ShareSession session{};
    CHECK(scheduler.get(peer(1), session));
    CHECK(!session.has_sent);
    CHECK(session.last_sent_millis == 0);

    Telemetry::ShareWork work{};
    CHECK(poll(scheduler, now, false, work) == Telemetry::SharePollResult::NO_WORK);
    CHECK(poll(scheduler, now, true, work) == Telemetry::SharePollResult::WORK);
    CHECK(work.type == Telemetry::ShareWorkType::LOCATION);
    CHECK(work.has_expiry);
    CHECK(work.token != 0);
    CHECK(poll(scheduler, now, true, work) == Telemetry::SharePollResult::NO_WORK);

    CHECK(scheduler.get(peer(1), session));
    CHECK(!session.has_sent);
    const uint64_t token = session.pending_token;
    CHECK(acknowledge(scheduler, peer(1), token, true, now + 10) ==
          Telemetry::ShareAckResult::ACCEPTED);
    CHECK(scheduler.get(peer(1), session));
    CHECK(session.has_sent);
    CHECK(session.last_sent_millis == now + 10);
    CHECK(poll(scheduler, now + 10 + 59999, true, work) ==
          Telemetry::SharePollResult::NO_WORK);
    CHECK(poll(scheduler, now + 10 + 60000, true, work) ==
          Telemetry::SharePollResult::WORK);
}

void retriesFailuresWithBoundedBackoffWithoutExtendingExpiry() {
    Telemetry::LocationShareScheduler scheduler;
    constexpr uint64_t start = 1000;
    auto config = options();
    CHECK(scheduler.start(peer(2), config, start) ==
          Telemetry::ShareSessionResult::STARTED);

    Telemetry::ShareWork work{};
    CHECK(poll(scheduler, start, true, work) == Telemetry::SharePollResult::WORK);
    CHECK(acknowledge(scheduler, peer(2), work.token, false, start) ==
          Telemetry::ShareAckResult::RETRY_SCHEDULED);
    Telemetry::ShareSession session{};
    CHECK(scheduler.get(peer(2), session));
    CHECK(!session.has_sent);
    CHECK(session.next_attempt_millis == start + Telemetry::INITIAL_RETRY_MILLIS);
    const uint64_t expiry = session.expires_at_millis;
    CHECK(poll(scheduler, session.next_attempt_millis - 1, true, work) ==
          Telemetry::SharePollResult::NO_WORK);
    CHECK(poll(scheduler, session.next_attempt_millis, true, work) ==
          Telemetry::SharePollResult::WORK);

    uint64_t now = session.next_attempt_millis;
    for (int failure = 0; failure < 12; ++failure) {
        CHECK(acknowledge(scheduler, peer(2), work.token, false, now) ==
              Telemetry::ShareAckResult::RETRY_SCHEDULED);
        CHECK(scheduler.get(peer(2), session));
        CHECK(session.next_attempt_millis >= now);
        CHECK(session.next_attempt_millis - now <= Telemetry::MAX_RETRY_MILLIS);
        CHECK(session.expires_at_millis == expiry);
        now = session.next_attempt_millis;
        if (now >= expiry) break;
        CHECK(poll(scheduler, now, true, work) == Telemetry::SharePollResult::WORK);
    }
}

void expirationQueuesExactlyOneCeaseUntilAccepted() {
    Telemetry::LocationShareScheduler scheduler;
    constexpr uint64_t start = 1000;
    CHECK(scheduler.start(peer(3), options(), start) ==
          Telemetry::ShareSessionResult::STARTED);
    Telemetry::ShareSession session{};
    CHECK(scheduler.get(peer(3), session));
    const uint64_t expiry = session.expires_at_millis;

    Telemetry::ShareWork work{};
    CHECK(poll(scheduler, expiry - 1, false, work) == Telemetry::SharePollResult::NO_WORK);
    CHECK(poll(scheduler, expiry, false, work) == Telemetry::SharePollResult::WORK);
    CHECK(work.type == Telemetry::ShareWorkType::CEASE);
    const uint64_t first_token = work.token;
    CHECK(poll(scheduler, expiry, false, work) == Telemetry::SharePollResult::NO_WORK);
    CHECK(acknowledge(scheduler, peer(3), first_token, false, expiry) ==
          Telemetry::ShareAckResult::RETRY_SCHEDULED);
    CHECK(scheduler.get(peer(3), session));
    CHECK(session.cease_pending);
    CHECK(poll(scheduler, expiry + Telemetry::INITIAL_RETRY_MILLIS - 1,
                         false, work) == Telemetry::SharePollResult::NO_WORK);
    CHECK(poll(scheduler, expiry + Telemetry::INITIAL_RETRY_MILLIS,
                         false, work) == Telemetry::SharePollResult::WORK);
    CHECK(work.type == Telemetry::ShareWorkType::CEASE);
    CHECK(work.token != first_token);
    CHECK(acknowledge(scheduler, peer(3), work.token, true,
                                expiry + Telemetry::INITIAL_RETRY_MILLIS) ==
          Telemetry::ShareAckResult::CEASED);
    CHECK(scheduler.size() == 0);
    CHECK(poll(scheduler, expiry + Telemetry::INITIAL_RETRY_MILLIS,
                         true, work) == Telemetry::SharePollResult::NO_WORK);
}

void unacknowledgedLocationLeaseCannotBlockExpiryForever() {
    Telemetry::LocationShareScheduler scheduler;
    constexpr uint64_t start = 1000;
    CHECK(scheduler.start(peer(14), options(), start) ==
          Telemetry::ShareSessionResult::STARTED);
    Telemetry::ShareSession session{};
    CHECK(scheduler.get(peer(14), session));
    const uint64_t expiry = session.expires_at_millis;

    Telemetry::ShareWork location_work{};
    CHECK(poll(scheduler, expiry - 1, true, location_work) ==
          Telemetry::SharePollResult::WORK);
    CHECK(location_work.type == Telemetry::ShareWorkType::LOCATION);
    CHECK(location_work.ack_deadline_monotonic_millis > expiry);
    CHECK(poll(scheduler, expiry, false, location_work) ==
          Telemetry::SharePollResult::NO_WORK);

    Telemetry::ShareWork cease_work{};
    CHECK(poll(scheduler, location_work.ack_deadline_monotonic_millis, false, cease_work) ==
          Telemetry::SharePollResult::WORK);
    CHECK(cease_work.type == Telemetry::ShareWorkType::CEASE);
    CHECK(acknowledge(scheduler, peer(14), location_work.token, true,
                                location_work.ack_deadline_monotonic_millis) ==
          Telemetry::ShareAckResult::STALE_TOKEN);
    CHECK(acknowledge(scheduler, peer(14), cease_work.token, true,
                                location_work.ack_deadline_monotonic_millis) ==
          Telemetry::ShareAckResult::CEASED);
}

void repeatedStopPreservesTheSingleOutstandingCease() {
    Telemetry::LocationShareScheduler scheduler;
    CHECK(scheduler.start(peer(15), options(), 1000) ==
          Telemetry::ShareSessionResult::STARTED);
    CHECK(scheduler.stop(peer(15), 1001) ==
          Telemetry::ShareSessionResult::STOPPING);
    Telemetry::ShareWork work{};
    CHECK(poll(scheduler, 1001, false, work) == Telemetry::SharePollResult::WORK);
    CHECK(work.type == Telemetry::ShareWorkType::CEASE);
    const uint64_t token = work.token;
    CHECK(scheduler.stop(peer(15), 1002) ==
          Telemetry::ShareSessionResult::STOPPING);
    Telemetry::ShareSession session{};
    CHECK(scheduler.get(peer(15), session));
    CHECK(session.awaiting_ack);
    CHECK(session.pending_token == token);
    CHECK(poll(scheduler, 1002, false, work) == Telemetry::SharePollResult::NO_WORK);
    CHECK(acknowledge(scheduler, peer(15), token, true, 1003) ==
          Telemetry::ShareAckResult::CEASED);
}

void serializesUpdateAndStopBehindOutstandingWork() {
    Telemetry::LocationShareScheduler scheduler;
    constexpr uint64_t now = 10000;
    CHECK(scheduler.start(peer(4), options(), now) ==
          Telemetry::ShareSessionResult::STARTED);
    Telemetry::ShareWork work{};
    CHECK(poll(scheduler, now, true, work) == Telemetry::SharePollResult::WORK);
    const uint64_t first_token = work.token;

    auto updated = options(Telemetry::ShareDuration::HOUR_1, 30000);
    CHECK(scheduler.start(peer(4), updated, now + 1) ==
          Telemetry::ShareSessionResult::BUSY);
    CHECK(!scheduler.cancelWithoutCease(peer(4)));
    CHECK(acknowledge(scheduler, peer(4), first_token, true, now + 2) ==
          Telemetry::ShareAckResult::ACCEPTED);
    CHECK(scheduler.start(peer(4), updated, now + 3) ==
          Telemetry::ShareSessionResult::UPDATED);
    CHECK(poll(scheduler, now + 3, true, work) == Telemetry::SharePollResult::WORK);

    CHECK(scheduler.stop(peer(4), now + 4) ==
          Telemetry::ShareSessionResult::STOPPING);
    CHECK(acknowledge(scheduler, peer(4), work.token, true, now + 5) ==
          Telemetry::ShareAckResult::ACCEPTED);
    CHECK(poll(scheduler, now + 5, false, work) == Telemetry::SharePollResult::WORK);
    CHECK(work.type == Telemetry::ShareWorkType::CEASE);
    CHECK(acknowledge(scheduler, peer(4), work.token, true, now + 6) ==
          Telemetry::ShareAckResult::CEASED);
    CHECK(scheduler.stop(peer(4), now + 7) ==
          Telemetry::ShareSessionResult::NOT_FOUND);
}

void enforcesCapacityWithoutEvictingConsent() {
    Telemetry::LocationShareScheduler scheduler;
    for (std::size_t index = 0; index < Telemetry::MAX_SHARE_SESSIONS; ++index) {
        CHECK(scheduler.start(peer(static_cast<uint8_t>(index)), options(), 1000) ==
              Telemetry::ShareSessionResult::STARTED);
    }
    CHECK(scheduler.start(peer(200), options(), 1000) ==
          Telemetry::ShareSessionResult::CAPACITY);
    CHECK(scheduler.size() == Telemetry::MAX_SHARE_SESSIONS);
    CHECK(scheduler.cancelWithoutCease(peer(10)));
    CHECK(scheduler.start(peer(200), options(), 1000) ==
          Telemetry::ShareSessionResult::STARTED);
    CHECK(scheduler.size() == Telemetry::MAX_SHARE_SESSIONS);
}

void restoresOnlyValidUnexpiredConsentAndRequiresGps() {
    Telemetry::LocationShareScheduler scheduler;
    Telemetry::ShareRestoreRecord record{};
    record.cadence_millis = 60000;
    record.has_expiry = true;
    record.expires_at_millis = 2000;
    record.approx_radius_meters = 50;

    CHECK(scheduler.restore(peer(5), record, 2000) ==
          Telemetry::ShareSessionResult::EXPIRED);
    CHECK(scheduler.size() == 0);
    CHECK(scheduler.restore(peer(5), record, 1999) ==
          Telemetry::ShareSessionResult::RESTORED);
    Telemetry::ShareWork work{};
    CHECK(poll(scheduler, 1999, false, work) == Telemetry::SharePollResult::NO_WORK);
    CHECK(poll(scheduler, 1999, true, work) == Telemetry::SharePollResult::WORK);
    CHECK(work.type == Telemetry::ShareWorkType::LOCATION);
    CHECK(work.approx_radius_meters == 50);
}

void handlesUnavailableAndBackwardClocksFailClosed() {
    Telemetry::LocationShareScheduler scheduler;
    CHECK(scheduler.start(peer(6), options(), 0) ==
          Telemetry::ShareSessionResult::CLOCK_UNAVAILABLE);
    Telemetry::ShareWork work{};
    CHECK(poll(scheduler, 0, true, work) ==
          Telemetry::SharePollResult::CLOCK_UNAVAILABLE);
    CHECK(scheduler.size() == 0);

    CHECK(scheduler.start(peer(6), options(Telemetry::ShareDuration::INDEFINITE),
                          100000) == Telemetry::ShareSessionResult::STARTED);
    CHECK(scheduler.poll(100000, 1000, true, work) ==
          Telemetry::SharePollResult::WORK);
    CHECK(scheduler.acknowledge(peer(6), work.token, true, 100000, 1001) ==
          Telemetry::ShareAckResult::ACCEPTED);
    CHECK(scheduler.poll(90000, 1002, true, work) ==
          Telemetry::SharePollResult::WORK);
    CHECK(work.type == Telemetry::ShareWorkType::LOCATION);
    const uint64_t invalid_clock =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1ULL;
    CHECK(scheduler.acknowledge(
              peer(6), work.token, true, invalid_clock, 1003) ==
          Telemetry::ShareAckResult::CLOCK_UNAVAILABLE);
    CHECK(scheduler.acknowledge(peer(6), work.token, true, 90000, 1003) ==
          Telemetry::ShareAckResult::ACCEPTED);
    CHECK(poll(scheduler, invalid_clock, true, work) ==
          Telemetry::SharePollResult::CLOCK_UNAVAILABLE);
    CHECK(scheduler.stop(peer(6), invalid_clock) ==
          Telemetry::ShareSessionResult::INVALID_ARGUMENT);
}

void rebasesExistingSessionsWhenAnotherSessionObservesClockRollback() {
    Telemetry::LocationShareScheduler scheduler;
    const auto indefinite = options(Telemetry::ShareDuration::INDEFINITE);
    CHECK(scheduler.start(peer(11), indefinite, 100000) ==
          Telemetry::ShareSessionResult::STARTED);
    CHECK(scheduler.start(peer(12), indefinite, 100000) ==
          Telemetry::ShareSessionResult::STARTED);
    Telemetry::ShareWork work{};
    CHECK(scheduler.poll(100000, 1000, true, work) ==
          Telemetry::SharePollResult::WORK);
    CHECK(scheduler.acknowledge(
              work.peer, work.token, true, 100000, 1001) ==
          Telemetry::ShareAckResult::ACCEPTED);
    CHECK(scheduler.poll(100000, 1002, true, work) ==
          Telemetry::SharePollResult::WORK);
    CHECK(scheduler.acknowledge(
              work.peer, work.token, true, 100000, 1003) ==
          Telemetry::ShareAckResult::ACCEPTED);

    CHECK(scheduler.start(peer(13), indefinite, 90000) ==
          Telemetry::ShareSessionResult::STARTED);
    CHECK(scheduler.poll(90000, 1004, true, work) ==
          Telemetry::SharePollResult::WORK);
    CHECK(work.peer.bytes[0] == 11);
}

void enforcesExclusiveMonotonicLeaseBoundaries() {
    Telemetry::LocationShareScheduler scheduler;
    CHECK(scheduler.start(peer(16),
                          options(Telemetry::ShareDuration::INDEFINITE), 1000) ==
          Telemetry::ShareSessionResult::STARTED);
    Telemetry::ShareWork work{};
    CHECK(scheduler.poll(1000, 5000, true, work) ==
          Telemetry::SharePollResult::WORK);
    CHECK(work.ack_deadline_monotonic_millis ==
          5000 + Telemetry::ACKNOWLEDGEMENT_LEASE_MILLIS);
    CHECK(scheduler.acknowledge(peer(16), work.token, true, 1000, 4999) ==
          Telemetry::ShareAckResult::CLOCK_UNAVAILABLE);
    CHECK(scheduler.acknowledge(peer(16), work.token, true, 1000,
                                work.ack_deadline_monotonic_millis) ==
          Telemetry::ShareAckResult::STALE_TOKEN);
    Telemetry::ShareSession session{};
    CHECK(scheduler.get(peer(16), session));
    CHECK(!session.awaiting_ack);
    CHECK(!session.has_sent);

    Telemetry::LocationShareScheduler overflow;
    CHECK(overflow.start(peer(17),
                         options(Telemetry::ShareDuration::INDEFINITE), 1000) ==
          Telemetry::ShareSessionResult::STARTED);
    CHECK(overflow.poll(1000, std::numeric_limits<uint64_t>::max(), true, work) ==
          Telemetry::SharePollResult::CLOCK_UNAVAILABLE);
    CHECK(overflow.get(peer(17), session));
    CHECK(!session.awaiting_ack);
}

void wallClockRollbackDoesNotChangeAnAdvertisedLease() {
    Telemetry::LocationShareScheduler scheduler;
    CHECK(scheduler.start(peer(18),
                          options(Telemetry::ShareDuration::INDEFINITE), 100000) ==
          Telemetry::ShareSessionResult::STARTED);
    Telemetry::ShareWork work{};
    CHECK(scheduler.poll(100000, 5000, true, work) ==
          Telemetry::SharePollResult::WORK);
    CHECK(work.ack_deadline_monotonic_millis == 6000);
    CHECK(scheduler.poll(90000, 5500, true, work) ==
          Telemetry::SharePollResult::NO_WORK);
    CHECK(scheduler.acknowledge(peer(18), work.token, true, 90000, 5999) ==
          Telemetry::ShareAckResult::ACCEPTED);
}

void wallClockMaximumDoesNotOverflowTheLeaseDomain() {
    Telemetry::LocationShareScheduler scheduler;
    const uint64_t wall_max =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    CHECK(scheduler.start(peer(19),
                          options(Telemetry::ShareDuration::INDEFINITE), wall_max) ==
          Telemetry::ShareSessionResult::STARTED);
    Telemetry::ShareWork work{};
    CHECK(scheduler.poll(wall_max, 100, true, work) ==
          Telemetry::SharePollResult::WORK);
    CHECK(work.ack_deadline_monotonic_millis ==
          100 + Telemetry::ACKNOWLEDGEMENT_LEASE_MILLIS);
    CHECK(scheduler.acknowledge(peer(19), work.token, true, wall_max, 101) ==
          Telemetry::ShareAckResult::ACCEPTED);
    Telemetry::ShareSession session{};
    CHECK(scheduler.get(peer(19), session));
    CHECK(session.next_attempt_millis == wall_max);
}

void rejectsInvalidConfigurationAndCorruptRestoreState() {
    Telemetry::LocationShareScheduler scheduler;
    auto config = options();
    config.cadence_millis = Telemetry::MIN_SHARE_CADENCE_MILLIS - 1;
    CHECK(scheduler.start(peer(8), config, 1000) ==
          Telemetry::ShareSessionResult::INVALID_ARGUMENT);
    config = options();
    config.cadence_millis = Telemetry::MAX_SHARE_CADENCE_MILLIS + 1U;
    CHECK(scheduler.start(peer(8), config, 1000) ==
          Telemetry::ShareSessionResult::INVALID_ARGUMENT);
    config = options();
    config.approx_radius_meters = -1;
    CHECK(scheduler.start(peer(8), config, 1000) ==
          Telemetry::ShareSessionResult::INVALID_ARGUMENT);
    config = options();
    CHECK(scheduler.start(
              peer(8), config,
              static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) ==
          Telemetry::ShareSessionResult::INVALID_ARGUMENT);

    Telemetry::ShareRestoreRecord record{};
    record.cadence_millis = 60000;
    record.has_expiry = false;
    record.expires_at_millis = 1234;
    CHECK(scheduler.restore(peer(8), record, 1000) ==
          Telemetry::ShareSessionResult::INVALID_ARGUMENT);
    record.expires_at_millis = 0;
    record.approx_radius_meters = -1;
    CHECK(scheduler.restore(peer(8), record, 1000) ==
          Telemetry::ShareSessionResult::INVALID_ARGUMENT);
    CHECK(scheduler.size() == 0);
}

void snapshotsOnlyDurableConsentFieldsIntoCallerStorage() {
    Telemetry::LocationShareScheduler scheduler;
    auto first = options(Telemetry::ShareDuration::HOUR_1, 30000);
    first.approx_radius_meters = 25;
    CHECK(scheduler.start(peer(9), first, 1000) ==
          Telemetry::ShareSessionResult::STARTED);
    CHECK(scheduler.start(peer(10), options(Telemetry::ShareDuration::INDEFINITE),
                          1000) == Telemetry::ShareSessionResult::STARTED);

    Telemetry::ShareRestoreEntry too_small[1]{};
    too_small[0].record.cadence_millis = 1234;
    std::size_t written = 0;
    CHECK(scheduler.snapshot(too_small, 1, written) ==
          Telemetry::ShareSnapshotResult::BUFFER_TOO_SMALL);
    CHECK(written == 2);
    CHECK(too_small[0].record.cadence_millis == 1234);

    Telemetry::ShareRestoreEntry entries[2]{};
    CHECK(scheduler.snapshot(entries, 2, written) ==
          Telemetry::ShareSnapshotResult::OK);
    CHECK(written == 2);
    CHECK(entries[0].record.cadence_millis == 30000);
    CHECK(entries[0].record.approx_radius_meters == 25);
    CHECK(entries[0].record.has_expiry);
    const uint64_t saved_expiry = entries[0].record.expires_at_millis;

    CHECK(scheduler.stop(peer(9), 1001) ==
          Telemetry::ShareSessionResult::STOPPING);
    CHECK(entries[0].record.expires_at_millis == saved_expiry);
    CHECK(scheduler.snapshot(entries, 2, written) ==
          Telemetry::ShareSnapshotResult::OK);
    CHECK(written == 2);
    CHECK(entries[0].record.cease_pending);

    Telemetry::LocationShareScheduler restored;
    CHECK(restored.restore(entries[0].peer, entries[0].record,
                           saved_expiry + 1) ==
          Telemetry::ShareSessionResult::RESTORED);
    Telemetry::ShareWork work{};
    CHECK(poll(restored, saved_expiry + 1, false, work) ==
          Telemetry::SharePollResult::WORK);
    CHECK(work.type == Telemetry::ShareWorkType::CEASE);

    CHECK(scheduler.snapshot(nullptr, 2, written) ==
          Telemetry::ShareSnapshotResult::INVALID_ARGUMENT);
}

void survivesDeterministicHundredThousandOperationStress() {
    Telemetry::LocationShareScheduler scheduler;
    CHECK(sizeof(scheduler) <= 8192);
    uint32_t random = 0x9e3779b9U;
    Telemetry::ShareWork work{};
    for (uint64_t operation = 1; operation <= 100000; ++operation) {
        random = random * 1664525U + 1013904223U;
        const auto id = peer(static_cast<uint8_t>(random >> 24U));
        switch (random & 0x03U) {
            case 0:
                scheduler.start(id, options(Telemetry::ShareDuration::INDEFINITE,
                                             Telemetry::MIN_SHARE_CADENCE_MILLIS),
                                operation);
                break;
            case 1:
                scheduler.stop(id, operation);
                break;
            case 2:
                scheduler.cancelWithoutCease(id);
                break;
            default:
                break;
        }
        if (poll(scheduler, operation, true, work) ==
            Telemetry::SharePollResult::WORK) {
            acknowledge(scheduler, work.peer, work.token, (random & 0x10U) != 0,
                                  operation);
        }
        CHECK(scheduler.size() <= Telemetry::MAX_SHARE_SESSIONS);
    }
}

void expirationPreemptsAQueuedRetry() {
    Telemetry::LocationShareScheduler scheduler;
    constexpr uint64_t start = 1000;
    CHECK(scheduler.start(peer(7), options(), start) ==
          Telemetry::ShareSessionResult::STARTED);
    Telemetry::ShareSession session{};
    CHECK(scheduler.get(peer(7), session));
    const uint64_t expiry = session.expires_at_millis;

    Telemetry::ShareWork work{};
    CHECK(poll(scheduler, expiry - 1000, true, work) ==
          Telemetry::SharePollResult::WORK);
    CHECK(acknowledge(scheduler, peer(7), work.token, false, expiry - 1000) ==
          Telemetry::ShareAckResult::RETRY_SCHEDULED);
    CHECK(poll(scheduler, expiry, false, work) == Telemetry::SharePollResult::WORK);
    CHECK(work.type == Telemetry::ShareWorkType::CEASE);
}

}  // namespace

int main() {
    defaultsToNoSharing();
    computesDurationAndMidnightBoundaries();
    requestsImmediateWorkAndAdvancesOnlyAfterAcceptance();
    retriesFailuresWithBoundedBackoffWithoutExtendingExpiry();
    expirationQueuesExactlyOneCeaseUntilAccepted();
    unacknowledgedLocationLeaseCannotBlockExpiryForever();
    repeatedStopPreservesTheSingleOutstandingCease();
    serializesUpdateAndStopBehindOutstandingWork();
    enforcesCapacityWithoutEvictingConsent();
    restoresOnlyValidUnexpiredConsentAndRequiresGps();
    handlesUnavailableAndBackwardClocksFailClosed();
    rebasesExistingSessionsWhenAnotherSessionObservesClockRollback();
    enforcesExclusiveMonotonicLeaseBoundaries();
    wallClockRollbackDoesNotChangeAnAdvertisedLease();
    wallClockMaximumDoesNotOverflowTheLeaseDomain();
    rejectsInvalidConfigurationAndCorruptRestoreState();
    snapshotsOnlyDurableConsentFieldsIntoCallerStorage();
    expirationPreemptsAQueuedRetry();
    survivesDeterministicHundredThousandOperationStress();
    std::cout << "location share scheduler: " << passed << " passed, "
              << failures << " failed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
