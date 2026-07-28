#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "Telemetry/LocationPersistence.h"

namespace {

int passed = 0;
int failures = 0;
#define CHECK(expr) do { if (expr) { ++passed; } else { ++failures; \
    std::cerr << "FAIL line " << __LINE__ << ": " #expr << '\n'; } } while (false)

struct SlotData {
    bool present = false;
    std::size_t size = 0;
    uint8_t bytes[Telemetry::MAX_LOCATION_STATE_RECORD_BYTES]{};
};

class FakeStorage : public Telemetry::LocationPersistenceStorage {
public:
    bool mounted = true;
    int operations = 0;
    int fail_at = 0;
    bool corrupt_write = false;
    SlotData slots[3]{};

    bool available() const override { return mounted; }

    bool stat(Telemetry::LocationPersistenceSlot slot, bool& exists) override {
        if (fails()) return false;
        exists = at(slot).present;
        return true;
    }

    bool read(Telemetry::LocationPersistenceSlot slot, uint8_t* output,
              std::size_t capacity, std::size_t& size) override {
        if (fails()) return false;
        const SlotData& source = at(slot);
        if (!source.present || source.size > capacity) return false;
        std::memcpy(output, source.bytes, source.size);
        size = source.size;
        return true;
    }

    bool write(Telemetry::LocationPersistenceSlot slot, const uint8_t* data,
               std::size_t size) override {
        if (fails()) {
            SlotData& target = at(slot);
            target.present = true;
            target.size = size / 2;
            if (target.size > 0) std::memcpy(target.bytes, data, target.size);
            return false;
        }
        SlotData& target = at(slot);
        target.present = true;
        target.size = size;
        std::memcpy(target.bytes, data, size);
        if (corrupt_write && size > 20) target.bytes[20] ^= 1U;
        return true;
    }

    bool remove(Telemetry::LocationPersistenceSlot slot) override {
        if (fails()) return false;
        at(slot) = SlotData{};
        return true;
    }

