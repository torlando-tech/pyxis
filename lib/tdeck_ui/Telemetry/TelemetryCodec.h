// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef TELEMETRY_TELEMETRYCODEC_H
#define TELEMETRY_TELEMETRYCODEC_H

#ifdef ARDUINO

#include <stdint.h>
#include <MsgPack.h>
#include "Bytes.h"

namespace Telemetry {

// LXMF field key constants
static const uint8_t FIELD_TELEMETRY = 0x02;
static const uint8_t FIELD_ICON_APPEARANCE = 0x04;
static const uint8_t FIELD_COLUMBA_META = 0x70;

// Sensor IDs (Sideband standard)
static const uint8_t SID_TIME = 0x01;
static const uint8_t SID_LOCATION = 0x02;

struct LocationData {
    double lat;         // degrees
    double lon;         // degrees
    double altitude;    // meters
    double speed;       // m/s
    double bearing;     // degrees
    double accuracy;    // meters
    uint32_t timestamp;
    bool valid;
};

/**
 * Encode telemetry data in Sideband-compatible LXMF FIELD_TELEMETRY format.
 *
 * Wire format: msgpack map {SID_TIME: uint32, SID_LOCATION: [7 elements]}
 * Location array elements match Sideband sense.py Location.pack():
 *   [0] lat: BIN(4) signed i32 BE, microdegrees
 *   [1] lon: BIN(4) signed i32 BE, microdegrees
 *   [2] alt: BIN(4) signed i32 BE, centimeters
 *   [3] speed: BIN(4) unsigned u32 BE, cm/s
 *   [4] bearing: BIN(4) signed i32 BE, centidegrees
 *   [5] accuracy: BIN(2) unsigned u16 BE, centimeters (clamped to 65535)
 *   [6] last_update: plain integer (not BIN-wrapped)
 */
inline RNS::Bytes encode_telemetry(const LocationData& loc) {
    MsgPack::Packer packer;

    // Outer map with 2 entries: {SID_TIME: timestamp, SID_LOCATION: [...]}
    packer.packMapSize(2);

    // SID_TIME
    packer.pack((uint8_t)SID_TIME);
    packer.pack(loc.timestamp);

    // SID_LOCATION
    packer.pack((uint8_t)SID_LOCATION);
    packer.packArraySize(7);

    // Helper: pack a signed i32 as BIN(4) big-endian
    auto pack_i32_bin = [&](int32_t val) {
        uint8_t buf[4];
        buf[0] = (uint8_t)(val >> 24);
        buf[1] = (uint8_t)(val >> 16);
        buf[2] = (uint8_t)(val >> 8);
        buf[3] = (uint8_t)(val);
        packer.packBinary(buf, 4);
    };

    // Helper: pack an unsigned u32 as BIN(4) big-endian
    auto pack_u32_bin = [&](uint32_t val) {
        uint8_t buf[4];
        buf[0] = (uint8_t)(val >> 24);
        buf[1] = (uint8_t)(val >> 16);
        buf[2] = (uint8_t)(val >> 8);
        buf[3] = (uint8_t)(val);
        packer.packBinary(buf, 4);
    };

    // [0] lat: microdegrees
    pack_i32_bin((int32_t)(loc.lat * 1e6));

    // [1] lon: microdegrees
    pack_i32_bin((int32_t)(loc.lon * 1e6));

    // [2] alt: centimeters
    pack_i32_bin((int32_t)(loc.altitude * 100.0));

    // [3] speed: cm/s (unsigned)
    pack_u32_bin((uint32_t)(loc.speed * 100.0));

    // [4] bearing: centidegrees
    pack_i32_bin((int32_t)(loc.bearing * 100.0));

    // [5] accuracy: centimeters, u16 BE, clamped
    {
        uint32_t acc_cm = (uint32_t)(loc.accuracy * 100.0);
        if (acc_cm > 65535) acc_cm = 65535;
        uint8_t buf[2];
        buf[0] = (uint8_t)(acc_cm >> 8);
        buf[1] = (uint8_t)(acc_cm);
        packer.packBinary(buf, 2);
    }

    // [6] last_update: plain integer (not BIN-wrapped)
    packer.pack(loc.timestamp);

    return RNS::Bytes(packer.data(), packer.size());
}

/**
 * Decode telemetry data from Sideband/Columba/MeshChat FIELD_TELEMETRY format.
 */
inline LocationData decode_telemetry(const RNS::Bytes& data) {
    LocationData loc;
    loc.valid = false;
    loc.lat = loc.lon = loc.altitude = loc.speed = loc.bearing = loc.accuracy = 0.0;
    loc.timestamp = 0;

    MsgPack::Unpacker unpacker;
    unpacker.feed(data.data(), data.size());

    // Outer structure is a map
    MsgPack::map_size_t map_size;
    if (!unpacker.deserialize(map_size)) return loc;

    for (size_t i = 0; i < map_size.size(); i++) {
        uint8_t key;
        if (!unpacker.deserialize(key)) return loc;

        if (key == SID_TIME) {
            unpacker.deserialize(loc.timestamp);
        } else if (key == SID_LOCATION) {
            MsgPack::arr_size_t arr_size;
            if (!unpacker.deserialize(arr_size)) return loc;
            if (arr_size.size() < 7) return loc;

            // Helper: read BIN(4) as signed i32 BE
            auto read_i32_bin = [&](int32_t& out) -> bool {
                MsgPack::bin_t<uint8_t> bin;
                if (!unpacker.deserialize(bin)) return false;
                if (bin.size() < 4) return false;
                const uint8_t* b = bin.data();
                out = ((int32_t)b[0] << 24) | ((int32_t)b[1] << 16) |
                      ((int32_t)b[2] << 8) | (int32_t)b[3];
                return true;
            };

            auto read_u32_bin = [&](uint32_t& out) -> bool {
                MsgPack::bin_t<uint8_t> bin;
                if (!unpacker.deserialize(bin)) return false;
                if (bin.size() < 4) return false;
                const uint8_t* b = bin.data();
                out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                      ((uint32_t)b[2] << 8) | (uint32_t)b[3];
                return true;
            };

            int32_t i32_val;
            uint32_t u32_val;

            // [0] lat
            if (!read_i32_bin(i32_val)) return loc;
            loc.lat = (double)i32_val / 1e6;

            // [1] lon
            if (!read_i32_bin(i32_val)) return loc;
            loc.lon = (double)i32_val / 1e6;

            // [2] alt
            if (!read_i32_bin(i32_val)) return loc;
            loc.altitude = (double)i32_val / 100.0;

            // [3] speed
            if (!read_u32_bin(u32_val)) return loc;
            loc.speed = (double)u32_val / 100.0;

            // [4] bearing
            if (!read_i32_bin(i32_val)) return loc;
            loc.bearing = (double)i32_val / 100.0;

            // [5] accuracy (BIN(2) u16 BE)
            {
                MsgPack::bin_t<uint8_t> bin;
                if (!unpacker.deserialize(bin)) return loc;
                if (bin.size() >= 2) {
                    uint16_t acc = ((uint16_t)bin.data()[0] << 8) | bin.data()[1];
                    loc.accuracy = (double)acc / 100.0;
                }
            }

            // [6] last_update (plain integer)
            uint32_t last_update;
            if (!unpacker.deserialize(last_update)) return loc;

            loc.valid = true;
        } else {
            // Skip unknown key's value
            unpacker.unpackNil();
        }
    }

    return loc;
}

/**
 * Encode Columba meta field (FIELD_COLUMBA_META).
 * msgpack map with optional keys: expires, approxRadius, cease
 */
inline RNS::Bytes encode_columba_meta(uint32_t expires, int approx_radius, bool cease) {
    MsgPack::Packer packer;

    int count = 0;
    if (expires > 0) count++;
    if (approx_radius > 0) count++;
    if (cease) count++;

    packer.packMapSize(count);

    if (expires > 0) {
        packer.packString("expires", 7);
        packer.pack(expires);
    }
    if (approx_radius > 0) {
        packer.packString("approxRadius", 12);
        packer.pack(approx_radius);
    }
    if (cease) {
        packer.packString("cease", 5);
        packer.pack(true);
    }

    return RNS::Bytes(packer.data(), packer.size());
}

/**
 * Check if a Columba meta field contains a cease signal.
 */
inline bool decode_columba_cease(const RNS::Bytes& data) {
    MsgPack::Unpacker unpacker;
    unpacker.feed(data.data(), data.size());

    MsgPack::map_size_t map_size;
    if (!unpacker.deserialize(map_size)) return false;

    for (size_t i = 0; i < map_size.size(); i++) {
        MsgPack::str_t key;
        if (!unpacker.deserialize(key)) return false;

        if (String(key.c_str()) == "cease") {
            bool val;
            if (unpacker.deserialize(val)) return val;
            return false;
        } else {
            // Skip value
            unpacker.unpackNil();
        }
    }
    return false;
}

} // namespace Telemetry

#endif // ARDUINO
#endif // TELEMETRY_TELEMETRYCODEC_H
