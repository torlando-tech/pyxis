// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "TelemetryManager.h"

#ifdef ARDUINO

#include <SPIFFS.h>
#include "Log.h"
#include "Utilities/OS.h"

using namespace RNS;

namespace Telemetry {

static const char* SESSIONS_FILE = "/telemetry_sessions.dat";
static const char* LOCATIONS_FILE = "/telemetry_locations.dat";

TelemetryManager::TelemetryManager()
    : _dirty(false), _last_save(0) {
}

TelemetryManager::~TelemetryManager() {
    if (_dirty) {
        save();
    }
}

void TelemetryManager::start_sharing(const Bytes& peer_hash, ShareDuration duration) {
    // Check if already sharing with this peer
    for (auto& session : _sessions) {
        if (session.peer_hash == peer_hash) {
            // Update existing session
            uint32_t now = (uint32_t)Utilities::OS::time();
            session.duration = duration;
            session.start_time = now;
            session.end_time = compute_end_time(now, duration);
            session.active = true;
            session.last_send = 0;  // Send immediately
            _dirty = true;

            char log_buf[64];
            snprintf(log_buf, sizeof(log_buf), "Telemetry: Updated sharing with %.8s",
                     peer_hash.toHex().c_str());
            INFO(log_buf);
            return;
        }
    }

    if (_sessions.size() >= MAX_SESSIONS) {
        char log_buf[96];
        snprintf(log_buf, sizeof(log_buf),
                 "Telemetry: Session limit reached (%u), refusing %.8s",
                 static_cast<unsigned>(MAX_SESSIONS), peer_hash.toHex().c_str());
        WARNING(log_buf);
        return;
    }

    // Create new session
    SharingSession session;
    session.peer_hash = peer_hash;
    session.duration = duration;
    uint32_t now = (uint32_t)Utilities::OS::time();
    session.start_time = now;
    session.end_time = compute_end_time(now, duration);
    session.active = true;
    session.last_send = 0;
    _sessions.push_back(session);
    _dirty = true;

    char log_buf[64];
    snprintf(log_buf, sizeof(log_buf), "Telemetry: Started sharing with %.8s",
             peer_hash.toHex().c_str());
    INFO(log_buf);
}

void TelemetryManager::stop_sharing(const Bytes& peer_hash) {
    for (auto it = _sessions.begin(); it != _sessions.end(); ++it) {
        if (it->peer_hash == peer_hash) {
            _sessions.erase(it);
            _dirty = true;
            INFO("Telemetry: Stopped sharing");
            return;
        }
    }
}

bool TelemetryManager::is_sharing(const Bytes& peer_hash) const {
    for (const auto& session : _sessions) {
        if (session.peer_hash == peer_hash && session.active) {
            return true;
        }
    }
    return false;
}

std::vector<Bytes> TelemetryManager::update(uint32_t now) {
    std::vector<Bytes> peers_needing_send;

    // Check for expired sessions
    for (auto it = _sessions.begin(); it != _sessions.end(); ) {
        if (it->end_time > 0 && now >= it->end_time) {
            char log_buf[64];
            snprintf(log_buf, sizeof(log_buf), "Telemetry: Session expired for %.8s",
                     it->peer_hash.toHex().c_str());
            INFO(log_buf);
            it = _sessions.erase(it);
            _dirty = true;
        } else {
            ++it;
        }
    }

    // Check which peers need telemetry sent
    for (auto& session : _sessions) {
        if (session.active && (now - session.last_send >= SEND_INTERVAL)) {
            peers_needing_send.push_back(session.peer_hash);
            session.last_send = now;
        }
    }

    // Periodic save
    if (_dirty && (now - _last_save >= SAVE_INTERVAL)) {
        save();
    }

    return peers_needing_send;
}

void TelemetryManager::on_location_received(const Bytes& peer_hash, const LocationData& loc) {
    // Update existing entry or add new
    for (auto& entry : _received) {
        if (entry.peer_hash == peer_hash) {
            entry.lat = loc.lat;
            entry.lon = loc.lon;
            entry.altitude = loc.altitude;
            entry.speed = loc.speed;
            entry.bearing = loc.bearing;
            entry.accuracy = loc.accuracy;
            entry.timestamp = loc.timestamp;
            entry.received_time = (uint32_t)Utilities::OS::time();
            _dirty = true;
            return;
        }
    }

    // Add new entry (cap at MAX_RECEIVED)
    if ((int)_received.size() >= MAX_RECEIVED) {
        // Remove oldest
        uint32_t oldest_time = UINT32_MAX;
        size_t oldest_idx = 0;
        for (size_t i = 0; i < _received.size(); i++) {
            if (_received[i].received_time < oldest_time) {
                oldest_time = _received[i].received_time;
                oldest_idx = i;
            }
        }
        _received.erase(_received.begin() + oldest_idx);
    }

    ReceivedLocation entry;
    entry.peer_hash = peer_hash;
    entry.lat = loc.lat;
    entry.lon = loc.lon;
    entry.altitude = loc.altitude;
    entry.speed = loc.speed;
    entry.bearing = loc.bearing;
    entry.accuracy = loc.accuracy;
    entry.timestamp = loc.timestamp;
    entry.received_time = (uint32_t)Utilities::OS::time();
    _received.push_back(entry);
    _dirty = true;

    char log_buf[80];
    snprintf(log_buf, sizeof(log_buf), "Telemetry: Location from %.8s (%.4f, %.4f)",
             peer_hash.toHex().c_str(), loc.lat, loc.lon);
    INFO(log_buf);
}

void TelemetryManager::on_cease_received(const Bytes& peer_hash) {
    // Remove received location for this peer
    for (auto it = _received.begin(); it != _received.end(); ++it) {
        if (it->peer_hash == peer_hash) {
            _received.erase(it);
            _dirty = true;
            INFO("Telemetry: Peer ceased sharing");
            return;
        }
    }
}

uint32_t TelemetryManager::compute_end_time(uint32_t start, ShareDuration duration) {
    switch (duration) {
        case ShareDuration::MINUTES_15:
            return start + 15 * 60;
        case ShareDuration::HOURS_1:
            return start + 3600;
        case ShareDuration::HOURS_4:
            return start + 4 * 3600;
        case ShareDuration::UNTIL_MIDNIGHT: {
            // Calculate seconds until midnight UTC
            uint32_t secs_today = start % 86400;
            return start + (86400 - secs_today);
        }
        case ShareDuration::INDEFINITE:
        default:
            return 0;  // No expiry
    }
}

void TelemetryManager::save() {
    // Save sessions
    {
        File f = SPIFFS.open(SESSIONS_FILE, FILE_WRITE);
        if (f) {
            const size_t session_count = _sessions.size() > MAX_SESSIONS ? MAX_SESSIONS : _sessions.size();
            if (session_count != _sessions.size()) {
                char log_buf[96];
                snprintf(log_buf, sizeof(log_buf),
                         "Telemetry: Truncating persisted sessions from %u to %u",
                         static_cast<unsigned>(_sessions.size()),
                         static_cast<unsigned>(session_count));
                WARNING(log_buf);
            }

            uint8_t count = static_cast<uint8_t>(session_count);
            f.write(&count, 1);
            for (size_t i = 0; i < session_count; ++i) {
                const auto& s = _sessions[i];
                uint8_t hash_len = (uint8_t)s.peer_hash.size();
                f.write(&hash_len, 1);
                f.write(s.peer_hash.data(), hash_len);
                f.write((const uint8_t*)&s.duration, sizeof(s.duration));
                f.write((const uint8_t*)&s.start_time, sizeof(s.start_time));
                f.write((const uint8_t*)&s.end_time, sizeof(s.end_time));
                uint8_t active = s.active ? 1 : 0;
                f.write(&active, 1);
            }
            f.close();
        }
    }

    // Save received locations
    {
        File f = SPIFFS.open(LOCATIONS_FILE, FILE_WRITE);
        if (f) {
            uint8_t count = (uint8_t)_received.size();
            f.write(&count, 1);
            for (const auto& r : _received) {
                uint8_t hash_len = (uint8_t)r.peer_hash.size();
                f.write(&hash_len, 1);
                f.write(r.peer_hash.data(), hash_len);
                f.write((const uint8_t*)&r.lat, sizeof(r.lat));
                f.write((const uint8_t*)&r.lon, sizeof(r.lon));
                f.write((const uint8_t*)&r.altitude, sizeof(r.altitude));
                f.write((const uint8_t*)&r.speed, sizeof(r.speed));
                f.write((const uint8_t*)&r.bearing, sizeof(r.bearing));
                f.write((const uint8_t*)&r.accuracy, sizeof(r.accuracy));
                f.write((const uint8_t*)&r.timestamp, sizeof(r.timestamp));
                f.write((const uint8_t*)&r.received_time, sizeof(r.received_time));
            }
            f.close();
        }
    }

    _dirty = false;
    _last_save = (uint32_t)Utilities::OS::time();
    DEBUG("Telemetry: State saved to SPIFFS");
}

void TelemetryManager::load() {
    // Load sessions
    {
        File f = SPIFFS.open(SESSIONS_FILE, FILE_READ);
        if (f) {
            uint8_t count = 0;
            f.read(&count, 1);
            for (uint8_t i = 0; i < count; i++) {
                if (_sessions.size() >= MAX_SESSIONS) {
                    WARNING("Telemetry: Reached session load limit, skipping remaining entries");
                    break;
                }
                SharingSession s;
                uint8_t hash_len = 0;
                f.read(&hash_len, 1);
                if (hash_len > 32) break;
                uint8_t hash_buf[32];
                f.read(hash_buf, hash_len);
                s.peer_hash = Bytes(hash_buf, hash_len);
                f.read((uint8_t*)&s.duration, sizeof(s.duration));
                f.read((uint8_t*)&s.start_time, sizeof(s.start_time));
                f.read((uint8_t*)&s.end_time, sizeof(s.end_time));
                uint8_t active = 0;
                f.read(&active, 1);
                s.active = (active != 0);
                s.last_send = 0;
                _sessions.push_back(s);
            }
            f.close();
            char log_buf[48];
            snprintf(log_buf, sizeof(log_buf), "Telemetry: Loaded %zu sessions", _sessions.size());
            INFO(log_buf);
        }
    }

    // Load received locations
    {
        File f = SPIFFS.open(LOCATIONS_FILE, FILE_READ);
        if (f) {
            uint8_t count = 0;
            f.read(&count, 1);
            for (uint8_t i = 0; i < count; i++) {
                ReceivedLocation r;
                uint8_t hash_len = 0;
                f.read(&hash_len, 1);
                if (hash_len > 32) break;
                uint8_t hash_buf[32];
                f.read(hash_buf, hash_len);
                r.peer_hash = Bytes(hash_buf, hash_len);
                f.read((uint8_t*)&r.lat, sizeof(r.lat));
                f.read((uint8_t*)&r.lon, sizeof(r.lon));
                f.read((uint8_t*)&r.altitude, sizeof(r.altitude));
                f.read((uint8_t*)&r.speed, sizeof(r.speed));
                f.read((uint8_t*)&r.bearing, sizeof(r.bearing));
                f.read((uint8_t*)&r.accuracy, sizeof(r.accuracy));
                f.read((uint8_t*)&r.timestamp, sizeof(r.timestamp));
                f.read((uint8_t*)&r.received_time, sizeof(r.received_time));
                _received.push_back(r);
            }
            f.close();
            char log_buf[48];
            snprintf(log_buf, sizeof(log_buf), "Telemetry: Loaded %zu locations", _received.size());
            INFO(log_buf);
        }
    }
}

} // namespace Telemetry

#endif // ARDUINO
