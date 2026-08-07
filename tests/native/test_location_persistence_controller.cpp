#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "Telemetry/LocationPersistenceController.h"

namespace {
int passed = 0;
int failures = 0;
#define CHECK(expr) do { if (expr) { ++passed; } else { ++failures; std::cerr << "FAIL line " << __LINE__ << ": " #expr << '\n'; } } while (false)

constexpr uint64_t WALL = Telemetry::TRUSTED_WALL_CLOCK_MIN_MILLIS + 100000;

Telemetry::PeerId peer(uint8_t seed) {
    Telemetry::PeerId id{};
    for (std::size_t i = 0; i < Telemetry::PEER_ID_SIZE; ++i) id.bytes[i] = static_cast<uint8_t>(seed + i);
    return id;
}

Telemetry::LocationTelemetry location(uint64_t seconds) {
    Telemetry::LocationTelemetry value{};
    value.latitude_e6 = 123;
    value.longitude_e6 = -456;
    value.timestamp_seconds = seconds;
    value.sensor_timestamp_seconds = seconds;
    return value;
}

class MemoryStorage : public Telemetry::LocationPersistenceStorage {
public:
    struct Slot {
        bool exists = false;
        std::size_t size = 0;
        uint8_t bytes[Telemetry::MAX_LOCATION_STATE_RECORD_BYTES]{};
    } slots[3];
    bool mounted = true;
    bool fail_io = false;
    int stat_calls = 0;
    int write_calls = 0;

