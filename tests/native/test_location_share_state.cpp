#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

#include "Telemetry/LocationShareState.h"

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

Telemetry::LocationTelemetry location(uint64_t seconds, int32_t latitude = 1000000) {
    Telemetry::LocationTelemetry value{};
    value.latitude_e6 = latitude;
    value.longitude_e6 = -1000000;
    value.accuracy_cm = 100;
    value.timestamp_seconds = seconds;
    value.sensor_timestamp_seconds = seconds;
    return value;
}

Telemetry::CustomLocationMeta metaTimestamp(uint64_t millis) {
    Telemetry::CustomLocationMeta meta{};
    meta.has_timestamp = true;
    meta.timestamp_millis = millis;
    return meta;
}

bool hasPeer(const Telemetry::PeerLocationStore& store,
             const Telemetry::PeerId& id,
             Telemetry::PeerLocationRecord* record = nullptr) {
    Telemetry::PeerLocationRecord candidate{};
    const bool found = store.get(id, candidate);
    if (found && record != nullptr) *record = candidate;
    return found;
}

void insertsUpdatesAndRejectsStaleData() {
    Telemetry::PeerLocationStore store;
    const auto id = peer(1);
    Telemetry::CustomLocationMeta no_meta{};

    CHECK(store.apply(id, location(10, 100), no_meta, 1000) ==
          Telemetry::PeerLocationResult::INSERTED);
    CHECK(store.size() == 1);
    CHECK(store.apply(id, location(10, 200), no_meta, 1001) ==
          Telemetry::PeerLocationResult::UPDATED);

    Telemetry::PeerLocationRecord record{};
    CHECK(hasPeer(store, id, &record));
    CHECK(record.location.latitude_e6 == 200);
    CHECK(record.source_timestamp_millis == 10000);
    CHECK(record.received_at_millis == 1001);

    CHECK(store.apply(id, location(9, 300), no_meta, 1002) ==
          Telemetry::PeerLocationResult::STALE);
    CHECK(hasPeer(store, id, &record));
    CHECK(record.location.latitude_e6 == 200);
    CHECK(record.received_at_millis == 1001);

    const auto precise = metaTimestamp(10001);
    CHECK(store.apply(id, location(1, 400), precise, 1003) ==
          Telemetry::PeerLocationResult::UPDATED);
    CHECK(hasPeer(store, id, &record));
    CHECK(record.source_timestamp_millis == 10001);
    CHECK(record.location.latitude_e6 == 400);
}

void appliesOrderedCeaseWithoutTouchingOtherPeers() {
    Telemetry::PeerLocationStore store;
    Telemetry::CustomLocationMeta no_meta{};
    const auto first = peer(10);
    const auto second = peer(20);
    CHECK(store.apply(first, location(10), no_meta, 1000) ==
          Telemetry::PeerLocationResult::INSERTED);
    CHECK(store.apply(second, location(10), no_meta, 1001) ==
          Telemetry::PeerLocationResult::INSERTED);

    auto cease = metaTimestamp(9999);
    cease.has_cease = true;
    cease.cease = true;
    CHECK(store.apply(first, location(0), cease, 1002) ==
          Telemetry::PeerLocationResult::STALE);
    CHECK(hasPeer(store, first));
    CHECK(hasPeer(store, second));

    cease.timestamp_millis = 10000;
    CHECK(store.apply(first, location(0), cease, 1003) ==
          Telemetry::PeerLocationResult::CEASED);
    CHECK(!hasPeer(store, first));
    CHECK(hasPeer(store, second));
    CHECK(store.size() == 1);
    CHECK(store.apply(first, location(0), cease, 1004) ==
          Telemetry::PeerLocationResult::NOT_FOUND);
}

