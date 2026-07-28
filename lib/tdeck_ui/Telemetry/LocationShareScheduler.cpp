#include "LocationShareScheduler.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace Telemetry {
namespace {

constexpr std::size_t NO_SLOT = MAX_SHARE_SESSIONS;
constexpr uint64_t MILLIS_PER_DAY = 24ULL * 60ULL * 60ULL * 1000ULL;
constexpr int32_t MAX_UTC_OFFSET_SECONDS = 18 * 60 * 60;

bool expiryForDuration(
    const ShareStartOptions& options,
    uint64_t now_millis,
    bool& has_expiry,
    uint64_t& expires_at_millis) {
    uint64_t duration_millis = 0;
    has_expiry = true;
    switch (options.duration) {
        case ShareDuration::MINUTES_15:
            duration_millis = 15ULL * 60ULL * 1000ULL;
            break;
        case ShareDuration::HOUR_1:
            duration_millis = 60ULL * 60ULL * 1000ULL;
            break;
        case ShareDuration::HOURS_4:
            duration_millis = 4ULL * 60ULL * 60ULL * 1000ULL;
            break;
        case ShareDuration::LOCAL_MIDNIGHT:
            if (options.local_midnight_millis <= now_millis ||
                options.local_midnight_millis >
                    static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                return false;
            }
            expires_at_millis = options.local_midnight_millis;
            return true;
        case ShareDuration::INDEFINITE:
            has_expiry = false;
            expires_at_millis = 0;
            return true;
        default:
            return false;
    }

    const uint64_t maximum =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (now_millis > maximum - duration_millis) return false;
    expires_at_millis = now_millis + duration_millis;
    return true;
}

}  // namespace

MidnightResult nextFixedOffsetMidnight(
    uint64_t now_millis,
    int32_t utc_offset_seconds,
    uint64_t& output_millis) {
    if (now_millis == 0) return MidnightResult::CLOCK_UNAVAILABLE;
    if (utc_offset_seconds < -MAX_UTC_OFFSET_SECONDS ||
        utc_offset_seconds > MAX_UTC_OFFSET_SECONDS ||
        now_millis > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return MidnightResult::INVALID_ARGUMENT;
    }

    const int64_t now = static_cast<int64_t>(now_millis);
    const int64_t offset = static_cast<int64_t>(utc_offset_seconds) * 1000LL;
    if ((offset > 0 && now > std::numeric_limits<int64_t>::max() - offset) ||
        (offset < 0 && now < -offset)) {
        return MidnightResult::OVERFLOW;
    }
    const int64_t local_now = now + offset;
    const int64_t day = static_cast<int64_t>(MILLIS_PER_DAY);
    const int64_t day_index = local_now / day;
    if (day_index >= std::numeric_limits<int64_t>::max() / day - 1) {
        return MidnightResult::OVERFLOW;
    }
    const int64_t next_local_midnight = (day_index + 1) * day;
    if ((offset < 0 &&
         next_local_midnight > std::numeric_limits<int64_t>::max() + offset) ||
        (offset > 0 && next_local_midnight < offset)) {
        return MidnightResult::OVERFLOW;
    }
    const int64_t result = next_local_midnight - offset;
    if (result <= now || result <= 0) return MidnightResult::OVERFLOW;
    output_millis = static_cast<uint64_t>(result);
    return MidnightResult::OK;
}

bool LocationShareScheduler::peerEquals(
    const PeerId& left,
    const PeerId& right) {
    return std::memcmp(left.bytes, right.bytes, PEER_ID_SIZE) == 0;
}

bool LocationShareScheduler::validCadence(uint32_t cadence_millis) {
    return cadence_millis >= MIN_SHARE_CADENCE_MILLIS &&
           cadence_millis <= MAX_SHARE_CADENCE_MILLIS;
}

uint64_t LocationShareScheduler::boundedAdd(uint64_t value, uint64_t delta) {
    const uint64_t maximum =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (value >= maximum || delta > maximum - value) return maximum;
    return value + delta;
}

uint64_t LocationShareScheduler::retryDelay(uint8_t failure_count) {
    uint64_t delay = INITIAL_RETRY_MILLIS;
    for (uint8_t index = 1; index < failure_count; ++index) {
        if (delay >= MAX_RETRY_MILLIS / 2) return MAX_RETRY_MILLIS;
        delay *= 2;
    }
    return delay > MAX_RETRY_MILLIS ? MAX_RETRY_MILLIS : delay;
}

