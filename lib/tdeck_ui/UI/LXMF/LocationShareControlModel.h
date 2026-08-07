#ifndef PYXIS_UI_LXMF_LOCATION_SHARE_CONTROL_MODEL_H
#define PYXIS_UI_LXMF_LOCATION_SHARE_CONTROL_MODEL_H

#include <cstddef>
#include <cstdint>

namespace UI {
namespace LXMF {

enum class LocationShareControlStatus : uint8_t { OFF, ACTIVE, STOPPING };
enum class LocationShareControlError : uint8_t {
    NONE,
    CLOCK_UNAVAILABLE,
    STORAGE_FAILURE,
    CAPACITY,
    BUSY,
    INVALID,
};
enum class LocationShareDuration : uint8_t { MINUTES_15, HOUR_1, HOURS_4 };
enum class LocationShareCadence : uint8_t { MINUTE_1, MINUTES_5, MINUTES_15 };
enum class LocationSharePrecision : uint8_t {
    EXACT,
    METERS_100,
    KILOMETER_1,
    KILOMETERS_10,
};

// Portable, allocation-free presentation state. It deliberately knows nothing
// about the scheduler, persistence, router, Arduino, or LVGL.
class LocationShareControlModel {
public:
    LocationShareControlModel();

    bool openForPeer(const uint8_t* peer, std::size_t peerSize);
    bool selectDuration(LocationShareDuration duration);
    bool selectCadence(LocationShareCadence cadence);
    bool selectPrecision(LocationSharePrecision precision);
    bool requestConfirmation();
    void cancelConfirmation() { confirmationPending_ = false; }

    void applyOff();
    void applyActive(uint64_t expiresAtMillis, uint32_t cadenceMillis,
                     bool hasApproximation, int32_t approximationMeters);
    void markStopping();
    void applyError(LocationShareControlError error);

    bool formatConfirmation(char* output, std::size_t capacity) const;
    bool formatState(char* output, std::size_t capacity) const;

    LocationShareControlStatus status() const { return status_; }
    LocationShareControlError error() const { return error_; }
    LocationShareDuration duration() const { return duration_; }
    LocationShareCadence cadence() const { return cadence_; }
    LocationSharePrecision precision() const { return precision_; }
    bool confirmationPending() const { return confirmationPending_; }
    bool hasPeer() const { return hasPeer_; }
    const uint8_t* peer() const { return peer_; }
    uint64_t durationMillis() const;
    uint32_t cadenceMillis() const;
    bool hasApproximation() const;
    int32_t approximationMeters() const;
    uint64_t expiresAtMillis() const { return expiresAtMillis_; }
    const char* errorText() const;

private:
    static const char* durationText(LocationShareDuration duration);
    static const char* cadenceText(uint32_t cadenceMillis);
    static const char* precisionText(bool hasApproximation, int32_t meters);

    uint8_t peer_[16];
    LocationShareControlStatus status_;
    LocationShareControlError error_;
    LocationShareDuration duration_;
    LocationShareCadence cadence_;
    LocationSharePrecision precision_;
    uint64_t expiresAtMillis_;
    bool hasPeer_;
    bool confirmationPending_;
};

} // namespace LXMF
} // namespace UI

#endif
