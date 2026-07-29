#include "LocationPersistenceController.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace Telemetry {

bool LocationPersistenceController::observeMonotonic(uint64_t now_millis) {
    if (has_monotonic_observation_ && now_millis < last_monotonic_millis_) {
        state_ = LocationControllerState::BLOCKED;
        return false;
    }
    last_monotonic_millis_ = now_millis;
    has_monotonic_observation_ = true;
    return true;
}

bool LocationPersistenceController::collectSnapshot() {
    std::size_t session_count = 0;
    if (scheduler_.snapshot(scratch_.sessions, MAX_SHARE_SESSIONS,
                            session_count) != ShareSnapshotResult::OK) {
        return false;
    }
    scratch_.session_count = session_count;
    scratch_.location_count = peers_.durableSnapshot(
        scratch_.locations, MAX_PEER_LOCATIONS);
    return scratch_.location_count == peers_.size();
}

void LocationPersistenceController::markSaved() {
    saved_scheduler_revision_ = scheduler_.revision();
    saved_peer_revision_ = peers_.revision();
    dirty_ = false;
    dirty_since_monotonic_millis_ = 0;
    next_save_attempt_monotonic_millis_ = 0;
}

void LocationPersistenceController::observeDirty(
    uint64_t monotonic_now_millis) {
    if (scheduler_.revision() == saved_scheduler_revision_ &&
        peers_.revision() == saved_peer_revision_) {
        return;
    }
    if (!dirty_) {
        dirty_ = true;
        dirty_since_monotonic_millis_ = monotonic_now_millis;
    }
}

bool LocationPersistenceController::restoreSnapshot(
    uint64_t wall_now_millis,
    uint64_t monotonic_now_millis) {
    bool pruned = false;
    for (std::size_t index = 0; index < scratch_.session_count; ++index) {
        const ShareRestoreEntry& entry = scratch_.sessions[index];
        if (!entry.record.cease_pending && entry.record.has_expiry &&
            wall_now_millis >= entry.record.expires_at_millis) {
            pruned = true;
            continue;
        }
        if (scheduler_.restore(entry.peer, entry.record, wall_now_millis) !=
            ShareSessionResult::RESTORED) {
            return false;
        }
    }
    for (std::size_t index = 0; index < scratch_.location_count; ++index) {
        const PeerLocationRecord& record = scratch_.locations[index];
        if (record.has_expiry && wall_now_millis >= record.expires_at_millis) {
            pruned = true;
            continue;
        }
        const PeerLocationResult result = peers_.restore(record);
        if (result != PeerLocationResult::INSERTED &&
            result != PeerLocationResult::UPDATED) {
            return false;
        }
    }
    if (peers_.prune(wall_now_millis,
                     std::numeric_limits<uint64_t>::max()) != 0) {
        pruned = true;
    }
    saved_scheduler_revision_ = scheduler_.revision();
    saved_peer_revision_ = peers_.revision();
    if (pruned) {
        dirty_ = true;
        dirty_since_monotonic_millis_ = monotonic_now_millis;
    }
    return true;
}

LocationControllerState LocationPersistenceController::service(
    uint64_t wall_now_millis,
    uint64_t monotonic_now_millis) {
    if (state_ == LocationControllerState::BLOCKED ||
        !observeMonotonic(monotonic_now_millis)) {
        return LocationControllerState::BLOCKED;
    }

    if (state_ == LocationControllerState::WAITING_FOR_CLOCK) {
        if (wall_now_millis < TRUSTED_WALL_CLOCK_MIN_MILLIS) return state_;
        if (monotonic_now_millis < next_restore_attempt_monotonic_millis_) {
            return state_;
        }
        const LocationPersistenceResult loaded = persistence_.load(scratch_);
        if (loaded == LocationPersistenceResult::IO_ERROR) {
            next_restore_attempt_monotonic_millis_ =
                monotonic_now_millis >
                        std::numeric_limits<uint64_t>::max() -
                            LOCATION_PERSISTENCE_CADENCE_MILLIS
                    ? std::numeric_limits<uint64_t>::max()
                    : monotonic_now_millis +
                          LOCATION_PERSISTENCE_CADENCE_MILLIS;
            return state_;
        }
        if (loaded == LocationPersistenceResult::UNAVAILABLE ||
            loaded == LocationPersistenceResult::INVALID_STATE ||
            loaded == LocationPersistenceResult::ENCODE_ERROR ||
            loaded == LocationPersistenceResult::SAVED) {
            state_ = LocationControllerState::BLOCKED;
            return state_;
        }
        if (loaded != LocationPersistenceResult::NOT_FOUND &&
            !restoreSnapshot(wall_now_millis, monotonic_now_millis)) {
            state_ = LocationControllerState::BLOCKED;
            return state_;
        }
        if (loaded == LocationPersistenceResult::NOT_FOUND) markSaved();
        state_ = LocationControllerState::READY;
    }

    peers_.prune(wall_now_millis, std::numeric_limits<uint64_t>::max());
    observeDirty(monotonic_now_millis);
    if (dirty_ &&
        monotonic_now_millis - dirty_since_monotonic_millis_ >=
            LOCATION_PERSISTENCE_CADENCE_MILLIS &&
        monotonic_now_millis >= next_save_attempt_monotonic_millis_) {
        if (collectSnapshot() &&
            persistence_.save(scratch_) == LocationPersistenceResult::SAVED) {
            markSaved();
        } else {
            next_save_attempt_monotonic_millis_ =
                monotonic_now_millis >
                        std::numeric_limits<uint64_t>::max() -
                            LOCATION_PERSISTENCE_CADENCE_MILLIS
                    ? std::numeric_limits<uint64_t>::max()
                    : monotonic_now_millis +
                          LOCATION_PERSISTENCE_CADENCE_MILLIS;
        }
    }
    return state_;
}