void LocationShareScheduler::scheduleRejectedWork(
    ShareSession& session,
    ShareWorkType type,
    uint64_t now_millis) {
    session.awaiting_ack = false;
    session.pending_token = 0;
    session.ack_deadline_monotonic_millis = 0;
    if (type == ShareWorkType::LOCATION &&
        (session.cease_pending ||
         (session.has_expiry && now_millis >= session.expires_at_millis))) {
        session.cease_pending = true;
        session.failure_count = 0;
        session.next_attempt_millis = now_millis;
        return;
    }
    if (session.failure_count < std::numeric_limits<uint8_t>::max()) {
        ++session.failure_count;
    }
    uint64_t next = boundedAdd(now_millis, retryDelay(session.failure_count));
    if (type == ShareWorkType::LOCATION && session.has_expiry &&
        next > session.expires_at_millis) {
        next = session.expires_at_millis;
    }
    session.next_attempt_millis = next;
}

std::size_t LocationShareScheduler::find(const PeerId& peer) const {
    for (std::size_t index = 0; index < MAX_SHARE_SESSIONS; ++index) {
        if (slots_[index].occupied &&
            peerEquals(slots_[index].session.peer, peer)) {
            return index;
        }
    }
    return NO_SLOT;
}

std::size_t LocationShareScheduler::firstVacant() const {
    for (std::size_t index = 0; index < MAX_SHARE_SESSIONS; ++index) {
        if (!slots_[index].occupied) return index;
    }
    return NO_SLOT;
}

uint64_t LocationShareScheduler::nextToken() {
    uint64_t token = next_token_++;
    if (token == 0) token = next_token_++;
    if (next_token_ == 0) next_token_ = 1;
    return token;
}

void LocationShareScheduler::clear(std::size_t index) {
    if (index >= MAX_SHARE_SESSIONS || !slots_[index].occupied) return;
    slots_[index] = Slot{};
    --size_;
}

void LocationShareScheduler::observeClock(uint64_t now_millis) {
    if (last_observed_millis_ != 0 && now_millis < last_observed_millis_) {
        for (std::size_t index = 0; index < MAX_SHARE_SESSIONS; ++index) {
            if (!slots_[index].occupied) continue;
            ShareSession& session = slots_[index].session;
            if (!session.awaiting_ack) {
                session.next_attempt_millis = now_millis;
            }
        }
    }
    last_observed_millis_ = now_millis;
}

bool LocationShareScheduler::observeMonotonic(uint64_t now_millis) {
    if (has_monotonic_observation_ && now_millis < last_monotonic_millis_) {
        return false;
    }
    last_monotonic_millis_ = now_millis;
    has_monotonic_observation_ = true;
    return true;
}

ShareSessionResult LocationShareScheduler::start(
    const PeerId& peer,
    const ShareStartOptions& options,
    uint64_t now_millis) {
    if (now_millis == 0) return ShareSessionResult::CLOCK_UNAVAILABLE;
    if (now_millis > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        !validCadence(options.cadence_millis) ||
        options.approx_radius_meters < 0) {
        return ShareSessionResult::INVALID_ARGUMENT;
    }

    bool has_expiry = false;
    uint64_t expires_at_millis = 0;
    if (!expiryForDuration(options, now_millis, has_expiry, expires_at_millis)) {
        return ShareSessionResult::INVALID_ARGUMENT;
    }
    observeClock(now_millis);

    std::size_t target = find(peer);
    const bool updating = target != NO_SLOT;
    if (updating && slots_[target].session.awaiting_ack) {
        return ShareSessionResult::BUSY;
    }
    if (!updating) {
        target = firstVacant();
        if (target == NO_SLOT) return ShareSessionResult::CAPACITY;
    }

    ShareSession session{};
    session.peer = peer;
    session.cadence_millis = options.cadence_millis;
    session.approx_radius_meters = options.approx_radius_meters;
    session.has_expiry = has_expiry;
    session.expires_at_millis = expires_at_millis;
    session.next_attempt_millis = now_millis;
    slots_[target].session = session;
    if (!slots_[target].occupied) {
        slots_[target].occupied = true;
        ++size_;
    }
    return updating ? ShareSessionResult::UPDATED : ShareSessionResult::STARTED;
}

