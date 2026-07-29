// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#include "MapTileHttpArduino.h"

#ifdef ARDUINO
#include <climits>
#include <cstring>

namespace Hardware {
namespace TDeck {

namespace {
std::uint16_t boundedReadTimeout(std::uint32_t value) {
    return value > UINT16_MAX ? UINT16_MAX : static_cast<std::uint16_t>(value);
}
std::int32_t boundedConnectTimeout(std::uint32_t value) {
    return value > static_cast<std::uint32_t>(INT32_MAX) ? INT32_MAX : static_cast<std::int32_t>(value);
}
}

MapTileHttpArduino::MapTileHttpArduino()
    : stream_(NULL), remaining_(-1), open_(false), content_type_{0} {}
MapTileHttpArduino::~MapTileHttpArduino() { close(); }

TileTransportResult MapTileHttpArduino::start(const char* url, const char* user_agent,
    const char* ca_certificate, std::uint32_t connect_timeout_ms,
    std::uint32_t read_timeout_ms, TileHttpResponse& response) {
    close();
    if (url == NULL || user_agent == NULL || ca_certificate == NULL || ca_certificate[0] == '\0' ||
        std::strncmp(url, "https://", 8U) != 0) return TileTransportResult::ERROR;

    client_.setCACert(ca_certificate);
    client_.setTimeout(read_timeout_ms);
    http_.setConnectTimeout(boundedConnectTimeout(connect_timeout_ms));
    http_.setTimeout(boundedReadTimeout(read_timeout_ms));
    // Keep the single visible-tile TLS session alive across sequential requests.
    // Repeated handshakes fragment the ESP32-S3 internal heap and can exhaust the
    // hardware SHA/AES allocation budget before the six-tile viewport completes.
    http_.setReuse(true);
    http_.setUserAgent(user_agent);
    const char* response_headers[] = {"Content-Type"};
    http_.collectHeaders(response_headers, 1U);
    if (!http_.begin(client_, url)) {
        http_.end();
        return TileTransportResult::ERROR;
    }

    const int status = http_.GET();
    if (status < 0) {
        http_.end();
        return status == HTTPC_ERROR_READ_TIMEOUT
            ? TileTransportResult::TIMEOUT
            : TileTransportResult::ERROR;
    }
    open_ = true;
    stream_ = http_.getStreamPtr();
    remaining_ = static_cast<std::int64_t>(http_.getSize());
    const String type = http_.header("Content-Type");
    if (type.length() >= sizeof(content_type_)) { close(); return TileTransportResult::ERROR; }
    std::memcpy(content_type_, type.c_str(), type.length() + 1U);
    response.status_code = status;
    response.content_length = remaining_;
    response.content_type = content_type_;
    return TileTransportResult::OK;
}

TileTransportResult MapTileHttpArduino::read(std::uint8_t* output, std::size_t capacity,
    std::size_t& count, bool& eof) {
    count = 0U;
    eof = false;
    if (!open_ || stream_ == NULL || (output == NULL && capacity != 0U) || capacity == 0U) {
        return TileTransportResult::ERROR;
    }
    if (remaining_ == 0) { eof = true; return TileTransportResult::OK; }

    std::size_t wanted = capacity;
    if (remaining_ > 0 && static_cast<std::uint64_t>(remaining_) < wanted) {
        wanted = static_cast<std::size_t>(remaining_);
    }
    const int available = stream_->available();
    if (available > 0 && static_cast<std::size_t>(available) < wanted) wanted = static_cast<std::size_t>(available);
    else if (available == 0) wanted = 1U;
    count = stream_->readBytes(output, wanted);
    if (count == 0U) {
        if (!stream_->connected() && stream_->available() == 0) { eof = true; return TileTransportResult::OK; }
        return TileTransportResult::TIMEOUT;
    }
    if (remaining_ > 0) remaining_ -= static_cast<std::int64_t>(count);
    eof = remaining_ == 0 || (!stream_->connected() && stream_->available() == 0);
    return TileTransportResult::OK;
}

void MapTileHttpArduino::close() {
    if (open_) http_.end();
    stream_ = NULL;
    remaining_ = -1;
    open_ = false;
    content_type_[0] = '\0';
}

MapTileMillisClock::MapTileMillisClock() : previous_(millis()), high_(0U) {}
std::uint64_t MapTileMillisClock::nowMs() const {
    const std::uint32_t current = millis();
    if (current < previous_) high_ += (UINT64_C(1) << 32U);
    previous_ = current;
    return high_ | current;
}

} // namespace TDeck
} // namespace Hardware
#endif // ARDUINO
