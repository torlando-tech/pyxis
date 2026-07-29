// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#ifndef PYXIS_UI_LXMF_MAP_SCREEN_PRESENTER_H
#define PYXIS_UI_LXMF_MAP_SCREEN_PRESENTER_H

#include <cstddef>
#include <cstdint>

#include "MapViewModel.h"
#include "Hardware/TDeck/MapTileStore.h"

namespace Pyxis {

enum class MapTileLoadResult : std::uint8_t {
    READY,
    MISS,
    STORAGE_UNAVAILABLE,
    INVALID_PNG,
    TOO_LARGE,
    IO_ERROR
};

struct MapTileRequest {
    std::uint32_t generation;
    std::uint32_t frame_epoch;
    std::uint32_t slot_token;
    std::uint8_t slot_index;
    Hardware::TDeck::TileKey key;
};

struct MapTileCompletion {
    std::uint32_t generation;
    std::uint32_t frame_epoch;
    std::uint32_t slot_token;
    std::uint8_t slot_index;
    Hardware::TDeck::TileKey key;
    MapTileLoadResult result;
};

struct MapTileSlot {
    enum State : std::uint8_t {
        EMPTY,
        PENDING,
        READY,
        MISS,
        STORAGE_UNAVAILABLE,
        INVALID_PNG,
        TOO_LARGE,
        IO_ERROR
    };

    State state;
    Hardware::TDeck::TileKey key;
    std::uint32_t token;
    double screen_x;
    double screen_y;
};

/**
 * Portable, allocation-free state owner for the 320x208 offline map.
 *
 * Calls crossing worker/UI task boundaries must be externally serialized. The
 * presenter itself contains only fixed arrays, making that lock scope short and
 * independent of LVGL, SD, and decoder work.
 */
class MapScreenPresenter {
public:
    enum : std::size_t {
        TILE_SLOT_COUNT = 6,
        TILE_REQUEST_CAPACITY = 6,
        TILE_COMPLETION_CAPACITY = 6
    };
    enum : std::uint32_t {
        VIEWPORT_WIDTH = 320,
        VIEWPORT_HEIGHT = 208
    };

    MapScreenPresenter();

    void show();
    void hide();
    bool active() const { return active_; }

    bool setView(const MapProjection::GeoPoint& center, std::uint32_t zoom);
    bool panPixels(double delta_x, double delta_y);
    bool zoomBy(int delta);
    bool recenter(bool has_fix,
                  const Telemetry::LocationTelemetry& location);

    MapView::Result buildFrame(const MapView::Request& request);
    bool takeRequest(MapTileRequest& output);
    bool publishCompletion(const MapTileCompletion& completion);
    bool takeApplicableCompletion(MapTileCompletion& output);

    const MapView::Frame& frame() const { return frame_; }
    const MapTileSlot& slot(std::size_t index) const { return slots_[index]; }
    const MapProjection::GeoPoint& center() const { return center_; }
    std::uint32_t zoom() const { return zoom_; }
    std::uint32_t generation() const { return generation_; }
    std::uint32_t frameEpoch() const { return frame_epoch_; }
    std::size_t requestCount() const { return request_count_; }
    std::size_t completionCount() const { return completion_count_; }

private:
    MapProjection::GeoPoint center_;
    std::uint32_t zoom_;
    std::uint32_t generation_;
    std::uint32_t frame_epoch_;
    bool active_;
    bool frame_built_for_epoch_;
    MapView::Frame frame_;
    MapTileSlot slots_[TILE_SLOT_COUNT];
    MapTileRequest requests_[TILE_REQUEST_CAPACITY];
    MapTileCompletion completions_[TILE_COMPLETION_CAPACITY];
    std::size_t request_head_;
    std::size_t request_count_;
    std::size_t completion_head_;
    std::size_t completion_count_;

    static bool sameKey(const Hardware::TDeck::TileKey& left,
                        const Hardware::TDeck::TileKey& right);
    static Hardware::TDeck::TileKey keyFor(
        const MapProjection::TilePlacement& placement);
    static std::uint32_t advance(std::uint32_t value);
    void startNewFrame();
    void clearSlots();
    void clearQueues();
    int findSlot(const Hardware::TDeck::TileKey& key) const;
    int findFreeSlot() const;
    bool enqueueRequest(std::size_t slot_index);
    bool completionMatches(const MapTileCompletion& completion) const;
    static MapTileSlot::State stateFor(MapTileLoadResult result);
};

static_assert(MapScreenPresenter::TILE_SLOT_COUNT == 6,
              "320x208 map viewport requires exactly six reusable tile slots");
static_assert(MapScreenPresenter::TILE_REQUEST_CAPACITY == 6,
              "tile request queue must remain bounded to one per slot");
static_assert(MapScreenPresenter::TILE_COMPLETION_CAPACITY == 6,
              "tile completion queue must remain bounded to one per slot");

}  // namespace Pyxis

#endif
