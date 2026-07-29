// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#include "MapTileDownloader.h"

#include <cstdio>
#include <cstring>

namespace Hardware {
namespace TDeck {

namespace {
const char DEFAULT_ENDPOINT[] = "https://tile.openstreetmap.org";
const char USER_AGENT_PREFIX[] = "Pyxis/";
const char USER_AGENT_SUFFIX[] = " (+https://github.com/torlando-tech/pyxis)";

char asciiLower(char value) {
    return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
}

bool validKey(const TileKey& key) {
    if (key.zoom > MapTileStore::MAX_ZOOM) return false;
    const std::uint32_t count = UINT32_C(1) << key.zoom;
    return key.x < count && key.y < count;
}
}

MapTileDownloadConfig::MapTileDownloadConfig()
    : endpoint(DEFAULT_ENDPOINT), ca_certificate(NULL), firmware_version(NULL),
      overall_timeout_ms(15000U), connect_timeout_ms(5000U), read_timeout_ms(5000U) {}

MapTileDownloader::MapTileDownloader(MapTileDownloadStore& store, MapTileTransport& transport,
    MapTileDownloadClock& clock, const MapTileDownloadPolicy& policy,
    const MapTileDownloadConfig& config)
    : store_(store), transport_(transport), clock_(clock), policy_(policy), config_(config),
      queue_{}, queue_count_(0U), results_{}, result_head_(0U), result_count_(0U),
      dropped_results_(0U), current_{}, active_(false), transport_open_(false),
      store_open_(false), stage_(Stage::SELECTED), last_now_(0U), deadline_(0U),
      received_(0U), expected_length_(-1), url_{0}, user_agent_{0}, chunk_{0} {}

MapTileDownloader::~MapTileDownloader() {
    if (store_open_) store_.abortPut();
    if (transport_open_) transport_.close();
}

bool MapTileDownloader::sameKey(const TileKey& a, const TileKey& b) {
    return a.zoom == b.zoom && a.x == b.x && a.y == b.y;
}

MapTileUrlResult MapTileDownloader::canonicalUrl(const char* endpoint, const TileKey& key,
    char* output, std::size_t capacity) {
    if (output == NULL || capacity == 0U || endpoint == NULL || endpoint[0] == '\0') {
        return MapTileUrlResult::INVALID_ARGUMENT;
    }
    output[0] = '\0';
    if (!validKey(key)) return MapTileUrlResult::INVALID_KEY;
    static const char scheme[] = "https://";
    if (std::strncmp(endpoint, scheme, sizeof(scheme) - 1U) != 0) {
        return MapTileUrlResult::INVALID_ARGUMENT;
    }
    const char* authority = endpoint + sizeof(scheme) - 1U;
    if (*authority == '\0' || *authority == '/') {
        return MapTileUrlResult::INVALID_ARGUMENT;
    }
    for (const char* cursor = authority; *cursor != '\0' && *cursor != '/'; ++cursor) {
        if (*cursor == '@' || *cursor == '?' || *cursor == '#') {
            return MapTileUrlResult::INVALID_ARGUMENT;
        }
    }
    std::size_t endpoint_size = std::strlen(endpoint);
    while (endpoint_size != 0U && endpoint[endpoint_size - 1U] == '/') --endpoint_size;
    if (endpoint_size == 0U) return MapTileUrlResult::INVALID_ARGUMENT;
    const int count = std::snprintf(output, capacity, "%.*s/%u/%lu/%lu.png",
        static_cast<int>(endpoint_size), endpoint, static_cast<unsigned>(key.zoom),
        static_cast<unsigned long>(key.x), static_cast<unsigned long>(key.y));
    if (count < 0 || static_cast<std::size_t>(count) >= capacity) {
        output[0] = '\0';
        return MapTileUrlResult::TOO_LONG;
    }
    return MapTileUrlResult::OK;
}

MapTileEnqueueResult MapTileDownloader::enqueue(const TileKey& key, std::uint32_t generation) {
    if (!policy_.enabled) return MapTileEnqueueResult::DISABLED;
    if (!validKey(key)) return MapTileEnqueueResult::INVALID_KEY;
    if (active_ && sameKey(current_.key, key)) return MapTileEnqueueResult::DUPLICATE;
    for (std::size_t i = 0U; i < queue_count_; ++i) {
        if (sameKey(queue_[i].key, key)) return MapTileEnqueueResult::DUPLICATE;
    }
    if (queue_count_ == QUEUE_CAPACITY) return MapTileEnqueueResult::QUEUE_FULL;
    queue_[queue_count_].key = key;
    queue_[queue_count_].generation = generation;
    ++queue_count_;
    return MapTileEnqueueResult::ACCEPTED;
}

void MapTileDownloader::publish(MapTileResultCode code) {
    MapTileDownloadResult result = {current_.key, current_.generation, code, received_};
    if (result_count_ == RESULT_CAPACITY) {
        ++dropped_results_;
        return;
    }
    const std::size_t index = (result_head_ + result_count_) % RESULT_CAPACITY;
    results_[index] = result;
    ++result_count_;
}

void MapTileDownloader::finish(MapTileResultCode code, bool abort_store) {
    if (abort_store && store_open_) store_.abortPut();
    if (transport_open_) transport_.close();
    publish(code);
    active_ = false;
    transport_open_ = false;
    store_open_ = false;
    received_ = 0U;
    expected_length_ = -1;
}

std::size_t MapTileDownloader::cancelGeneration(std::uint32_t generation) {
    std::size_t canceled = 0U;
    if (active_ && current_.generation == generation) {
        finish(MapTileResultCode::CANCELED, true);
        ++canceled;
    }
    std::size_t write = 0U;
    for (std::size_t read = 0U; read < queue_count_; ++read) {
        if (queue_[read].generation == generation) {
            current_ = queue_[read];
            received_ = 0U;
            publish(MapTileResultCode::CANCELED);
            ++canceled;
        } else {
            if (write != read) queue_[write] = queue_[read];
            ++write;
        }
    }
    queue_count_ = write;
    return canceled;
}

bool MapTileDownloader::validPngContentType(const char* value) {
    if (value == NULL) return false;
    static const char expected[] = "image/png";
    std::size_t i = 0U;
    for (; i < sizeof(expected) - 1U; ++i) {
        if (value[i] == '\0' || asciiLower(value[i]) != expected[i]) return false;
    }
    const char* cursor = value + i;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    return *cursor == '\0' || *cursor == ';';
}

bool MapTileDownloader::checkClock() {
    const std::uint64_t now = clock_.nowMs();
    if (now < last_now_) {
        finish(MapTileResultCode::CLOCK_ERROR, true);
        return false;
    }
    last_now_ = now;
    if (now > deadline_) {
        finish(MapTileResultCode::TIMEOUT, true);
        return false;
    }
    return true;
}

MapTilePumpResult MapTileDownloader::pump() {
    if (!active_) {
        if (queue_count_ == 0U) return MapTilePumpResult::IDLE;
        current_ = queue_[0];
        for (std::size_t i = 1U; i < queue_count_; ++i) queue_[i - 1U] = queue_[i];
        --queue_count_;
        received_ = 0U;
        expected_length_ = -1;
        transport_open_ = false;
        store_open_ = false;
        stage_ = Stage::SELECTED;
        last_now_ = clock_.nowMs();
        const std::uint64_t room = UINT64_MAX - last_now_;
        deadline_ = config_.overall_timeout_ms > room ? UINT64_MAX :
            last_now_ + static_cast<std::uint64_t>(config_.overall_timeout_ms);
        active_ = true;
        if (canonicalUrl(config_.endpoint, current_.key, url_, sizeof(url_)) != MapTileUrlResult::OK ||
            config_.firmware_version == NULL || config_.ca_certificate == NULL || config_.ca_certificate[0] == '\0') {
            finish(MapTileResultCode::URL_ERROR, false);
            return MapTilePumpResult::PROGRESSED;
        }
        const int agent_size = std::snprintf(user_agent_, sizeof(user_agent_), "%s%s%s",
            USER_AGENT_PREFIX, config_.firmware_version, USER_AGENT_SUFFIX);
        if (agent_size < 0 || static_cast<std::size_t>(agent_size) >= sizeof(user_agent_)) {
            finish(MapTileResultCode::URL_ERROR, false);
        }
        return MapTilePumpResult::PROGRESSED;
    }

    if (!checkClock()) return MapTilePumpResult::PROGRESSED;
    if (!store_.isAvailable()) {
        finish(MapTileResultCode::STORE_UNAVAILABLE, true);
        return MapTilePumpResult::PROGRESSED;
    }

    if (stage_ == Stage::SELECTED) {
        TileHttpResponse response = {0, -1, NULL};
        const TileTransportResult started = transport_.start(url_, user_agent_, config_.ca_certificate,
            config_.connect_timeout_ms, config_.read_timeout_ms, response);
        if (started != TileTransportResult::OK) {
            finish(started == TileTransportResult::TIMEOUT ? MapTileResultCode::TIMEOUT :
                MapTileResultCode::TRANSPORT_ERROR, false);
            return MapTilePumpResult::PROGRESSED;
        }
        transport_open_ = true;
        if (response.status_code != 200) {
            finish(MapTileResultCode::HTTP_STATUS_ERROR, false);
            return MapTilePumpResult::PROGRESSED;
        }
        if (!validPngContentType(response.content_type)) {
            finish(MapTileResultCode::CONTENT_TYPE_ERROR, false);
            return MapTilePumpResult::PROGRESSED;
        }
        expected_length_ = response.content_length;
        if (expected_length_ < -1) {
            finish(MapTileResultCode::LENGTH_MISMATCH, false);
            return MapTilePumpResult::PROGRESSED;
        }
        if (expected_length_ > static_cast<std::int64_t>(store_.maxTileBytes())) {
            finish(MapTileResultCode::TOO_LARGE, false);
            return MapTilePumpResult::PROGRESSED;
        }
        stage_ = Stage::TRANSPORT_STARTED;
        return MapTilePumpResult::PROGRESSED;
    }

    if (stage_ == Stage::TRANSPORT_STARTED) {
        const TileStoreResult result = store_.beginPut(current_.key);
        if (result != TileStoreResult::OK) {
            finish(result == TileStoreResult::STORAGE_UNAVAILABLE ? MapTileResultCode::STORE_UNAVAILABLE :
                MapTileResultCode::STORE_ERROR, false);
            return MapTilePumpResult::PROGRESSED;
        }
        store_open_ = true;
        stage_ = Stage::READING;
        return MapTilePumpResult::PROGRESSED;
    }

    std::size_t count = 0U;
    bool eof = false;
    const TileTransportResult read_result = transport_.read(chunk_, sizeof(chunk_), count, eof);
    if (read_result != TileTransportResult::OK || count > sizeof(chunk_)) {
        finish(read_result == TileTransportResult::TIMEOUT ? MapTileResultCode::TIMEOUT :
            MapTileResultCode::READ_ERROR, true);
        return MapTilePumpResult::PROGRESSED;
    }
    // A blocking transport read may consume the remaining overall budget.
    // Recheck before committing bytes or accepting EOF.
    if (!checkClock()) return MapTilePumpResult::PROGRESSED;
    const std::uint32_t maximum = store_.maxTileBytes();
    if (count > maximum || received_ > maximum - static_cast<std::uint32_t>(count)) {
        finish(MapTileResultCode::TOO_LARGE, true);
        return MapTilePumpResult::PROGRESSED;
    }
    if (count != 0U) {
        const TileStoreResult written = store_.writePutChunk(chunk_, count);
        if (written != TileStoreResult::OK) {
            finish(written == TileStoreResult::STORAGE_UNAVAILABLE ? MapTileResultCode::STORE_UNAVAILABLE :
                MapTileResultCode::STORE_ERROR, true);
            return MapTilePumpResult::PROGRESSED;
        }
        received_ += static_cast<std::uint32_t>(count);
    }
    if (!eof) return MapTilePumpResult::PROGRESSED;
    if (expected_length_ >= 0 && static_cast<std::uint64_t>(expected_length_) != received_) {
        finish(MapTileResultCode::LENGTH_MISMATCH, true);
        return MapTilePumpResult::PROGRESSED;
    }
    const TileStoreResult finished = store_.finishPut();
    if (finished != TileStoreResult::OK) {
        finish(finished == TileStoreResult::STORAGE_UNAVAILABLE ? MapTileResultCode::STORE_UNAVAILABLE :
            MapTileResultCode::STORE_ERROR, true);
        return MapTilePumpResult::PROGRESSED;
    }
    store_open_ = false;
    finish(MapTileResultCode::SUCCESS, false);
    return MapTilePumpResult::PROGRESSED;
}

bool MapTileDownloader::takeResult(MapTileDownloadResult& result) {
    if (result_count_ == 0U) return false;
    result = results_[result_head_];
    result_head_ = (result_head_ + 1U) % RESULT_CAPACITY;
    --result_count_;
    return true;
}

} // namespace TDeck
} // namespace Hardware
