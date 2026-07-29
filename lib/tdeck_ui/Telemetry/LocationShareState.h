#ifndef PYXIS_TELEMETRY_LOCATION_SHARE_STATE_H
#define PYXIS_TELEMETRY_LOCATION_SHARE_STATE_H

#include <cstddef>
#include <cstdint>

#include "LocationTelemetryCodec.h"

namespace Telemetry {

constexpr std::size_t PEER_ID_SIZE = 16;
constexpr std::size_t MAX_PEER_LOCATIONS = 32;

struct PeerId {
    PeerId() : bytes{} {}
    uint8_t bytes[PEER_ID_SIZE];
};

struct PeerLocationRecord {
    PeerId peer{};
    LocationTelemetry location{};
    uint64_t source_timestamp_millis = 0;
    uint64_t received_at_millis = 0;
    bool has_expiry = false;
    uint64_t expires_at_millis = 0;
    bool has_approx_radius = false;
    uint32_t approx_radius_meters = 0;
};

enum class PeerLocationResult : uint8_t {
    INSERTED,
    UPDATED,
    CEASED,
    NOT_FOUND,
    STALE,
    EXPIRED,
    INVALID_ARGUMENT,
};

// Mirrors the argument-domain checks performed by PeerLocationStore::apply().
// Staleness and capacity remain store-state decisions, not wire validity.
bool isValidPeerLocationInput(
    const LocationTelemetry& location,
    const CustomLocationMeta& meta,
    uint64_t received_at_millis);

class PeerLocationStore {
public:
    PeerLocationStore() = default;

    PeerLocationResult apply(
        const PeerId& peer,
        const LocationTelemetry& location,
        const CustomLocationMeta& meta,
        uint64_t received_at_millis);

    bool get(const PeerId& peer, PeerLocationRecord& output) const;

    std::size_t snapshot(
        uint64_t now_millis,
        uint64_t maximum_age_millis,
        PeerLocationRecord* output,
        std::size_t capacity) const;

    std::size_t prune(uint64_t now_millis, uint64_t maximum_age_millis);
    std::size_t size() const { return size_; }
    uint64_t revision() const { return revision_; }
    PeerLocationResult restore(const PeerLocationRecord& record);
    std::size_t durableSnapshot(PeerLocationRecord* output,
                                std::size_t capacity) const;

private:
    struct Slot {
        bool occupied = false;
        PeerLocationRecord record{};
    };

    std::size_t find(const PeerId& peer) const;
    std::size_t firstVacant() const;
    std::size_t evictionCandidate(uint64_t now_millis) const;
    static bool peerEquals(const PeerId& left, const PeerId& right);
    static bool visible(const PeerLocationRecord& record,
                        uint64_t now_millis,
                        uint64_t maximum_age_millis);
    void clear(std::size_t index);

    Slot slots_[MAX_PEER_LOCATIONS]{};
    std::size_t size_ = 0;
    uint64_t revision_ = 0;
};

}  // namespace Telemetry

#endif  // PYXIS_TELEMETRY_LOCATION_SHARE_STATE_H
