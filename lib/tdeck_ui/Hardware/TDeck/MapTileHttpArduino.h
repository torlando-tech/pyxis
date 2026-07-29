// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_MAP_TILE_HTTP_ARDUINO_H
#define HARDWARE_TDECK_MAP_TILE_HTTP_ARDUINO_H

#include "MapTileDownloader.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

namespace Hardware {
namespace TDeck {

/** HTTPS-only transport. A non-empty explicit CA is mandatory for peer verification. */
class MapTileHttpArduino : public MapTileTransport {
public:
    MapTileHttpArduino();
    virtual ~MapTileHttpArduino();
    virtual TileTransportResult start(const char* url, const char* user_agent,
        const char* ca_certificate, std::uint32_t connect_timeout_ms,
        std::uint32_t read_timeout_ms, TileHttpResponse& response);
    virtual TileTransportResult read(std::uint8_t* output, std::size_t capacity,
        std::size_t& count, bool& eof);
    virtual void close();
    void disconnectIdle();
private:
    WiFiClientSecure client_;
    HTTPClient http_;
    WiFiClient* stream_;
    std::int64_t remaining_;
    bool open_;
    char content_type_[64];
};

/** Widens Arduino millis() rollovers into a monotonic 64-bit clock. */
class MapTileMillisClock : public MapTileDownloadClock {
public:
    MapTileMillisClock();
    virtual std::uint64_t nowMs() const;
private:
    mutable std::uint32_t previous_;
    mutable std::uint64_t high_;
};

} // namespace TDeck
} // namespace Hardware
#endif // ARDUINO
#endif
