// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_MAP_TILE_DOWNLOADER_H
#define HARDWARE_TDECK_MAP_TILE_DOWNLOADER_H

#include "MapTileStore.h"

#include <cstddef>
#include <cstdint>

namespace Hardware {
namespace TDeck {

/** Optional network policy. Construction is deliberately disabled by default. */
struct MapTileDownloadPolicy {
    bool enabled;
    MapTileDownloadPolicy() : enabled(false) {}
};

struct MapTileDownloadConfig {
    const char* endpoint;
    const char* ca_certificate;
    const char* firmware_version;
    std::uint32_t overall_timeout_ms;
    std::uint32_t connect_timeout_ms;
    std::uint32_t read_timeout_ms;
    MapTileDownloadConfig();
};

enum class TileTransportResult : std::uint8_t { OK, ERROR, TIMEOUT };

struct TileHttpResponse {
    int status_code;
    std::int64_t content_length; // -1 means absent
    const char* content_type;    // valid until close()
};

class MapTileTransport {
public:
    virtual ~MapTileTransport() {}
    virtual TileTransportResult start(const char* url, const char* user_agent,
        const char* ca_certificate, std::uint32_t connect_timeout_ms,
        std::uint32_t read_timeout_ms, TileHttpResponse& response) = 0;
    virtual TileTransportResult read(std::uint8_t* output, std::size_t capacity,
        std::size_t& count, bool& eof) = 0;
    virtual void close() = 0;
    /** Hard-reset transport state before a bounded reconnect attempt. */
    virtual void reset() { close(); }
};

class MapTileDownloadClock {
public:
    virtual ~MapTileDownloadClock() {}
    virtual std::uint64_t nowMs() const = 0;
};

/** Narrow store boundary; MapTileStore can be wired through MapTileStoreDownloadAdapter. */
class MapTileDownloadStore {
public:
    virtual ~MapTileDownloadStore() {}
    virtual bool isAvailable() const = 0;
    virtual std::uint32_t maxTileBytes() const = 0;
    virtual TileStoreResult beginPut(const TileKey& key) = 0;
    virtual TileStoreResult writePutChunk(const std::uint8_t* data, std::size_t size) = 0;
    virtual TileStoreResult finishPut() = 0;
    virtual void abortPut() = 0;
};

class MapTileStoreDownloadAdapter : public MapTileDownloadStore {
public:
    explicit MapTileStoreDownloadAdapter(MapTileStore& store) : store_(store) {}
    virtual bool isAvailable() const { return store_.isAvailable(); }
    virtual std::uint32_t maxTileBytes() const { return store_.maxTileBytes(); }
    virtual TileStoreResult beginPut(const TileKey& key) { return store_.beginPut(key); }
    virtual TileStoreResult writePutChunk(const std::uint8_t* data, std::size_t size) { return store_.writePutChunk(data, size); }
    virtual TileStoreResult finishPut() { return store_.finishPut(); }
    virtual void abortPut() { store_.abortPut(); }
private:
    MapTileStore& store_;
};

enum class MapTileEnqueueResult : std::uint8_t { ACCEPTED, POLICY_DISABLED, INVALID_KEY, DUPLICATE, QUEUE_FULL };
enum class MapTileUrlResult : std::uint8_t { OK, INVALID_ARGUMENT, INVALID_KEY, TOO_LONG };
enum class MapTilePumpResult : std::uint8_t { IDLE, PROGRESSED };
enum class MapTileResultCode : std::uint8_t {
    SUCCESS, CANCELED, URL_ERROR, TRANSPORT_ERROR, HTTP_STATUS_ERROR,
    CONTENT_TYPE_ERROR, TOO_LARGE, LENGTH_MISMATCH, READ_ERROR,
    STORE_UNAVAILABLE, STORE_ERROR, TIMEOUT, CLOCK_ERROR
};

struct MapTileDownloadResult {
    TileKey key;
    std::uint32_t generation;
    MapTileResultCode code;
    std::uint32_t bytes;
};

/**
 * Fixed-capacity, caller-pumped downloader for visible slippy-map tiles.
 *
 * Requests contain only TileKey + generation. A failed transport start gets one
 * hard-reset reconnect attempt; there is no content retry, prefetch, background
 * bulk mode, credential support, or hidden URL input. Keep one
 * visible tile request active at a time. Users of the default public endpoint
 * must preserve visible OpenStreetMap attribution in the eventual map UI and
 * comply with https://operations.osmfoundation.org/policies/tiles/ .
 */
class MapTileDownloader {
public:
    static const std::size_t QUEUE_CAPACITY = 6U;
    static const std::size_t RESULT_CAPACITY = 6U;
    static const std::size_t CHUNK_CAPACITY = 4096U;
    static const std::size_t URL_CAPACITY = 128U;
    static const std::size_t USER_AGENT_CAPACITY = 96U;

    MapTileDownloader(MapTileDownloadStore& store, MapTileTransport& transport,
        MapTileDownloadClock& clock, const MapTileDownloadPolicy& policy,
        const MapTileDownloadConfig& config);
    ~MapTileDownloader();

    static MapTileUrlResult canonicalUrl(const char* endpoint, const TileKey& key,
        char* output, std::size_t capacity);
    MapTileEnqueueResult enqueue(const TileKey& key, std::uint32_t generation);
    /** Worker-owner only: disabling aborts active and queued work immediately. */
    void setEnabled(bool enabled);
    std::size_t cancelGeneration(std::uint32_t generation);
    MapTilePumpResult pump();
    bool takeResult(MapTileDownloadResult& result);

    bool isBusy() const { return active_ || queue_count_ != 0U; }
    bool willStartTransportOnNextPump() const {
        return active_ && stage_ == Stage::SELECTED;
    }
    std::size_t queuedCount() const { return queue_count_; }
    std::size_t resultCount() const { return result_count_; }
    std::uint32_t droppedResultCount() const { return dropped_results_; }
    std::uint64_t lastDeadline() const { return deadline_; }

private:
    struct Request { TileKey key; std::uint32_t generation; };
    enum class Stage : std::uint8_t { SELECTED, TRANSPORT_STARTED, STORE_STARTED, READING };

    MapTileDownloadStore& store_;
    MapTileTransport& transport_;
    MapTileDownloadClock& clock_;
    MapTileDownloadPolicy policy_;
    MapTileDownloadConfig config_;
    Request queue_[QUEUE_CAPACITY];
    std::size_t queue_count_;
    MapTileDownloadResult results_[RESULT_CAPACITY];
    std::size_t result_head_;
    std::size_t result_count_;
    std::uint32_t dropped_results_;
    Request current_;
    bool active_;
    bool transport_open_;
    bool transport_retry_used_;
    bool store_open_;
    Stage stage_;
    std::uint64_t last_now_;
    std::uint64_t deadline_;
    std::uint32_t received_;
    std::int64_t expected_length_;
    char url_[URL_CAPACITY];
    char user_agent_[USER_AGENT_CAPACITY];
    std::uint8_t chunk_[CHUNK_CAPACITY];

    static bool sameKey(const TileKey& a, const TileKey& b);
    static bool validPngContentType(const char* value);
    void publish(MapTileResultCode code);
    void finish(MapTileResultCode code, bool abort_store);
    bool checkClock();
};

} // namespace TDeck
} // namespace Hardware

#endif
