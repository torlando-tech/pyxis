#include "LocationShareState.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace Telemetry {
namespace {

constexpr std::size_t NO_SLOT = MAX_PEER_LOCATIONS;

bool effectiveTimestampMillis(
    const LocationTelemetry& location,
    const CustomLocationMeta& meta,
    uint64_t& timestamp_millis) {
    if (meta.has_timestamp) {
        if (meta.timestamp_millis < 0) return false;
        timestamp_millis = static_cast<uint64_t>(meta.timestamp_millis);
        return true;
    }
    if (location.timestamp_seconds >
        std::numeric_limits<uint64_t>::max() / 1000ULL) {
        return false;
    }
    timestamp_millis = location.timestamp_seconds * 1000ULL;
    return true;
}

bool locationInRange(const LocationTelemetry& location) {
    return location.latitude_e6 >= -90000000 &&
           location.latitude_e6 <= 90000000 &&
           location.longitude_e6 >= -180000000 &&
           location.longitude_e6 <= 180000000;
}

}  // namespace

bool isValidPeerLocationInput(
    const LocationTelemetry& location,
    const CustomLocationMeta& meta,
    uint64_t received_at_millis) {
    if ((meta.has_expires && meta.expires_millis < 0) ||
        (meta.has_approx_radius && meta.approx_radius_meters < 0)) {
        return false;
    }
    uint64_t source_timestamp_millis = 0;
    if (!effectiveTimestampMillis(location, meta, source_timestamp_millis)) {
        return false;
    }
    if (meta.has_cease && meta.cease) return true;
    if (meta.has_expires &&
        received_at_millis >= static_cast<uint64_t>(meta.expires_millis)) {
        return true;
    }
    return locationInRange(location);
}

bool PeerLocationStore::peerEquals(const PeerId& left, const PeerId& right) {
    return std::memcmp(left.bytes, right.bytes, PEER_ID_SIZE) == 0;
}

std::size_t PeerLocationStore::find(const PeerId& peer) const {
    for (std::size_t index = 0; index < MAX_PEER_LOCATIONS; ++index) {
        if (slots_[index].occupied && peerEquals(slots_[index].record.peer, peer)) {
            return index;
        }
    }
    return NO_SLOT;
}

std::size_t PeerLocationStore::firstVacant() const {
    for (std::size_t index = 0; index < MAX_PEER_LOCATIONS; ++index) {
        if (!slots_[index].occupied) return index;
    }
    return NO_SLOT;
}

std::size_t PeerLocationStore::evictionCandidate(uint64_t now_millis) const {
    for (std::size_t index = 0; index < MAX_PEER_LOCATIONS; ++index) {
        if (slots_[index].occupied &&
            slots_[index].record.has_expiry &&
            now_millis >= slots_[index].record.expires_at_millis) {
            return index;
        }
    }

    std::size_t candidate = NO_SLOT;
    for (std::size_t index = 0; index < MAX_PEER_LOCATIONS; ++index) {
        if (!slots_[index].occupied) continue;
        if (candidate == NO_SLOT ||
            slots_[index].record.received_at_millis <
                slots_[candidate].record.received_at_millis) {
            candidate = index;
        }
    }
    return candidate;
}

bool PeerLocationStore::visible(
    const PeerLocationRecord& record,
    uint64_t now_millis,
    uint64_t maximum_age_millis) {
    if (record.has_expiry &&
        now_millis >= record.expires_at_millis) {
        return false;
    }
    if (now_millis >= record.source_timestamp_millis &&
        now_millis - record.source_timestamp_millis > maximum_age_millis) {
        return false;
    }
    return true;
}

void PeerLocationStore::clear(std::size_t index) {
    if (index >= MAX_PEER_LOCATIONS || !slots_[index].occupied) return;
    slots_[index] = Slot{};
    --size_;
}

PeerLocationResult PeerLocationStore::apply(
    const PeerId& peer,
    const LocationTelemetry& location,
    const CustomLocationMeta& meta,
    uint64_t received_at_millis) {
    if (!isValidPeerLocationInput(location, meta, received_at_millis)) {
        return PeerLocationResult::INVALID_ARGUMENT;
    }

    uint64_t source_timestamp_millis = 0;
    if (!effectiveTimestampMillis(location, meta, source_timestamp_millis)) {
        return PeerLocationResult::INVALID_ARGUMENT;
    }

    const std::size_t existing = find(peer);
    if (existing != NO_SLOT &&
        source_timestamp_millis <
            slots_[existing].record.source_timestamp_millis) {
        return PeerLocationResult::STALE;
    }

    if (meta.has_cease && meta.cease) {
        if (existing == NO_SLOT) return PeerLocationResult::NOT_FOUND;
        clear(existing);
        return PeerLocationResult::CEASED;
    }

    const uint64_t expires_at_millis = meta.has_expires
                                           ? static_cast<uint64_t>(meta.expires_millis)
                                           : 0;
    if (meta.has_expires &&
        received_at_millis >= expires_at_millis) {
        if (existing != NO_SLOT) clear(existing);
        return PeerLocationResult::EXPIRED;
    }
    if (!locationInRange(location)) {
        return PeerLocationResult::INVALID_ARGUMENT;
    }

    PeerLocationRecord record{};
    record.peer = peer;
    record.location = location;
    record.source_timestamp_millis = source_timestamp_millis;
    record.received_at_millis = received_at_millis;
    record.has_expiry = meta.has_expires;
    record.expires_at_millis = expires_at_millis;
    record.approx_radius_meters =
        meta.has_approx_radius
            ? static_cast<uint32_t>(meta.approx_radius_meters)
            : 0;

    if (existing != NO_SLOT) {
        slots_[existing].record = record;
        return PeerLocationResult::UPDATED;
    }

    std::size_t target = firstVacant();
    if (target == NO_SLOT) target = evictionCandidate(received_at_millis);
    if (target == NO_SLOT) return PeerLocationResult::INVALID_ARGUMENT;
    if (!slots_[target].occupied) ++size_;
    slots_[target].record = record;
    slots_[target].occupied = true;
    return PeerLocationResult::INSERTED;
}

bool PeerLocationStore::get(
    const PeerId& peer,
    PeerLocationRecord& output) const {
    const std::size_t index = find(peer);
    if (index == NO_SLOT) return false;
    output = slots_[index].record;
    return true;
}

std::size_t PeerLocationStore::snapshot(
    uint64_t now_millis,
    uint64_t maximum_age_millis,
    PeerLocationRecord* output,
    std::size_t capacity) const {
    if (output == nullptr || capacity == 0) return 0;
    std::size_t copied = 0;
    for (std::size_t index = 0;
         index < MAX_PEER_LOCATIONS && copied < capacity;
         ++index) {
        if (!slots_[index].occupied ||
            !visible(slots_[index].record, now_millis, maximum_age_millis)) {
            continue;
        }
        output[copied++] = slots_[index].record;
    }
    return copied;
}

std::size_t PeerLocationStore::prune(
    uint64_t now_millis,
    uint64_t maximum_age_millis) {
    std::size_t removed = 0;
    for (std::size_t index = 0; index < MAX_PEER_LOCATIONS; ++index) {
        if (slots_[index].occupied &&
            !visible(slots_[index].record, now_millis, maximum_age_millis)) {
            clear(index);
            ++removed;
        }
    }
    return removed;
}

}  // namespace Telemetry