void reusesVacanciesBeforeDeterministicEviction() {
    Telemetry::PeerLocationStore store;
    Telemetry::CustomLocationMeta no_meta{};
    for (std::size_t index = 0; index < Telemetry::MAX_PEER_LOCATIONS; ++index) {
        CHECK(store.apply(peer(static_cast<uint8_t>(index)), location(10), no_meta, 5) ==
              Telemetry::PeerLocationResult::INSERTED);
    }
    CHECK(store.size() == Telemetry::MAX_PEER_LOCATIONS);

    auto cease = metaTimestamp(10000);
    cease.has_cease = true;
    cease.cease = true;
    CHECK(store.apply(peer(10), location(0), cease, 6) ==
          Telemetry::PeerLocationResult::CEASED);
    CHECK(store.apply(peer(100), location(11), no_meta, 6) ==
          Telemetry::PeerLocationResult::INSERTED);
    CHECK(hasPeer(store, peer(0)));
    CHECK(hasPeer(store, peer(100)));

    // Every live record had received_at=5 except the replacement at 6. With
    // no vacancy, the stable tie-breaker evicts the lowest slot, peer 0.
    CHECK(store.apply(peer(101), location(12), no_meta, 7) ==
          Telemetry::PeerLocationResult::INSERTED);
    CHECK(!hasPeer(store, peer(0)));
    CHECK(hasPeer(store, peer(1)));
    CHECK(hasPeer(store, peer(101)));
    CHECK(store.size() == Telemetry::MAX_PEER_LOCATIONS);
}

void enforcesExpiryAndStaleDisplayBoundaries() {
    Telemetry::PeerLocationStore store;
    const auto id = peer(30);
    auto meta = metaTimestamp(10000);
    meta.has_expires = true;
    meta.expires_millis = 5000;
    CHECK(store.apply(id, location(10), meta, 1000) ==
          Telemetry::PeerLocationResult::INSERTED);

    Telemetry::PeerLocationRecord snapshot[2]{};
    CHECK(store.snapshot(4999, 10000, snapshot, 2) == 1);
    CHECK(store.snapshot(5000, 10000, snapshot, 2) == 0);
    CHECK(store.prune(4999, 10000) == 0);
    CHECK(store.prune(5000, 10000) == 1);
    CHECK(store.size() == 0);

    Telemetry::CustomLocationMeta no_meta{};
    CHECK(store.apply(id, location(1), no_meta, 1000) ==
          Telemetry::PeerLocationResult::INSERTED);
    CHECK(store.snapshot(1100, 100, snapshot, 2) == 1);
    CHECK(store.snapshot(1101, 100, snapshot, 2) == 0);
    CHECK(store.snapshot(999, 100, snapshot, 2) == 1);  // clock moved backward
    CHECK(store.prune(1100, 100) == 0);
    CHECK(store.prune(1101, 100) == 1);
}

void basesFreshnessOnSenderCaptureTimeNotReceiptTime() {
    Telemetry::PeerLocationStore store;
    Telemetry::CustomLocationMeta no_meta{};
    const auto id = peer(31);
    CHECK(store.apply(id, location(1), no_meta, 100000) ==
          Telemetry::PeerLocationResult::INSERTED);

    Telemetry::PeerLocationRecord snapshot[1]{};
    CHECK(store.snapshot(100000, 1000, snapshot, 1) == 0);
    CHECK(store.prune(100000, 1000) == 1);
    CHECK(!hasPeer(store, id));
}

void treatsPresentEpochZeroExpiryAsExpired() {
    Telemetry::PeerLocationStore store;
    auto meta = metaTimestamp(10000);
    meta.has_expires = true;
    meta.expires_millis = 0;
    CHECK(store.apply(peer(32), location(10), meta, 1000) ==
          Telemetry::PeerLocationResult::EXPIRED);
    CHECK(store.size() == 0);
}

void reusesExpiredSlotsBeforeEvictingLiveRecords() {
    Telemetry::PeerLocationStore store;
    Telemetry::CustomLocationMeta no_meta{};
    for (std::size_t index = 0; index < Telemetry::MAX_PEER_LOCATIONS; ++index) {
        Telemetry::CustomLocationMeta meta{};
        if (index + 1 == Telemetry::MAX_PEER_LOCATIONS) {
            meta.has_expires = true;
            meta.expires_millis = 1000;
        }
        CHECK(store.apply(peer(static_cast<uint8_t>(index)), location(10), meta,
                          100 + index) ==
              Telemetry::PeerLocationResult::INSERTED);
    }

    CHECK(store.apply(peer(200), location(11), no_meta, 2000) ==
          Telemetry::PeerLocationResult::INSERTED);
    CHECK(hasPeer(store, peer(0)));
    CHECK(!hasPeer(store, peer(31)));
    CHECK(hasPeer(store, peer(200)));
    CHECK(store.size() == Telemetry::MAX_PEER_LOCATIONS);
}