    bool rename(Telemetry::LocationPersistenceSlot from,
                Telemetry::LocationPersistenceSlot to) override {
        if (fails()) return false;
        if (!at(from).present || at(to).present) return false;
        at(to) = at(from);
        at(from) = SlotData{};
        return true;
    }

private:
    bool fails() {
        ++operations;
        return fail_at != 0 && operations == fail_at;
    }
    SlotData& at(Telemetry::LocationPersistenceSlot slot) {
        return slots[static_cast<std::size_t>(slot)];
    }
    const SlotData& at(Telemetry::LocationPersistenceSlot slot) const {
        return slots[static_cast<std::size_t>(slot)];
    }
};

Telemetry::LocationStateSnapshot state(uint32_t marker) {
    Telemetry::LocationStateSnapshot value{};
    value.session_count = 1;
    for (std::size_t i = 0; i < Telemetry::PEER_ID_SIZE; ++i) {
        value.sessions[0].peer.bytes[i] = static_cast<uint8_t>(marker + i);
    }
    value.sessions[0].record.cadence_millis =
        Telemetry::MIN_SHARE_CADENCE_MILLIS + marker;
    value.sessions[0].record.approx_radius_meters = static_cast<int32_t>(marker);
    value.location_count = 1;
    value.locations[0].peer = value.sessions[0].peer;
    value.locations[0].location.latitude_e6 = 1000000 + static_cast<int32_t>(marker);
    value.locations[0].location.longitude_e6 = -2000000;
    value.locations[0].source_timestamp_millis = marker;
    value.locations[0].received_at_millis = marker + 1;
    return value;
}

uint32_t marker(const Telemetry::LocationStateSnapshot& value) {
    return value.sessions[0].record.cadence_millis -
           Telemetry::MIN_SHARE_CADENCE_MILLIS;
}

void put(FakeStorage& storage, Telemetry::LocationPersistenceSlot slot,
         const Telemetry::LocationStateSnapshot& value) {
    SlotData& target = storage.slots[static_cast<std::size_t>(slot)];
    target.present = true;
    std::size_t written = 0;
    CHECK(Telemetry::encodeLocationStateRecord(
              value, target.bytes, sizeof(target.bytes), written) ==
          Telemetry::LocationStateRecordResult::OK);
    target.size = written;
}

void recoversByLiveTempBackupPriority() {
    FakeStorage storage;
    put(storage, Telemetry::LocationPersistenceSlot::LIVE, state(1));
    put(storage, Telemetry::LocationPersistenceSlot::TEMP, state(2));
    put(storage, Telemetry::LocationPersistenceSlot::BACKUP, state(3));
    Telemetry::TransactionalLocationPersistence persistence(storage);
    Telemetry::LocationStateSnapshot output{};
    CHECK(persistence.load(output) == Telemetry::LocationPersistenceResult::LOADED_LIVE);
    CHECK(marker(output) == 1);

    storage.slots[0].bytes[20] ^= 1U;
    CHECK(persistence.load(output) == Telemetry::LocationPersistenceResult::RECOVERED_TEMP);
    CHECK(marker(output) == 2);
    CHECK(storage.slots[0].present);

    FakeStorage backup_only;
    put(backup_only, Telemetry::LocationPersistenceSlot::LIVE, state(4));
    backup_only.slots[0].bytes[20] ^= 1U;
    put(backup_only, Telemetry::LocationPersistenceSlot::TEMP, state(5));
    backup_only.slots[1].bytes[20] ^= 1U;
    put(backup_only, Telemetry::LocationPersistenceSlot::BACKUP, state(6));
    Telemetry::TransactionalLocationPersistence backup_persistence(backup_only);
    CHECK(backup_persistence.load(output) ==
          Telemetry::LocationPersistenceResult::RECOVERED_BACKUP);
    CHECK(marker(output) == 6);
    CHECK(backup_only.slots[2].present);
}

void failsClosedWhenUnavailableMissingOrCorrupt() {
    Telemetry::LocationStateSnapshot output = state(9);
    FakeStorage unavailable;
    unavailable.mounted = false;
    Telemetry::TransactionalLocationPersistence unavailable_persistence(unavailable);
    CHECK(unavailable_persistence.load(output) ==
          Telemetry::LocationPersistenceResult::UNAVAILABLE);
    CHECK(marker(output) == 9);

    FakeStorage empty;
    Telemetry::TransactionalLocationPersistence empty_persistence(empty);
    CHECK(empty_persistence.load(output) ==
          Telemetry::LocationPersistenceResult::NOT_FOUND);
    CHECK(marker(output) == 9);

    FakeStorage corrupt;
    put(corrupt, Telemetry::LocationPersistenceSlot::LIVE, state(1));
    put(corrupt, Telemetry::LocationPersistenceSlot::TEMP, state(2));
    put(corrupt, Telemetry::LocationPersistenceSlot::BACKUP, state(3));
    for (auto& slot : corrupt.slots) slot.bytes[20] ^= 1U;
    Telemetry::TransactionalLocationPersistence corrupt_persistence(corrupt);
    CHECK(corrupt_persistence.load(output) ==
          Telemetry::LocationPersistenceResult::INVALID_STATE);
    CHECK(marker(output) == 9);
}

void everyInterruptedSaveRetainsAValidGeneration() {
    FakeStorage baseline;
    put(baseline, Telemetry::LocationPersistenceSlot::LIVE, state(10));
    put(baseline, Telemetry::LocationPersistenceSlot::BACKUP, state(9));
    int observed_failures = 0;
    for (int fail_at = 1; fail_at <= 16; ++fail_at) {
        FakeStorage interrupted = baseline;
        interrupted.fail_at = fail_at;
        Telemetry::TransactionalLocationPersistence persistence(interrupted);
        const auto result = persistence.save(state(20));
        if (result != Telemetry::LocationPersistenceResult::SAVED) {
            ++observed_failures;
        }
        interrupted.fail_at = 0;
        interrupted.operations = 0;
        Telemetry::LocationStateSnapshot output{};
        const auto loaded = persistence.load(output);
        CHECK(loaded == Telemetry::LocationPersistenceResult::LOADED_LIVE ||
              loaded == Telemetry::LocationPersistenceResult::RECOVERED_TEMP ||
              loaded == Telemetry::LocationPersistenceResult::RECOVERED_BACKUP);
        CHECK(marker(output) == 10 || marker(output) == 20);
    }
    CHECK(observed_failures >= 8);
}

void validatesTempBeforeReplacingLive() {
    FakeStorage storage;
    put(storage, Telemetry::LocationPersistenceSlot::LIVE, state(30));
    storage.corrupt_write = true;
    Telemetry::TransactionalLocationPersistence persistence(storage);
    CHECK(persistence.save(state(40)) ==
          Telemetry::LocationPersistenceResult::INVALID_STATE);
    storage.corrupt_write = false;
    Telemetry::LocationStateSnapshot output{};
    CHECK(persistence.load(output) == Telemetry::LocationPersistenceResult::LOADED_LIVE);
    CHECK(marker(output) == 30);
}

}  // namespace

int main() {
    recoversByLiveTempBackupPriority();
    failsClosedWhenUnavailableMissingOrCorrupt();
    everyInterruptedSaveRetainsAValidGeneration();
    validatesTempBeforeReplacingLive();
    std::cout << "location persistence: " << passed << " passed, "
              << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