ShareSessionResult LocationShareScheduler::restore(
    const PeerId& peer,
    const ShareRestoreRecord& record,
    uint64_t now_millis) {
    if (now_millis == 0) return ShareSessionResult::CLOCK_UNAVAILABLE;
    if (now_millis > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        !validCadence(record.cadence_millis) ||
        record.approx_radius_meters < 0 ||
        (!record.has_expiry && record.expires_at_millis != 0) ||
        record.expires_at_millis >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return ShareSessionResult::INVALID_ARGUMENT;
    }
    if (!record.cease_pending && record.has_expiry &&
        now_millis >= record.expires_at_millis) {
        return ShareSessionResult::EXPIRED;
    }
    observeClock(now_millis);

    std::size_t target = find(peer);
    if (target != NO_SLOT && slots_[target].session.awaiting_ack) {
        return ShareSessionResult::BUSY;
    }
    if (target == NO_SLOT) {
        target = firstVacant();
        if (target == NO_SLOT) return ShareSessionResult::CAPACITY;
    }

    ShareSession session{};
    session.peer = peer;
    session.cadence_millis = record.cadence_millis;
    session.approx_radius_meters = record.approx_radius_meters;
    session.has_expiry = record.has_expiry;
    session.expires_at_millis = record.has_expiry ? record.expires_at_millis : 0;
    session.cease_pending = record.cease_pending;
    session.next_attempt_millis = now_millis;
    slots_[target].session = session;
    if (!slots_[target].occupied) {
        slots_[target].occupied = true;
        ++size_;
    }
    return ShareSessionResult::RESTORED;
}

ShareSessionResult LocationShareScheduler::stop(
    const PeerId& peer,
    uint64_t now_millis) {
    if (now_millis == 0) return ShareSessionResult::CLOCK_UNAVAILABLE;
    if (now_millis >
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return ShareSessionResult::INVALID_ARGUMENT;
    }
    const std::size_t index = find(peer);
    if (index == NO_SLOT) return ShareSessionResult::NOT_FOUND;
    observeClock(now_millis);
    ShareSession& session = slots_[index].session;
    session.cease_pending = true;
    if (session.awaiting_ack) {
        return ShareSessionResult::STOPPING;
    }
    session.failure_count = 0;
    session.next_attempt_millis = now_millis;
    return ShareSessionResult::STOPPING;
}

bool LocationShareScheduler::cancelWithoutCease(const PeerId& peer) {
    const std::size_t index = find(peer);
    if (index == NO_SLOT) return false;
    if (slots_[index].session.awaiting_ack) return false;
    clear(index);
    return true;
}

SharePollResult LocationShareScheduler::poll(
    uint64_t wall_now_millis,
    uint64_t monotonic_now_millis,
    bool current_location_valid,
    ShareWork& output) {
    if (wall_now_millis == 0 ||
        wall_now_millis >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return SharePollResult::CLOCK_UNAVAILABLE;
    }
    if (!observeMonotonic(monotonic_now_millis)) {
        return SharePollResult::CLOCK_UNAVAILABLE;
    }
    observeClock(wall_now_millis);

    for (std::size_t index = 0; index < MAX_SHARE_SESSIONS; ++index) {
        if (!slots_[index].occupied) continue;
        ShareSession& session = slots_[index].session;
        if (!session.cease_pending && session.has_expiry &&
            wall_now_millis >= session.expires_at_millis) {
            session.cease_pending = true;
            session.failure_count = 0;
            if (!session.awaiting_ack) {
                session.next_attempt_millis = wall_now_millis;
            }
        }
        if (session.awaiting_ack &&
            monotonic_now_millis >= session.ack_deadline_monotonic_millis) {
            const ShareWorkType expired_type = session.pending_type;
            scheduleRejectedWork(session, expired_type, wall_now_millis);
        }
        if (session.awaiting_ack ||
            wall_now_millis < session.next_attempt_millis) {
            continue;
        }

        const ShareWorkType type = session.cease_pending
                                       ? ShareWorkType::CEASE
                                       : ShareWorkType::LOCATION;
        if (type == ShareWorkType::LOCATION && !current_location_valid) continue;
        if (monotonic_now_millis >
            std::numeric_limits<uint64_t>::max() -
                ACKNOWLEDGEMENT_LEASE_MILLIS) {
            return SharePollResult::CLOCK_UNAVAILABLE;
        }

        ShareWork candidate{};
        candidate.peer = session.peer;
        candidate.type = type;
        candidate.token = nextToken();
        candidate.ack_deadline_monotonic_millis =
            monotonic_now_millis + ACKNOWLEDGEMENT_LEASE_MILLIS;
        candidate.has_expiry = session.has_expiry;
        candidate.expires_at_millis = session.expires_at_millis;
        candidate.approx_radius_meters = session.approx_radius_meters;

        session.awaiting_ack = true;
        session.pending_type = type;
        session.pending_token = candidate.token;
        session.ack_deadline_monotonic_millis = candidate.ack_deadline_monotonic_millis;
        output = candidate;
        return SharePollResult::WORK;
    }
    return SharePollResult::NO_WORK;
}

