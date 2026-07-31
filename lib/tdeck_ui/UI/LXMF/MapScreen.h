// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_MAP_SCREEN_H
#define UI_LXMF_MAP_SCREEN_H

#include "MapScreenPresenter.h"
#include "DecodedTileCache.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <atomic>
#include <functional>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "Hardware/TDeck/MapTileStore.h"
#include "Hardware/TDeck/MapTileStoreSD.h"
#include "Hardware/TDeck/MapTilePack.h"
#include "Hardware/TDeck/MapTileDownloader.h"
#include "Hardware/TDeck/MapTileHttpArduino.h"

namespace UI {
namespace LXMF {

class MapScreen {
public:
    using BackCallback = std::function<void()>;

    enum : std::size_t {
        TILE_COUNT = Pyxis::MapScreenPresenter::TILE_SLOT_COUNT,
        MARKER_COUNT = 33,
        MAX_COMPLETIONS_PER_TICK = 1,
        READ_CHUNK_BYTES = 4096
    };
    enum : std::uint32_t {
        MAX_COMPRESSED_TILE_BYTES = 384U * 1024U
    };

    explicit MapScreen(lv_obj_t* parent = nullptr);
    ~MapScreen();

    void set_back_callback(BackCallback callback) { back_callback_ = callback; }
    void show();
    void hide();

    // These methods never call LVGL and are invoked before LVGL_LOCK.
    void serviceIo();
    void updateModel(const Pyxis::MapView::Request& request);
    void setDownloadEnabled(bool enabled);

    // These methods only mutate the pre-created object pool and are invoked
    // while UIManager owns LVGL_LOCK.
    void applyFrame();
    bool applyOneCompletion();

private:
    lv_obj_t* screen_;
    lv_obj_t* toolbar_;
    lv_obj_t* viewport_;
    lv_obj_t* status_label_;
    lv_obj_t* attribution_label_;
    lv_obj_t* zoom_label_;
    lv_obj_t* back_button_;
    lv_obj_t* zoom_out_button_;
    lv_obj_t* zoom_in_button_;
    lv_obj_t* recenter_button_;
    lv_obj_t* pan_buttons_[4];
    lv_obj_t* tile_images_[TILE_COUNT];
    lv_img_dsc_t tile_descriptors_[TILE_COUNT];
    lv_color_t* tile_pixels_[TILE_COUNT];
    Pyxis::DecodedTileCache decoded_tile_cache_;
    std::uint16_t* decoded_cache_pixels_[Pyxis::DecodedTileCache::CAPACITY];
    lv_obj_t* approximation_halos_[MARKER_COUNT];
    lv_obj_t* markers_[MARKER_COUNT];
    lv_obj_t* marker_labels_[MARKER_COUNT];

    Pyxis::MapScreenPresenter presenter_;
    Hardware::TDeck::MapTileStoreSD storage_;
    Hardware::TDeck::TileStoreConfig store_config_;
    Hardware::TDeck::MapTileStore store_;
    Hardware::TDeck::MapTilePack pack_;
    Hardware::TDeck::MapTileStoreDownloadAdapter download_store_;
    Hardware::TDeck::MapTileHttpArduino download_transport_;
    Hardware::TDeck::MapTileMillisClock download_clock_;
    Hardware::TDeck::MapTileDownloadPolicy download_policy_;
    Hardware::TDeck::MapTileDownloadConfig download_config_;
    Hardware::TDeck::MapTileDownloader downloader_;
    std::atomic<bool> downloads_enabled_;
    std::atomic<bool> screen_visible_;
    std::atomic<std::uint32_t> pack_refresh_epoch_;
    std::atomic<std::uint32_t> transport_close_epoch_;
    std::uint32_t download_failed_frame_epoch_;
    Hardware::TDeck::TileKey decode_failed_keys_[TILE_COUNT];
    std::uint32_t decode_failed_generations_[TILE_COUNT];
    std::uint8_t* compressed_staging_;
    SemaphoreHandle_t state_mutex_;
    SemaphoreHandle_t transport_start_mutex_;
    TaskHandle_t worker_task_;
    std::atomic<bool> stop_requested_;
    std::atomic<bool> worker_exited_;
    bool worker_started_;
    bool store_initialized_;
    bool requests_released_;
    bool has_location_fix_;
    bool center_initialized_;
    Telemetry::LocationTelemetry current_location_;
    bool dragging_;
    lv_point_t last_drag_point_;
    BackCallback back_callback_;

    static void workerEntry(void* context);
    enum class CompressedTileSource : std::uint8_t {
        PACK = 0,
        LIVE_STORE
    };
    void workerLoop();
    Pyxis::MapTileLoadResult loadTile(const Pyxis::MapTileRequest& request,
                                      std::uint32_t transport_epoch);
    Pyxis::MapTileLoadResult readTile(const Pyxis::MapTileRequest& request);
    Pyxis::MapTileLoadResult readCompressedTile(
        const Pyxis::MapTileRequest& request, CompressedTileSource source);
    Pyxis::MapTileLoadResult downloadTile(const Pyxis::MapTileRequest& request,
                                          std::uint32_t transport_epoch);
    bool decodeFailedFor(const Pyxis::MapTileRequest& request) const;
    void markDecodeFailed(const Pyxis::MapTileRequest& request);
    bool startWorker();
    void stopWorker();
    bool lockState(TickType_t ticks = portMAX_DELAY);
    void unlockState();
    void synchronizeTransportStart();
    void setPlaceholder(std::size_t index);
    void setStatusFor(Pyxis::MapTileLoadResult result);
    void pan(double dx, double dy);

    static MapScreen* fromEvent(lv_event_t* event);
    static void onBack(lv_event_t* event);
    static void onZoomIn(lv_event_t* event);
    static void onZoomOut(lv_event_t* event);
    static void onRecenter(lv_event_t* event);
    static void onPan(lv_event_t* event);
    static void onMapPressed(lv_event_t* event);
    static void onMapPressing(lv_event_t* event);
    static void onMapReleased(lv_event_t* event);
};

static_assert(MapScreen::TILE_COUNT == 6,
              "MapScreen owns exactly six permanent tile image objects");
static_assert(MapScreen::MARKER_COUNT ==
                  static_cast<std::size_t>(Pyxis::MapView::MAX_MAP_MARKERS),
              "MapScreen marker pool must cover local plus all peer snapshots");
static_assert(MapScreen::MARKER_COUNT <= 33,
              "MapScreen marker object pools must remain bounded");
static_assert(MapScreen::MAX_COMPRESSED_TILE_BYTES == 384U * 1024U,
              "compressed tile staging cap is part of the memory contract");

}  // namespace LXMF
}  // namespace UI

#endif  // ARDUINO
#endif
