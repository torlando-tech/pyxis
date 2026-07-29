#ifndef PYXIS_TELEMETRY_LOCATION_PERSISTENCE_CONTROLLER_H
#define PYXIS_TELEMETRY_LOCATION_PERSISTENCE_CONTROLLER_H

#include <cstdint>

#include "LocationPersistence.h"

namespace Telemetry {

constexpr uint64_t TRUSTED_WALL_CLOCK_MIN_MILLIS = 1577836800000ULL;
constexpr uint64_t LOCATION_PERSISTENCE_CADENCE_MILLIS = 5000ULL;

enum class LocationControllerState : uint8_t {
    WAITING_FOR_CLOCK,
    READY,
    BLOCKED,
};

enum class LocationControllerSaveResult : uint8_t {
    SAVED,
    NOT_READY,
    STORAGE_FAILURE,
};

enum class LocationConsentResult : uint8_t {
    STARTED,
    UPDATED,
    STOPPING,
    NOT_READY,
    STORAGE_FAILURE,
    NOT_FOUND,
    CAPACITY,
    CLOCK_UNAVAILABLE,
    INVALID_ARGUMENT,
    BUSY,
};

// Owns the bounded restore/save scratch. This object and the transactional
// persistence object must live in durable storage, never on a task stack.
class LocationPersistenceController {
public:
    LocationPersistenceController(LocationShareScheduler& scheduler,
                                  PeerLocationStore& peers,
                                  TransactionalLocationPersistence& persistence)
        : scheduler_(scheduler), peers_(peers), persistence_(persistence) {}

    LocationControllerState service(uint64_t wall_now_millis,
                                    uint64_t monotonic_now_millis);
    LocationControllerState state() const { return state_; }
    bool dirty() const { return dirty_; }

    LocationControllerSaveResult urgentSave(uint64_t monotonic_now_millis);
    LocationConsentResult startSharing(const PeerId& peer,
                                       const ShareStartOptions& options,
                                       uint64_t wall_now_millis,
                                       uint64_t monotonic_now_millis);
    LocationConsentResult stopSharing(const PeerId& peer,
                                      uint64_t wall_now_millis,
                                      uint64_t monotonic_now_millis);

private:
    bool observeMonotonic(uint64_t now_millis);
    bool collectSnapshot();
    bool restoreSnapshot(uint64_t wall_now_millis,
                         uint64_t monotonic_now_millis);
    void observeDirty(uint64_t monotonic_now_millis);
    void markSaved();
    static LocationConsentResult mapSessionResult(ShareSessionResult result);
    void captureRollback(const PeerId& peer);
    void restoreRollback(const PeerId& peer);

    LocationShareScheduler& scheduler_;
    PeerLocationStore& peers_;
    TransactionalLocationPersistence& persistence_;
    LocationStateSnapshot scratch_{};

    LocationControllerState state_ = LocationControllerState::WAITING_FOR_CLOCK;
    uint64_t saved_scheduler_revision_ = 0;
    uint64_t saved_peer_revision_ = 0;
    uint64_t dirty_since_monotonic_millis_ = 0;
    uint64_t next_save_attempt_monotonic_millis_ = 0;
    uint64_t next_restore_attempt_monotonic_millis_ = 0;
    uint64_t last_monotonic_millis_ = 0;
    bool has_monotonic_observation_ = false;
    bool dirty_ = false;

    ShareSession rollback_session_{};
    uint64_t rollback_revision_ = 0;
    bool rollback_existed_ = false;
};

}  // namespace Telemetry

#endif  // PYXIS_TELEMETRY_LOCATION_PERSISTENCE_CONTROLLER_H
