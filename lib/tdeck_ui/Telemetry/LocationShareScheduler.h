#ifndef PYXIS_TELEMETRY_LOCATION_SHARE_SCHEDULER_H
#define PYXIS_TELEMETRY_LOCATION_SHARE_SCHEDULER_H

#include <cstddef>
#include <cstdint>

#include "LocationShareState.h"

namespace Telemetry {

constexpr std::size_t MAX_SHARE_SESSIONS = 32;
constexpr uint32_t MIN_SHARE_CADENCE_MILLIS = 1000;
constexpr uint32_t MAX_SHARE_CADENCE_MILLIS = 24U * 60U * 60U * 1000U;
constexpr uint64_t INITIAL_RETRY_MILLIS = 5000;
constexpr uint64_t MAX_RETRY_MILLIS = 5ULL * 60ULL * 1000ULL;
constexpr uint64_t ACKNOWLEDGEMENT_LEASE_MILLIS = 1000;

enum class ShareDuration : uint8_t {
    MINUTES_15,
    HOUR_1,
    HOURS_4,
    LOCAL_MIDNIGHT,
    INDEFINITE,
};

enum class ShareSessionResult : uint8_t {
    STARTED,
    UPDATED,
    RESTORED,
    STOPPING,
    EXPIRED,
    NOT_FOUND,
    CAPACITY,
    CLOCK_UNAVAILABLE,
    INVALID_ARGUMENT,
    BUSY,
};

enum class ShareWorkType : uint8_t {
    LOCATION,
    CEASE,
};

enum class SharePollResult : uint8_t {
    WORK,
    NO_WORK,
    CLOCK_UNAVAILABLE,
};

enum class ShareAckResult : uint8_t {
    ACCEPTED,
    CEASED,
    RETRY_SCHEDULED,
    STALE_TOKEN,
    NOT_FOUND,
    CLOCK_UNAVAILABLE,
};

enum class ShareSnapshotResult : uint8_t {
    OK,
    BUFFER_TOO_SMALL,
    INVALID_ARGUMENT,
};

enum class MidnightResult : uint8_t {
    OK,
    CLOCK_UNAVAILABLE,
    INVALID_ARGUMENT,
    OVERFLOW,
};

struct ShareStartOptions {
    ShareDuration duration = ShareDuration::MINUTES_15;
    uint32_t cadence_millis = 60000;
    bool has_approx_radius = false;
    int32_t approx_radius_meters = 0;
    // Required only for LOCAL_MIDNIGHT. The platform resolves the exact
    // DST-aware local boundary before calling the scheduler.
    uint64_t local_midnight_millis = 0;
};

struct ShareRestoreRecord {
    uint32_t cadence_millis = 60000;
    bool has_approx_radius = false;
    int32_t approx_radius_meters = 0;
    bool has_expiry = false;
    uint64_t expires_at_millis = 0;
    bool cease_pending = false;
};

struct ShareRestoreEntry {
    PeerId peer{};
    ShareRestoreRecord record{};
};

struct ShareSession {
    PeerId peer{};
    uint32_t cadence_millis = 60000;
    bool has_approx_radius = false;
    int32_t approx_radius_meters = 0;
    bool has_expiry = false;
    uint64_t expires_at_millis = 0;
    bool has_sent = false;
    uint64_t last_sent_millis = 0;
    uint64_t next_attempt_millis = 0;
    bool cease_pending = false;
    bool awaiting_ack = false;
    ShareWorkType pending_type = ShareWorkType::LOCATION;
    uint64_t pending_token = 0;
    uint64_t ack_deadline_monotonic_millis = 0;
    uint8_t failure_count = 0;
};

struct ShareWork {
    PeerId peer{};
    ShareWorkType type = ShareWorkType::LOCATION;
    uint64_t token = 0;
    uint64_t ack_deadline_monotonic_millis = 0;
    bool has_expiry = false;
    uint64_t expires_at_millis = 0;
    bool has_approx_radius = false;
    int32_t approx_radius_meters = 0;
};

// Calculates midnight for a fixed UTC offset. The caller must provide the
// offset effective at the target boundary; platform code should resolve DST.
MidnightResult nextFixedOffsetMidnight(
    uint64_t now_millis,
    int32_t utc_offset_seconds,
    uint64_t& output_millis);

class LocationShareScheduler {
public:
    LocationShareScheduler() = default;

    ShareSessionResult start(
        const PeerId& peer,
        const ShareStartOptions& options,
        uint64_t now_millis);

    ShareSessionResult restore(
        const PeerId& peer,
        const ShareRestoreRecord& record,
        uint64_t now_millis);

    ShareSessionResult stop(const PeerId& peer, uint64_t now_millis);
    bool cancelWithoutCease(const PeerId& peer);

    // WORK is a short exclusive queue-attempt lease. The caller must attempt
    // queueing synchronously, acknowledge before the advertised monotonic
    // deadline, and must
    // never enqueue the work after that deadline. stop() orders CEASE behind
    // an in-flight LOCATION; start()/restore() report BUSY instead of
    // invalidating externally borrowed work. monotonic_now_millis must come
    // from a non-wall-clock source that cannot move backward during a boot.
    SharePollResult poll(
        uint64_t wall_now_millis,
        uint64_t monotonic_now_millis,
        bool current_location_valid,
        ShareWork& output);

    ShareAckResult acknowledge(
        const PeerId& peer,
        uint64_t token,
        bool queue_accepted,
        uint64_t wall_now_millis,
        uint64_t monotonic_now_millis);

    bool get(const PeerId& peer, ShareSession& output) const;
    // Atomic: BUFFER_TOO_SMALL writes no entries and reports the required
    // capacity in written_or_required.
    ShareSnapshotResult snapshot(
        ShareRestoreEntry* output,
        std::size_t capacity,
        std::size_t& written_or_required) const;
    std::size_t size() const { return size_; }

private:
    struct Slot {
        bool occupied = false;
        ShareSession session{};
    };

    static bool peerEquals(const PeerId& left, const PeerId& right);
    static bool validCadence(uint32_t cadence_millis);
    static uint64_t boundedAdd(uint64_t value, uint64_t delta);
    static uint64_t retryDelay(uint8_t failure_count);
    static void scheduleRejectedWork(
        ShareSession& session,
        ShareWorkType type,
        uint64_t now_millis);
    std::size_t find(const PeerId& peer) const;
    std::size_t firstVacant() const;
    uint64_t nextToken();
    void clear(std::size_t index);
    void observeClock(uint64_t now_millis);
    bool observeMonotonic(uint64_t now_millis);

    Slot slots_[MAX_SHARE_SESSIONS]{};
    std::size_t size_ = 0;
    uint64_t next_token_ = 1;
    uint64_t last_observed_millis_ = 0;
    uint64_t last_monotonic_millis_ = 0;
    bool has_monotonic_observation_ = false;
};

}  // namespace Telemetry

#endif  // PYXIS_TELEMETRY_LOCATION_SHARE_SCHEDULER_H
