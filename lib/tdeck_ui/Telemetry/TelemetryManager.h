// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef TELEMETRY_TELEMETRYMANAGER_H
#define TELEMETRY_TELEMETRYMANAGER_H

#ifdef ARDUINO

#include <stdint.h>
#include <vector>
#include "Bytes.h"
#include "TelemetryCodec.h"

namespace Telemetry {

enum class ShareDuration {
    MINUTES_15,
    HOURS_1,
    HOURS_4,
    UNTIL_MIDNIGHT,
    INDEFINITE
};

struct SharingSession {
    RNS::Bytes peer_hash;
    ShareDuration duration;
    uint32_t start_time;
    uint32_t end_time;      // 0 = indefinite
    uint32_t last_send;     // Last time we sent telemetry to this peer
    bool active;
};

struct ReceivedLocation {
    RNS::Bytes peer_hash;
    double lat, lon, altitude;
    double speed, bearing, accuracy;
    uint32_t timestamp;
    uint32_t received_time;
};

/**
 * Manages location sharing sessions and received peer locations.
 *
 * - Tracks active sharing sessions (per-recipient, with duration)
 * - Stores received locations from peers (capped at 32 entries)
 * - Persists to SPIFFS with dirty threshold to avoid fragmentation
 */
class TelemetryManager {
public:
    TelemetryManager();
    ~TelemetryManager();

    /** Start sharing location with a peer */
    void start_sharing(const RNS::Bytes& peer_hash, ShareDuration duration);

    /** Stop sharing location with a peer */
    void stop_sharing(const RNS::Bytes& peer_hash);

    /** Check if sharing is active with a peer */
    bool is_sharing(const RNS::Bytes& peer_hash) const;

    /**
     * Update: check expired sessions, return peers needing telemetry sent.
     * @param now Current timestamp (seconds since epoch)
     * @return Vector of peer hashes that need telemetry sent now
     */
    std::vector<RNS::Bytes> update(uint32_t now);

    /** Store a received location from a peer */
    void on_location_received(const RNS::Bytes& peer_hash, const LocationData& loc);

    /** Handle a cease signal from a peer */
    void on_cease_received(const RNS::Bytes& peer_hash);

    /** Get all received locations (for map display) */
    const std::vector<ReceivedLocation>& get_received_locations() const { return _received; }

    /** Get active sharing sessions */
    const std::vector<SharingSession>& get_sessions() const { return _sessions; }

    /** Save state to SPIFFS (called periodically) */
    void save();

    /** Load state from SPIFFS */
    void load();

private:
    std::vector<SharingSession> _sessions;
    std::vector<ReceivedLocation> _received;

    static const size_t MAX_SESSIONS = 255;
    static const int MAX_RECEIVED = 32;
    static const uint32_t SEND_INTERVAL = 60;   // Send telemetry every 60s
    static const uint32_t SAVE_INTERVAL = 60;    // Save at most every 60s

    bool _dirty;
    uint32_t _last_save;

    uint32_t compute_end_time(uint32_t start, ShareDuration duration);
};

} // namespace Telemetry

#endif // ARDUINO
#endif // TELEMETRY_TELEMETRYMANAGER_H