void rejectsDirectMetadataOutsideColumbaDomains() {
    Telemetry::PeerLocationStore store;
    auto meta = metaTimestamp(1);
    meta.timestamp_millis = -1;
    CHECK(store.apply(peer(33), location(1), meta, 1) ==
          Telemetry::PeerLocationResult::INVALID_ARGUMENT);

    meta = metaTimestamp(1);
    meta.has_expires = true;
    meta.expires_millis = -1;
    CHECK(store.apply(peer(33), location(1), meta, 1) ==
          Telemetry::PeerLocationResult::INVALID_ARGUMENT);

    meta = metaTimestamp(1);
    meta.has_approx_radius = true;
    meta.approx_radius_meters = -1;
    CHECK(store.apply(peer(33), location(1), meta, 1) ==
          Telemetry::PeerLocationResult::INVALID_ARGUMENT);
    CHECK(store.size() == 0);
}

void expiredNewerUpdateClearsExistingState() {
    Telemetry::PeerLocationStore store;
    const auto id = peer(40);
    Telemetry::CustomLocationMeta no_meta{};
    CHECK(store.apply(id, location(10), no_meta, 1000) ==
          Telemetry::PeerLocationResult::INSERTED);

    auto expired = metaTimestamp(11000);
    expired.has_expires = true;
    expired.expires_millis = 1999;
    CHECK(store.apply(id, location(11), expired, 2000) ==
          Telemetry::PeerLocationResult::EXPIRED);
    CHECK(!hasPeer(store, id));
    CHECK(store.size() == 0);
}

void snapshotsAreCallerOwnedAndCapacityBounded() {
    Telemetry::PeerLocationStore store;
    Telemetry::CustomLocationMeta no_meta{};
    CHECK(store.apply(peer(1), location(1, 10), no_meta, 1) ==
          Telemetry::PeerLocationResult::INSERTED);
    CHECK(store.apply(peer(2), location(2, 20), no_meta, 2) ==
          Telemetry::PeerLocationResult::INSERTED);

    Telemetry::PeerLocationRecord snapshot[1]{};
    CHECK(store.snapshot(2, std::numeric_limits<uint64_t>::max(), snapshot, 1) == 1);
    CHECK(snapshot[0].location.latitude_e6 == 10);
    CHECK(store.apply(peer(1), location(3, 99), no_meta, 3) ==
          Telemetry::PeerLocationResult::UPDATED);
    CHECK(snapshot[0].location.latitude_e6 == 10);
}

void rejectsTimestampOverflowWithoutMutation() {
    Telemetry::PeerLocationStore store;
    Telemetry::CustomLocationMeta no_meta{};
    auto invalid = location(std::numeric_limits<uint64_t>::max());
    CHECK(store.apply(peer(50), invalid, no_meta, 1) ==
          Telemetry::PeerLocationResult::INVALID_ARGUMENT);
    CHECK(store.size() == 0);
}

void survivesDeterministicHundredThousandOperationStress() {
    Telemetry::PeerLocationStore store;
    CHECK(sizeof(store) <= 4096);
    uint32_t state = 0x12345678U;
    for (std::size_t operation = 0; operation < 100000; ++operation) {
        state = state * 1664525U + 1013904223U;
        const auto id = peer(static_cast<uint8_t>(state >> 24U));
        Telemetry::CustomLocationMeta meta{};
        meta.has_timestamp = true;
        meta.timestamp_millis = operation;
        if ((state & 0x3fU) == 0) {
            meta.has_cease = true;
            meta.cease = true;
        }
        if ((state & 0x1fU) == 1) {
            meta.has_expires = true;
            meta.expires_millis = operation + 10;
        }
        store.apply(id, location(operation / 1000U), meta, operation);
        if ((operation % 257U) == 0) {
            store.prune(operation, 1000);
        }
        CHECK(store.size() <= Telemetry::MAX_PEER_LOCATIONS);
    }
}

}  // namespace

int main() {
    insertsUpdatesAndRejectsStaleData();
    appliesOrderedCeaseWithoutTouchingOtherPeers();
    reusesVacanciesBeforeDeterministicEviction();
    enforcesExpiryAndStaleDisplayBoundaries();
    basesFreshnessOnSenderCaptureTimeNotReceiptTime();
    treatsPresentEpochZeroExpiryAsExpired();
    reusesExpiredSlotsBeforeEvictingLiveRecords();
    rejectsDirectMetadataOutsideColumbaDomains();
    expiredNewerUpdateClearsExistingState();
    snapshotsAreCallerOwnedAndCapacityBounded();
    rejectsTimestampOverflowWithoutMutation();
    survivesDeterministicHundredThousandOperationStress();
    std::cout << "location share state: " << passed << " passed, "
              << failures << " failed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
