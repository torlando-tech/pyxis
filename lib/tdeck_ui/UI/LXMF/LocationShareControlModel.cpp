#include "LocationShareControlModel.h"

#include <cstdio>
#include <cstring>

namespace UI {
namespace LXMF {

LocationShareControlModel::LocationShareControlModel()
    : peer_{}, status_(LocationShareControlStatus::OFF),
      error_(LocationShareControlError::NONE),
      duration_(LocationShareDuration::MINUTES_15),
      cadence_(LocationShareCadence::MINUTE_1),
      precision_(LocationSharePrecision::EXACT), expiresAtMillis_(0),
      hasPeer_(false), confirmationPending_(false) {}

bool LocationShareControlModel::openForPeer(const uint8_t* peer,
                                            std::size_t peerSize) {
    applyOff();
    hasPeer_ = false;
    std::memset(peer_, 0, sizeof(peer_));
    if (peer == NULL || peerSize != sizeof(peer_)) {
        error_ = LocationShareControlError::INVALID;
        return false;
    }
    std::memcpy(peer_, peer, sizeof(peer_));
    hasPeer_ = true;
    return true;
}

bool LocationShareControlModel::selectDuration(LocationShareDuration duration) {
    switch (duration) {
        case LocationShareDuration::MINUTES_15:
        case LocationShareDuration::HOUR_1:
        case LocationShareDuration::HOURS_4:
            duration_ = duration;
            return true;
    }
    return false;
}

bool LocationShareControlModel::selectCadence(LocationShareCadence cadence) {
    switch (cadence) {
        case LocationShareCadence::MINUTE_1:
        case LocationShareCadence::MINUTES_5:
        case LocationShareCadence::MINUTES_15:
            cadence_ = cadence;
            return true;
    }
    return false;
}

bool LocationShareControlModel::selectPrecision(LocationSharePrecision precision) {
    switch (precision) {
        case LocationSharePrecision::EXACT:
        case LocationSharePrecision::METERS_100:
        case LocationSharePrecision::KILOMETER_1:
        case LocationSharePrecision::KILOMETERS_10:
            precision_ = precision;
            return true;
    }
    return false;
}

bool LocationShareControlModel::requestConfirmation() {
    if (!hasPeer_) {
        applyError(LocationShareControlError::INVALID);
        return false;
    }
    error_ = LocationShareControlError::NONE;
    confirmationPending_ = true;
    return true;
}

void LocationShareControlModel::applyOff() {
    status_ = LocationShareControlStatus::OFF;
    error_ = LocationShareControlError::NONE;
    expiresAtMillis_ = 0;
    confirmationPending_ = false;
}

void LocationShareControlModel::applyActive(uint64_t expiresAtMillis,
                                            uint32_t cadenceMillisValue,
                                            bool hasApproximationValue,
                                            int32_t approximationMetersValue) {
    status_ = LocationShareControlStatus::ACTIVE;
    error_ = LocationShareControlError::NONE;
    expiresAtMillis_ = expiresAtMillis;
    confirmationPending_ = false;
    if (cadenceMillisValue == 60000U) cadence_ = LocationShareCadence::MINUTE_1;
    else if (cadenceMillisValue == 300000U) cadence_ = LocationShareCadence::MINUTES_5;
    else if (cadenceMillisValue == 900000U) cadence_ = LocationShareCadence::MINUTES_15;
    if (!hasApproximationValue) precision_ = LocationSharePrecision::EXACT;
    else if (approximationMetersValue == 100) precision_ = LocationSharePrecision::METERS_100;
    else if (approximationMetersValue == 1000) precision_ = LocationSharePrecision::KILOMETER_1;
    else if (approximationMetersValue == 10000) precision_ = LocationSharePrecision::KILOMETERS_10;
}

void LocationShareControlModel::markStopping() {
    status_ = LocationShareControlStatus::STOPPING;
    error_ = LocationShareControlError::NONE;
    confirmationPending_ = false;
}

void LocationShareControlModel::applyError(LocationShareControlError error) {
    if (status_ != LocationShareControlStatus::ACTIVE) {
        status_ = LocationShareControlStatus::OFF;
    }
    error_ = error;
    confirmationPending_ = false;
}

uint64_t LocationShareControlModel::durationMillis() const {
    switch (duration_) {
        case LocationShareDuration::MINUTES_15: return 15ULL * 60ULL * 1000ULL;
        case LocationShareDuration::HOUR_1: return 60ULL * 60ULL * 1000ULL;
        case LocationShareDuration::HOURS_4: return 4ULL * 60ULL * 60ULL * 1000ULL;
    }
    return 0;
}

uint32_t LocationShareControlModel::cadenceMillis() const {
    switch (cadence_) {
        case LocationShareCadence::MINUTE_1: return 60000U;
        case LocationShareCadence::MINUTES_5: return 300000U;
        case LocationShareCadence::MINUTES_15: return 900000U;
    }
    return 0;
}

bool LocationShareControlModel::hasApproximation() const {
    return precision_ != LocationSharePrecision::EXACT;
}

int32_t LocationShareControlModel::approximationMeters() const {
    switch (precision_) {
        case LocationSharePrecision::EXACT: return 0;
        case LocationSharePrecision::METERS_100: return 100;
        case LocationSharePrecision::KILOMETER_1: return 1000;
        case LocationSharePrecision::KILOMETERS_10: return 10000;
    }
    return 0;
}

const char* LocationShareControlModel::durationText(LocationShareDuration duration) {
    switch (duration) {
        case LocationShareDuration::MINUTES_15: return "15 min";
        case LocationShareDuration::HOUR_1: return "1 hour";
        case LocationShareDuration::HOURS_4: return "4 hours";
    }
    return "invalid";
}

const char* LocationShareControlModel::cadenceText(uint32_t cadenceMillisValue) {
    if (cadenceMillisValue == 60000U) return "1 min";
    if (cadenceMillisValue == 300000U) return "5 min";
    if (cadenceMillisValue == 900000U) return "15 min";
    return "invalid";
}

const char* LocationShareControlModel::precisionText(bool hasApproximationValue,
                                                     int32_t meters) {
    if (!hasApproximationValue) return "Exact";
    if (meters == 100) return "100 m";
    if (meters == 1000) return "1 km";
    if (meters == 10000) return "10 km";
    return "invalid";
}

bool LocationShareControlModel::formatConfirmation(char* output,
                                                   std::size_t capacity) const {
    if (output == NULL || capacity == 0 || !hasPeer_) return false;
    char peerHex[33];
    for (std::size_t i = 0; i < sizeof(peer_); ++i) {
        std::snprintf(peerHex + i * 2, 3, "%02x", peer_[i]);
    }
    const int written = std::snprintf(
        output, capacity, "Share with %s?\nDuration: %s\nCadence: %s\nPrecision: %s",
        peerHex, durationText(duration_), cadenceText(cadenceMillis()),
        precisionText(hasApproximation(), approximationMeters()));
    return written >= 0 && static_cast<std::size_t>(written) < capacity;
}

bool LocationShareControlModel::formatState(char* output,
                                            std::size_t capacity) const {
    if (output == NULL || capacity == 0) return false;
    const char* statusText = status_ == LocationShareControlStatus::ACTIVE
        ? "ACTIVE" : status_ == LocationShareControlStatus::STOPPING
        ? "STOPPING" : "OFF";
    const int written = std::snprintf(
        output, capacity, "%s\nExpiry: %llu\nCadence: %s\nPrecision: %s",
        statusText, static_cast<unsigned long long>(expiresAtMillis_),
        cadenceText(cadenceMillis()),
        precisionText(hasApproximation(), approximationMeters()));
    return written >= 0 && static_cast<std::size_t>(written) < capacity;
}

const char* LocationShareControlModel::errorText() const {
    switch (error_) {
        case LocationShareControlError::NONE: return "";
        case LocationShareControlError::CLOCK_UNAVAILABLE:
            return "System clock unavailable. Get GPS/network time, then retry.";
        case LocationShareControlError::STORAGE_FAILURE:
            return "Location storage failure. Check filesystem and retry.";
        case LocationShareControlError::CAPACITY:
            return "Sharing capacity reached. Stop another peer and retry.";
        case LocationShareControlError::BUSY:
            return "Location sharing busy. Wait for pending delivery and retry.";
        case LocationShareControlError::INVALID:
            return "Peer or sharing selection invalid; sharing remains off.";
    }
    return "Invalid location sharing state.";
}

} // namespace LXMF
} // namespace UI