    bool available() const override { return mounted; }
    bool stat(Telemetry::LocationPersistenceSlot slot, bool& exists) override {
        ++stat_calls;
        exists = false;
        if (fail_io || !mounted) return false;
        exists = slots[static_cast<int>(slot)].exists;
        return true;
    }
    bool read(Telemetry::LocationPersistenceSlot slot, uint8_t* out,
              std::size_t capacity, std::size_t& size) override {
        size = 0;
        Slot& value = slots[static_cast<int>(slot)];
        if (fail_io || !value.exists || value.size > capacity) return false;
        std::memcpy(out, value.bytes, value.size);
        size = value.size;
        return true;
    }
    bool write(Telemetry::LocationPersistenceSlot slot, const uint8_t* data,
               std::size_t size) override {
        ++write_calls;
        if (fail_io || size > sizeof(slots[0].bytes)) return false;
        Slot& value = slots[static_cast<int>(slot)];
        value.exists = true;
        value.size = size;
        std::memcpy(value.bytes, data, size);
        return true;
    }
    bool remove(Telemetry::LocationPersistenceSlot slot) override {
        if (fail_io) return false;
        slots[static_cast<int>(slot)] = Slot{};
        return true;
    }
    bool rename(Telemetry::LocationPersistenceSlot from,
                Telemetry::LocationPersistenceSlot to) override {
        if (fail_io) return false;
        slots[static_cast<int>(to)] = slots[static_cast<int>(from)];
        slots[static_cast<int>(from)] = Slot{};
        return true;
    }
};

void put(MemoryStorage& storage, const Telemetry::LocationStateSnapshot& state) {
    std::size_t size = 0;
    CHECK(Telemetry::encodeLocationStateRecord(
              state, storage.slots[0].bytes, sizeof(storage.slots[0].bytes), size) ==
          Telemetry::LocationStateRecordResult::OK);
    storage.slots[0].exists = true;
    storage.slots[0].size = size;
}

void clockGateRestoreAndPrune() {
    MemoryStorage storage;
    Telemetry::LocationStateSnapshot saved{};
    saved.session_count = 2;
    saved.sessions[0].peer = peer(1);
    saved.sessions[0].record.has_expiry = true;
    saved.sessions[0].record.expires_at_millis = WALL - 1;
    saved.sessions[1].peer = peer(2);
    saved.sessions[1].record.cease_pending = true;
    saved.location_count = 1;
    saved.locations[0].peer = peer(3);
    saved.locations[0].location = location(WALL / 1000);
    saved.locations[0].source_timestamp_millis = WALL;
    saved.locations[0].received_at_millis = WALL;
    saved.locations[0].has_approx_radius = true;
    put(storage, saved);

    Telemetry::PeerLocationStore peers;
    Telemetry::LocationShareScheduler shares;
    Telemetry::TransactionalLocationPersistence persistence(storage);
    Telemetry::LocationPersistenceController controller(shares, peers, persistence);
    CHECK(controller.service(1000, 10) == Telemetry::LocationControllerState::WAITING_FOR_CLOCK);
    CHECK(storage.stat_calls == 0);
    CHECK(controller.service(WALL, 20) == Telemetry::LocationControllerState::READY);
    CHECK(shares.size() == 1);
    Telemetry::ShareSession session{};
    CHECK(shares.get(peer(2), session) && session.cease_pending);
    Telemetry::PeerLocationRecord record{};
    CHECK(peers.get(peer(3), record));
    CHECK(record.has_approx_radius && record.approx_radius_meters == 0);
    CHECK(controller.service(1000, 30) ==
          Telemetry::LocationControllerState::BLOCKED);
}

void unavailableCorruptAndIoRetryFailClosed() {
    MemoryStorage unavailable;
    unavailable.mounted = false;
    Telemetry::PeerLocationStore p1;
    Telemetry::LocationShareScheduler s1;
    Telemetry::TransactionalLocationPersistence x1(unavailable);
    Telemetry::LocationPersistenceController c1(s1, p1, x1);
    CHECK(c1.service(WALL, 1) == Telemetry::LocationControllerState::BLOCKED);

    MemoryStorage corrupt;
    corrupt.slots[0].exists = true;
    corrupt.slots[0].size = 20;
    Telemetry::PeerLocationStore p2;
    Telemetry::LocationShareScheduler s2;
    Telemetry::TransactionalLocationPersistence x2(corrupt);
    Telemetry::LocationPersistenceController c2(s2, p2, x2);
    CHECK(c2.service(WALL, 1) == Telemetry::LocationControllerState::BLOCKED);

    MemoryStorage retry;
    retry.fail_io = true;
    Telemetry::PeerLocationStore p3;
    Telemetry::LocationShareScheduler s3;
    Telemetry::TransactionalLocationPersistence x3(retry);
    Telemetry::LocationPersistenceController c3(s3, p3, x3);
    CHECK(c3.service(WALL, 100) == Telemetry::LocationControllerState::WAITING_FOR_CLOCK);
    const int calls = retry.stat_calls;
    retry.fail_io = false;
    CHECK(c3.service(WALL, 5099) == Telemetry::LocationControllerState::WAITING_FOR_CLOCK);
    CHECK(retry.stat_calls == calls);
    CHECK(c3.service(WALL, 5100) == Telemetry::LocationControllerState::READY);
}

void dirtyCadenceIsNonSlidingAndRetries() {
    MemoryStorage storage;
    Telemetry::PeerLocationStore peers;
    Telemetry::LocationShareScheduler shares;
    Telemetry::TransactionalLocationPersistence persistence(storage);
    Telemetry::LocationPersistenceController controller(shares, peers, persistence);
    CHECK(controller.service(WALL, 100) == Telemetry::LocationControllerState::READY);
    Telemetry::CustomLocationMeta meta{};
    CHECK(peers.apply(peer(4), location(WALL / 1000), meta, WALL) ==
          Telemetry::PeerLocationResult::INSERTED);
    CHECK(controller.service(WALL, 200) == Telemetry::LocationControllerState::READY);
    CHECK(storage.write_calls == 0);
    CHECK(peers.apply(peer(4), location(WALL / 1000 + 1), meta, WALL + 1) ==
          Telemetry::PeerLocationResult::UPDATED);
    CHECK(controller.service(WALL + 1, 5199) == Telemetry::LocationControllerState::READY);
    CHECK(storage.write_calls == 0);
    storage.fail_io = true;
    const int failed_attempt_calls = storage.stat_calls;
    CHECK(controller.service(WALL + 1, 5200) == Telemetry::LocationControllerState::READY);
    CHECK(storage.stat_calls > failed_attempt_calls);
    storage.fail_io = false;
    const int retry_wait_calls = storage.stat_calls;
    CHECK(controller.service(WALL + 1, 10199) == Telemetry::LocationControllerState::READY);
    CHECK(storage.stat_calls == retry_wait_calls);
    CHECK(controller.service(WALL + 1, 10200) == Telemetry::LocationControllerState::READY);
    CHECK(storage.write_calls >= 1);
    CHECK(!controller.dirty());
}

void urgentConsentSaveRollsBackAndRollbackClockBlocks() {
    MemoryStorage storage;
    Telemetry::PeerLocationStore peers;
    Telemetry::LocationShareScheduler shares;
    Telemetry::TransactionalLocationPersistence persistence(storage);
    Telemetry::LocationPersistenceController controller(shares, peers, persistence);
    CHECK(controller.service(WALL, 100) == Telemetry::LocationControllerState::READY);
    Telemetry::ShareStartOptions options{};
    options.duration = Telemetry::ShareDuration::INDEFINITE;
    storage.fail_io = true;
    CHECK(controller.startSharing(peer(5), options, WALL, 101) ==
          Telemetry::LocationConsentResult::STORAGE_FAILURE);
    CHECK(shares.size() == 0);
    storage.fail_io = false;
    CHECK(controller.startSharing(peer(5), options, WALL, 102) ==
          Telemetry::LocationConsentResult::STARTED);
    CHECK(shares.size() == 1);
    storage.fail_io = true;
    CHECK(controller.stopSharing(peer(5), WALL + 1, 103) ==
          Telemetry::LocationConsentResult::STORAGE_FAILURE);
    Telemetry::ShareSession session{};
    CHECK(shares.get(peer(5), session) && !session.cease_pending);
    CHECK(controller.service(WALL + 2, 102) == Telemetry::LocationControllerState::BLOCKED);
}

void failedConsentRestoresWholeScheduler() {
    MemoryStorage storage;
    Telemetry::PeerLocationStore peers;
    Telemetry::LocationShareScheduler shares;
    Telemetry::TransactionalLocationPersistence persistence(storage);
    Telemetry::LocationPersistenceController controller(shares, peers, persistence);
    CHECK(controller.service(WALL, 100) == Telemetry::LocationControllerState::READY);
    Telemetry::ShareStartOptions options{};
    options.duration = Telemetry::ShareDuration::INDEFINITE;
    CHECK(controller.startSharing(peer(6), options, WALL + 1000, 101) ==
          Telemetry::LocationConsentResult::STARTED);
    Telemetry::ShareSession before{};
    CHECK(shares.get(peer(6), before));
    storage.fail_io = true;
    CHECK(controller.startSharing(peer(5), options, WALL, 102) ==
          Telemetry::LocationConsentResult::STORAGE_FAILURE);
    Telemetry::ShareSession after{};
    CHECK(shares.get(peer(6), after));
    CHECK(after.next_attempt_millis == before.next_attempt_millis);
    CHECK(after.last_sent_millis == before.last_sent_millis);
}
}  // namespace

int main() {
    clockGateRestoreAndPrune();
    unavailableCorruptAndIoRetryFailClosed();
    dirtyCadenceIsNonSlidingAndRetries();
    urgentConsentSaveRollsBackAndRollbackClockBlocks();
    failedConsentRestoresWholeScheduler();
    std::cout << "location persistence controller: " << passed << " passed, "
              << failures << " failed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
