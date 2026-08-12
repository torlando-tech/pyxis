// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#include "MapScreenPresenter.h"

#include <cmath>
#include <cstring>

namespace Pyxis {
namespace {

bool validTelemetry(const Telemetry::LocationTelemetry& location) {
    return location.latitude_e6 >= -90000000 &&
           location.latitude_e6 <= 90000000 &&
           location.longitude_e6 >= -180000000 &&
           location.longitude_e6 <= 180000000;
}

}  // namespace

MapScreenPresenter::MapScreenPresenter()
    : center_{0.0, 0.0}, zoom_(2U), generation_(1U), frame_epoch_(1U),
      active_(false), frame_built_for_epoch_(false), frame_{}, slots_{},
      requests_{}, completions_{}, request_head_(0U), request_count_(0U),
      completion_head_(0U), completion_count_(0U) {
    clearSlots();
}

std::uint32_t MapScreenPresenter::advance(std::uint32_t value) {
    ++value;
    return value == 0U ? 1U : value;
}

bool MapScreenPresenter::sameKey(const Hardware::TDeck::TileKey& left,
                                 const Hardware::TDeck::TileKey& right) {
    return left.zoom == right.zoom && left.x == right.x && left.y == right.y;
}

Hardware::TDeck::TileKey MapScreenPresenter::keyFor(
    const MapProjection::TilePlacement& placement) {
    Hardware::TDeck::TileKey key{};
    key.zoom = static_cast<std::uint8_t>(placement.tile.zoom);
    key.x = placement.tile.x;
    key.y = placement.tile.y;
    return key;
}

void MapScreenPresenter::clearSlots() {
    for (std::size_t index = 0; index < TILE_SLOT_COUNT; ++index) {
        slots_[index].state = MapTileSlot::EMPTY;
        slots_[index].key = Hardware::TDeck::TileKey{0U, 0U, 0U};
        slots_[index].token = advance(slots_[index].token);
        slots_[index].screen_x = 0.0;
        slots_[index].screen_y = 0.0;
    }
}

void MapScreenPresenter::clearQueues() {
    request_head_ = 0U;
    request_count_ = 0U;
    completion_head_ = 0U;
    completion_count_ = 0U;
}

void MapScreenPresenter::show() {
    generation_ = advance(generation_);
    frame_epoch_ = advance(frame_epoch_);
    active_ = true;
    frame_built_for_epoch_ = false;
    clearQueues();
    clearSlots();
}

void MapScreenPresenter::hide() {
    generation_ = advance(generation_);
    frame_epoch_ = advance(frame_epoch_);
    active_ = false;
    frame_built_for_epoch_ = false;
    clearQueues();
    clearSlots();
    frame_.tile_count = 0U;
    frame_.marker_count = 0U;
}

void MapScreenPresenter::startNewFrame() {
    frame_epoch_ = advance(frame_epoch_);
    frame_built_for_epoch_ = false;
    request_head_ = 0U;
    request_count_ = 0U;
    completion_head_ = 0U;
    completion_count_ = 0U;
}

bool MapScreenPresenter::setView(const MapProjection::GeoPoint& center,
                                 std::uint32_t zoom) {
    if (!std::isfinite(center.latitude) || !std::isfinite(center.longitude) ||
        !MapProjection::isValidZoom(zoom)) {
        return false;
    }
    MapProjection::GeoPoint normalized{};
    normalized.latitude = MapProjection::clampLatitude(center.latitude);
    normalized.longitude = MapProjection::normalizeLongitude(center.longitude);
    if (center_.latitude == normalized.latitude &&
        center_.longitude == normalized.longitude && zoom_ == zoom) {
        return false;
    }
    center_ = normalized;
    zoom_ = zoom;
    startNewFrame();
    return true;
}

bool MapScreenPresenter::panPixels(double delta_x, double delta_y) {
    if (!std::isfinite(delta_x) || !std::isfinite(delta_y)) return false;
    if (delta_x == 0.0 && delta_y == 0.0) return false;
    MapProjection::GlobalPixel pixel{};
    MapProjection::GlobalPixel panned{};
    MapProjection::GeoPoint center{};
    if (MapProjection::latLonToGlobalPixel(center_, zoom_, pixel) !=
            MapProjection::Status::OK ||
        MapProjection::panGlobalPixel(pixel, delta_x, delta_y, zoom_, panned) !=
            MapProjection::Status::OK ||
        MapProjection::globalPixelToLatLon(panned, zoom_, center) !=
            MapProjection::Status::OK) {
        return false;
    }
    center_ = center;
    startNewFrame();
    return true;
}

bool MapScreenPresenter::zoomBy(int delta) {
    const int proposed = static_cast<int>(zoom_) + delta;
    const std::uint32_t bounded = proposed < 0
        ? 0U
        : (proposed > static_cast<int>(MapProjection::MAX_ZOOM)
               ? static_cast<std::uint32_t>(MapProjection::MAX_ZOOM)
               : static_cast<std::uint32_t>(proposed));
    if (bounded == zoom_) return false;
    zoom_ = bounded;
    startNewFrame();
    return true;
}

bool MapScreenPresenter::recenter(
    bool has_fix, const Telemetry::LocationTelemetry& location) {
    if (!has_fix || !validTelemetry(location)) return false;
    MapProjection::GeoPoint center{};
    center.latitude = static_cast<double>(location.latitude_e6) / 1000000.0;
    center.longitude = static_cast<double>(location.longitude_e6) / 1000000.0;
    if (center_.latitude == center.latitude && center_.longitude == center.longitude) {
        return true;
    }
    center_ = center;
    startNewFrame();
    return true;
}

void MapScreenPresenter::invalidateTiles() {
    startNewFrame();
    clearSlots();
    frame_.tile_count = 0U;
}

int MapScreenPresenter::findSlot(const Hardware::TDeck::TileKey& key) const {
    for (std::size_t index = 0; index < TILE_SLOT_COUNT; ++index) {
        if (slots_[index].state != MapTileSlot::EMPTY &&
            sameKey(slots_[index].key, key)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int MapScreenPresenter::findFreeSlot() const {
    for (std::size_t index = 0; index < TILE_SLOT_COUNT; ++index) {
        if (slots_[index].state == MapTileSlot::EMPTY) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

bool MapScreenPresenter::enqueueRequest(std::size_t slot_index) {
    if (request_count_ >= TILE_REQUEST_CAPACITY) return false;
    const std::size_t tail =
        (request_head_ + request_count_) % TILE_REQUEST_CAPACITY;
    MapTileRequest& request = requests_[tail];
    request.generation = generation_;
    request.frame_epoch = frame_epoch_;
    request.slot_token = slots_[slot_index].token;
    request.slot_index = static_cast<std::uint8_t>(slot_index);
    request.key = slots_[slot_index].key;
    ++request_count_;
    return true;
}

MapView::Result MapScreenPresenter::buildFrame(const MapView::Request& request) {
    if (!active_) return MapView::Result::INVALID_ARGUMENT;
    MapView::Request effective = request;
    effective.center = center_;
    effective.zoom = zoom_;
    effective.width = VIEWPORT_WIDTH;
    effective.height = VIEWPORT_HEIGHT;
    effective.include_tile_border = false;

    MapView::Frame candidate{};
    const MapView::Result result = MapView::buildFrame(effective, candidate);
    if (result != MapView::Result::OK) return result;
    if (candidate.tile_count > TILE_SLOT_COUNT) {
        return MapView::Result::VIEWPORT_TOO_LARGE;
    }

    if (frame_built_for_epoch_) {
        frame_ = candidate;
        for (std::size_t tile = 0; tile < candidate.tile_count; ++tile) {
            const int slot_index = findSlot(keyFor(candidate.tiles[tile]));
            if (slot_index >= 0) {
                slots_[static_cast<std::size_t>(slot_index)].screen_x =
                    candidate.tiles[tile].screen_x;
                slots_[static_cast<std::size_t>(slot_index)].screen_y =
                    candidate.tiles[tile].screen_y;
            }
        }
        return MapView::Result::OK;
    }

    bool keep[TILE_SLOT_COUNT] = {};
    for (std::size_t tile = 0; tile < candidate.tile_count; ++tile) {
        const Hardware::TDeck::TileKey key = keyFor(candidate.tiles[tile]);
        const int slot_index = findSlot(key);
        if (slot_index >= 0 && slots_[static_cast<std::size_t>(slot_index)].state ==
                                   MapTileSlot::READY) {
            const std::size_t slot = static_cast<std::size_t>(slot_index);
            keep[slot] = true;
            slots_[slot].screen_x = candidate.tiles[tile].screen_x;
            slots_[slot].screen_y = candidate.tiles[tile].screen_y;
        }
    }
    for (std::size_t index = 0; index < TILE_SLOT_COUNT; ++index) {
        if (!keep[index]) {
            slots_[index].state = MapTileSlot::EMPTY;
            slots_[index].token = advance(slots_[index].token);
        }
    }

    request_head_ = 0U;
    request_count_ = 0U;
    for (std::size_t tile = 0; tile < candidate.tile_count; ++tile) {
        const Hardware::TDeck::TileKey key = keyFor(candidate.tiles[tile]);
        int slot_index = findSlot(key);
        if (slot_index < 0) slot_index = findFreeSlot();
        if (slot_index < 0) return MapView::Result::CAPACITY_EXCEEDED;
        MapTileSlot& slot = slots_[static_cast<std::size_t>(slot_index)];
        slot.key = key;
        slot.screen_x = candidate.tiles[tile].screen_x;
        slot.screen_y = candidate.tiles[tile].screen_y;
        if (slot.state != MapTileSlot::READY) {
            slot.state = MapTileSlot::PENDING;
            if (!enqueueRequest(static_cast<std::size_t>(slot_index))) {
                return MapView::Result::CAPACITY_EXCEEDED;
            }
        }
    }
    frame_ = candidate;
    frame_built_for_epoch_ = true;
    return MapView::Result::OK;
}

bool MapScreenPresenter::takeRequest(MapTileRequest& output) {
    if (request_count_ == 0U) return false;
    output = requests_[request_head_];
    request_head_ = (request_head_ + 1U) % TILE_REQUEST_CAPACITY;
    --request_count_;
    return true;
}

bool MapScreenPresenter::completionMatches(
    const MapTileCompletion& completion) const {
    if (!active_ || completion.generation != generation_ ||
        completion.frame_epoch != frame_epoch_ ||
        completion.slot_index >= TILE_SLOT_COUNT) {
        return false;
    }
    const MapTileSlot& slot = slots_[completion.slot_index];
    return slot.state == MapTileSlot::PENDING &&
           slot.token == completion.slot_token &&
           sameKey(slot.key, completion.key);
}

bool MapScreenPresenter::publishCompletion(
    const MapTileCompletion& completion) {
    if (!completionMatches(completion) ||
        completion_count_ >= TILE_COMPLETION_CAPACITY) {
        return false;
    }
    for (std::size_t offset = 0; offset < completion_count_; ++offset) {
        const MapTileCompletion& queued = completions_[
            (completion_head_ + offset) % TILE_COMPLETION_CAPACITY];
        if (queued.generation == completion.generation &&
            queued.frame_epoch == completion.frame_epoch &&
            queued.slot_token == completion.slot_token &&
            queued.slot_index == completion.slot_index &&
            sameKey(queued.key, completion.key)) {
            return false;
        }
    }
    const std::size_t tail =
        (completion_head_ + completion_count_) % TILE_COMPLETION_CAPACITY;
    completions_[tail] = completion;
    ++completion_count_;
    return true;
}

MapTileSlot::State MapScreenPresenter::stateFor(MapTileLoadResult result) {
    switch (result) {
        case MapTileLoadResult::READY: return MapTileSlot::READY;
        case MapTileLoadResult::MISS: return MapTileSlot::MISS;
        case MapTileLoadResult::STORAGE_UNAVAILABLE:
            return MapTileSlot::STORAGE_UNAVAILABLE;
        case MapTileLoadResult::INVALID_PNG: return MapTileSlot::INVALID_PNG;
        case MapTileLoadResult::TOO_LARGE: return MapTileSlot::TOO_LARGE;
        case MapTileLoadResult::DOWNLOAD_FAILED: return MapTileSlot::IO_ERROR;
        case MapTileLoadResult::IO_ERROR: return MapTileSlot::IO_ERROR;
    }
    return MapTileSlot::IO_ERROR;
}

bool MapScreenPresenter::takeApplicableCompletion(MapTileCompletion& output) {
    while (completion_count_ != 0U) {
        const MapTileCompletion candidate = completions_[completion_head_];
        completion_head_ =
            (completion_head_ + 1U) % TILE_COMPLETION_CAPACITY;
        --completion_count_;
        if (!completionMatches(candidate)) continue;
        slots_[candidate.slot_index].state = stateFor(candidate.result);
        output = candidate;
        return true;
    }
    return false;
}

bool MapScreenPresenter::visibleTileStatus(MapTileLoadResult& output) const {
    bool pending = false;
    bool have_terminal = false;
    unsigned severity = 0U;
    MapTileLoadResult terminal = MapTileLoadResult::MISS;
    for (std::size_t index = 0U; index < TILE_SLOT_COUNT; ++index) {
        switch (slots_[index].state) {
            case MapTileSlot::READY:
                output = MapTileLoadResult::READY;
                return true;
            case MapTileSlot::PENDING:
                pending = true;
                break;
            case MapTileSlot::MISS:
                have_terminal = true;
                break;
            case MapTileSlot::TOO_LARGE:
                if (severity < 1U) {
                    severity = 1U;
                    terminal = MapTileLoadResult::TOO_LARGE;
                }
                have_terminal = true;
                break;
            case MapTileSlot::INVALID_PNG:
                if (severity < 2U) {
                    severity = 2U;
                    terminal = MapTileLoadResult::INVALID_PNG;
                }
                have_terminal = true;
                break;
            case MapTileSlot::STORAGE_UNAVAILABLE:
                if (severity < 3U) {
                    severity = 3U;
                    terminal = MapTileLoadResult::STORAGE_UNAVAILABLE;
                }
                have_terminal = true;
                break;
            case MapTileSlot::IO_ERROR:
                severity = 4U;
                terminal = MapTileLoadResult::IO_ERROR;
                have_terminal = true;
                break;
            case MapTileSlot::EMPTY:
                break;
        }
    }
    if (pending || !have_terminal) return false;
    output = terminal;
    return true;
}

}  // namespace Pyxis