ShareAckResult LocationShareScheduler::acknowledge(
    const PeerId& peer,
    uint64_t token,
    bool queue_accepted,
    uint64_t wall_now_millis,
    uint64_t monotonic_now_millis) {
    if (wall_now_millis == 0 ||
        wall_now_millis >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return ShareAckResult::CLOCK_UNAVAILABLE;
    }
    const std::size_t index = find(peer);
    if (index == NO_SLOT) return ShareAckResult::NOT_FOUND;
    ShareSession& session = slots_[index].session;
    if (!session.awaiting_ack || token == 0 || token != session.pending_token) {
        return ShareAckResult::STALE_TOKEN;
    }
    if (!observeMonotonic(monotonic_now_millis)) {
        return ShareAckResult::CLOCK_UNAVAILABLE;
    }
    observeClock(wall_now_millis);

    const ShareWorkType acknowledged_type = session.pending_type;
    if (monotonic_now_millis >= session.ack_deadline_monotonic_millis) {
        scheduleRejectedWork(session, acknowledged_type, wall_now_millis);
        return ShareAckResult::STALE_TOKEN;
    }
    session.awaiting_ack = false;
    session.pending_token = 0;
    session.ack_deadline_monotonic_millis = 0;
    if (queue_accepted) {
        session.failure_count = 0;
        if (acknowledged_type == ShareWorkType::CEASE) {
            clear(index);
            return ShareAckResult::CEASED;
        }

        session.has_sent = true;
        session.last_sent_millis = wall_now_millis;
        if (session.cease_pending ||
            (session.has_expiry &&
             wall_now_millis >= session.expires_at_millis)) {
            session.cease_pending = true;
            session.next_attempt_millis = wall_now_millis;
        } else {
            uint64_t next = boundedAdd(
                wall_now_millis, session.cadence_millis);
            if (session.has_expiry && next > session.expires_at_millis) {
                next = session.expires_at_millis;
            }
            session.next_attempt_millis = next;
        }
        return ShareAckResult::ACCEPTED;
    }

    scheduleRejectedWork(session, acknowledged_type, wall_now_millis);
    return ShareAckResult::RETRY_SCHEDULED;
}

bool LocationShareScheduler::get(
    const PeerId& peer,
    ShareSession& output) const {
    const std::size_t index = find(peer);
    if (index == NO_SLOT) return false;
    output = slots_[index].session;
    return true;
}

ShareSnapshotResult LocationShareScheduler::snapshot(
    ShareRestoreEntry* output,
    std::size_t capacity,
    std::size_t& written_or_required) const {
    written_or_required = size_;
    if (capacity < size_) return ShareSnapshotResult::BUFFER_TOO_SMALL;
    if (size_ != 0 && output == nullptr) {
        return ShareSnapshotResult::INVALID_ARGUMENT;
    }

    std::size_t copied = 0;
    for (std::size_t index = 0; index < MAX_SHARE_SESSIONS; ++index) {
        if (!slots_[index].occupied) continue;
        const ShareSession& session = slots_[index].session;
        ShareRestoreEntry entry{};
        entry.peer = session.peer;
        entry.record.cadence_millis = session.cadence_millis;
        entry.record.approx_radius_meters = session.approx_radius_meters;
        entry.record.has_expiry = session.has_expiry;
        entry.record.expires_at_millis = session.has_expiry
                                             ? session.expires_at_millis
                                             : 0;
        entry.record.cease_pending = session.cease_pending;
        output[copied++] = entry;
    }
    written_or_required = copied;
    return ShareSnapshotResult::OK;
}

}  // namespace Telemetry