LocationControllerSaveResult LocationPersistenceController::urgentSave(
    uint64_t monotonic_now_millis) {
    if (state_ != LocationControllerState::READY ||
        !observeMonotonic(monotonic_now_millis)) {
        return LocationControllerSaveResult::NOT_READY;
    }
    if (!collectSnapshot() ||
        persistence_.save(scratch_) != LocationPersistenceResult::SAVED) {
        observeDirty(monotonic_now_millis);
        next_save_attempt_monotonic_millis_ =
            monotonic_now_millis >
                    std::numeric_limits<uint64_t>::max() -
                        LOCATION_PERSISTENCE_CADENCE_MILLIS
                ? std::numeric_limits<uint64_t>::max()
                : monotonic_now_millis + LOCATION_PERSISTENCE_CADENCE_MILLIS;
        return LocationControllerSaveResult::STORAGE_FAILURE;
    }
    markSaved();
    return LocationControllerSaveResult::SAVED;
}

LocationConsentResult LocationPersistenceController::mapSessionResult(
    ShareSessionResult result) {
    switch (result) {
        case ShareSessionResult::STARTED: return LocationConsentResult::STARTED;
        case ShareSessionResult::UPDATED: return LocationConsentResult::UPDATED;
        case ShareSessionResult::STOPPING: return LocationConsentResult::STOPPING;
        case ShareSessionResult::NOT_FOUND: return LocationConsentResult::NOT_FOUND;
        case ShareSessionResult::CAPACITY: return LocationConsentResult::CAPACITY;
        case ShareSessionResult::CLOCK_UNAVAILABLE:
            return LocationConsentResult::CLOCK_UNAVAILABLE;
        case ShareSessionResult::INVALID_ARGUMENT:
            return LocationConsentResult::INVALID_ARGUMENT;
        case ShareSessionResult::BUSY: return LocationConsentResult::BUSY;
        case ShareSessionResult::RESTORED:
        case ShareSessionResult::EXPIRED:
            return LocationConsentResult::INVALID_ARGUMENT;
    }
    return LocationConsentResult::INVALID_ARGUMENT;
}

void LocationPersistenceController::captureRollback(const PeerId& peer) {
    const std::size_t index = scheduler_.find(peer);
    rollback_existed_ = index != MAX_SHARE_SESSIONS;
    rollback_session_ = rollback_existed_
                            ? scheduler_.slots_[index].session
                            : ShareSession{};
    rollback_revision_ = scheduler_.revision_;
}

void LocationPersistenceController::restoreRollback(const PeerId& peer) {
    const std::size_t index = scheduler_.find(peer);
    if (rollback_existed_) {
        if (index != MAX_SHARE_SESSIONS) {
            scheduler_.slots_[index].session = rollback_session_;
        }
    } else if (index != MAX_SHARE_SESSIONS) {
        scheduler_.clear(index);
    }
    scheduler_.revision_ = rollback_revision_;
}

LocationConsentResult LocationPersistenceController::startSharing(
    const PeerId& peer,
    const ShareStartOptions& options,
    uint64_t wall_now_millis,
    uint64_t monotonic_now_millis) {
    if (state_ != LocationControllerState::READY ||
        !observeMonotonic(monotonic_now_millis)) {
        return LocationConsentResult::NOT_READY;
    }
    captureRollback(peer);
    const ShareSessionResult result = scheduler_.start(peer, options, wall_now_millis);
    if (result != ShareSessionResult::STARTED &&
        result != ShareSessionResult::UPDATED) {
        return mapSessionResult(result);
    }
    if (urgentSave(monotonic_now_millis) != LocationControllerSaveResult::SAVED) {
        restoreRollback(peer);
        return LocationConsentResult::STORAGE_FAILURE;
    }
    return mapSessionResult(result);
}

LocationConsentResult LocationPersistenceController::stopSharing(
    const PeerId& peer,
    uint64_t wall_now_millis,
    uint64_t monotonic_now_millis) {
    if (state_ != LocationControllerState::READY ||
        !observeMonotonic(monotonic_now_millis)) {
        return LocationConsentResult::NOT_READY;
    }
    captureRollback(peer);
    const ShareSessionResult result = scheduler_.stop(peer, wall_now_millis);
    if (result != ShareSessionResult::STOPPING) return mapSessionResult(result);
    if (urgentSave(monotonic_now_millis) != LocationControllerSaveResult::SAVED) {
        restoreRollback(peer);
        return LocationConsentResult::STORAGE_FAILURE;
    }
    return LocationConsentResult::STOPPING;
}

}  // namespace Telemetry
