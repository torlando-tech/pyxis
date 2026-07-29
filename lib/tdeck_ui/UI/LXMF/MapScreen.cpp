// Copyright (c) 2026 Pyxis contributors
// SPDX-License-Identifier: MIT

#include "MapScreen.h"

#ifdef ARDUINO

#include "Theme.h"
#include "../LVGL/LVGLInit.h"
#include "../LVGL/LVGLLock.h"

#include <esp_heap_caps.h>
#define LODEPNG_NO_COMPILE_CPP
extern "C" {
#include <src/extra/libs/png/lodepng.h>
}
#include <cmath>
#include <cstdio>
#include <cstring>

namespace UI {
namespace LXMF {
namespace {

constexpr std::size_t TILE_PIXEL_COUNT = 256U * 256U;
constexpr std::uint32_t STORE_BYTE_QUOTA = 64U * 1024U * 1024U;
constexpr std::uint16_t STORE_ENTRY_CAPACITY = 128U;

lv_obj_t* createToolbarButton(lv_obj_t* parent, const char* text,
                              lv_event_cb_t callback, void* context,
                              lv_coord_t width) {
    lv_obj_t* button = lv_btn_create(parent);
    lv_obj_set_size(button, width, 26);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_bg_color(button, Theme::surfaceInput(), 0);
    lv_obj_set_style_bg_color(button, Theme::primaryPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, context);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

}  // namespace

using Hardware::TDeck::MapTileStore;

static_assert(MapTileStore::HARD_MAX_ENTRIES == 128,
              "map cache index contract changed; revisit bounded RAM budget");
static_assert(sizeof(lv_color_t) == 2U,
              "offline tile buffers assume LVGL RGB565 true color");

MapScreen::MapScreen(lv_obj_t* parent)
    : screen_(nullptr), toolbar_(nullptr), viewport_(nullptr),
      status_label_(nullptr), attribution_label_(nullptr), zoom_label_(nullptr),
      zoom_out_button_(nullptr), zoom_in_button_(nullptr),
      recenter_button_(nullptr), pan_buttons_{}, tile_images_{},
      tile_descriptors_{}, tile_pixels_{}, approximation_halos_{}, markers_{}, marker_labels_{},
      presenter_(), storage_(),
      store_config_{STORE_ENTRY_CAPACITY, STORE_BYTE_QUOTA,
                    MAX_COMPRESSED_TILE_BYTES},
      store_(storage_, store_config_), compressed_staging_(nullptr),
      state_mutex_(nullptr), worker_task_(nullptr), stop_requested_(false),
      worker_exited_(true), worker_started_(false), store_initialized_(false),
      requests_released_(false),
      has_location_fix_(false), center_initialized_(false), current_location_{}, dragging_(false),
      last_drag_point_{0, 0}, back_callback_() {
    LVGL_LOCK();
    state_mutex_ = xSemaphoreCreateMutex();

    screen_ = lv_obj_create(parent ? parent : lv_scr_act());
    lv_obj_set_size(screen_, 320, 240);
    lv_obj_set_style_pad_all(screen_, 0, 0);
    lv_obj_set_style_border_width(screen_, 0, 0);
    lv_obj_set_style_radius(screen_, 0, 0);
    lv_obj_set_style_bg_color(screen_, Theme::surface(), 0);
    lv_obj_clear_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);

    toolbar_ = lv_obj_create(screen_);
    lv_obj_set_size(toolbar_, 320, 32);
    lv_obj_align(toolbar_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(toolbar_, 2, 0);
    lv_obj_set_style_pad_gap(toolbar_, 3, 0);
    lv_obj_set_style_border_width(toolbar_, 0, 0);
    lv_obj_set_style_radius(toolbar_, 0, 0);
    lv_obj_set_style_bg_color(toolbar_, Theme::surfaceHeader(), 0);
    lv_obj_set_flex_flow(toolbar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toolbar_, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    back_button_ = createToolbarButton(toolbar_, LV_SYMBOL_LEFT, onBack, this, 36);
    zoom_out_button_ = createToolbarButton(toolbar_, LV_SYMBOL_MINUS,
                                            onZoomOut, this, 36);
    zoom_label_ = lv_label_create(toolbar_);
    lv_obj_set_width(zoom_label_, 62);
    lv_obj_set_style_text_align(zoom_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(zoom_label_, "z2");
    zoom_in_button_ = createToolbarButton(toolbar_, LV_SYMBOL_PLUS,
                                           onZoomIn, this, 36);
    recenter_button_ = createToolbarButton(toolbar_, LV_SYMBOL_GPS,
                                            onRecenter, this, 44);

    viewport_ = lv_obj_create(screen_);
    lv_obj_set_size(viewport_, Pyxis::MapScreenPresenter::VIEWPORT_WIDTH,
                    Pyxis::MapScreenPresenter::VIEWPORT_HEIGHT);
    lv_obj_align(viewport_, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_pad_all(viewport_, 0, 0);
    lv_obj_set_style_border_width(viewport_, 0, 0);
    lv_obj_set_style_radius(viewport_, 0, 0);
    lv_obj_set_style_bg_color(viewport_, lv_color_hex(0x25282b), 0);
    lv_obj_set_style_bg_opa(viewport_, LV_OPA_COVER, 0);
    lv_obj_add_flag(viewport_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(viewport_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(viewport_, onMapPressed, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(viewport_, onMapPressing, LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(viewport_, onMapReleased, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(viewport_, onMapReleased, LV_EVENT_PRESS_LOST, this);

    for (std::size_t index = 0; index < TILE_COUNT; ++index) {
        tile_pixels_[index] = static_cast<lv_color_t*>(heap_caps_malloc(
            TILE_PIXEL_COUNT * sizeof(lv_color_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (tile_pixels_[index]) {
            for (std::size_t pixel = 0; pixel < TILE_PIXEL_COUNT; ++pixel) {
                const bool light = (((pixel % 256U) / 32U) +
                                    ((pixel / 256U) / 32U)) % 2U == 0U;
                tile_pixels_[index][pixel] =
                    lv_color_hex(light ? 0x303438 : 0x272a2e);
            }
        }
        lv_img_dsc_t& descriptor = tile_descriptors_[index];
        std::memset(&descriptor, 0, sizeof(descriptor));
        descriptor.header.always_zero = 0;
        descriptor.header.w = 256;
        descriptor.header.h = 256;
        descriptor.header.cf = LV_IMG_CF_TRUE_COLOR;
        descriptor.data_size = TILE_PIXEL_COUNT * sizeof(lv_color_t);
        descriptor.data = reinterpret_cast<const std::uint8_t*>(
            tile_pixels_[index]);
        tile_images_[index] = lv_img_create(viewport_);
        lv_obj_add_flag(tile_images_[index], LV_OBJ_FLAG_HIDDEN);
    }

    for (std::size_t index = 0; index < MARKER_COUNT; ++index) {
        approximation_halos_[index] = lv_obj_create(viewport_);
        lv_obj_set_style_radius(approximation_halos_[index], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(approximation_halos_[index], Theme::info(), 0);
        lv_obj_set_style_bg_opa(approximation_halos_[index], LV_OPA_20, 0);
        lv_obj_set_style_border_width(approximation_halos_[index], 1, 0);
        lv_obj_set_style_border_color(approximation_halos_[index], Theme::info(), 0);
        lv_obj_set_style_pad_all(approximation_halos_[index], 0, 0);
        lv_obj_clear_flag(approximation_halos_[index], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(approximation_halos_[index], LV_OBJ_FLAG_HIDDEN);
        markers_[index] = lv_obj_create(viewport_);
        lv_obj_set_size(markers_[index], 10, 10);
        lv_obj_set_style_radius(markers_[index], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(markers_[index], 2, 0);
        lv_obj_set_style_border_color(markers_[index], lv_color_white(), 0);
        lv_obj_set_style_pad_all(markers_[index], 0, 0);
        lv_obj_clear_flag(markers_[index], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(markers_[index], LV_OBJ_FLAG_HIDDEN);
        marker_labels_[index] = lv_label_create(viewport_);
        lv_label_set_text(marker_labels_[index], "");
        lv_obj_set_style_text_font(marker_labels_[index],
                                   &lv_font_montserrat_12, 0);
        lv_obj_add_flag(marker_labels_[index], LV_OBJ_FLAG_HIDDEN);
    }

    static const char* pan_symbols[4] = {
        LV_SYMBOL_UP, LV_SYMBOL_DOWN, LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT};
    static const lv_align_t pan_alignments[4] = {
        LV_ALIGN_TOP_MID, LV_ALIGN_BOTTOM_MID,
        LV_ALIGN_LEFT_MID, LV_ALIGN_RIGHT_MID};
    static const lv_coord_t pan_x[4] = {0, 0, 2, -2};
    static const lv_coord_t pan_y[4] = {2, -2, 0, 0};
    for (std::size_t index = 0; index < 4U; ++index) {
        pan_buttons_[index] = createToolbarButton(
            viewport_, pan_symbols[index], onPan, this, 30);
        lv_obj_set_user_data(pan_buttons_[index],
                             reinterpret_cast<void*>(index));
        lv_obj_align(pan_buttons_[index], pan_alignments[index],
                     pan_x[index], pan_y[index]);
        lv_obj_set_style_bg_opa(pan_buttons_[index], LV_OPA_60, 0);
    }

    status_label_ = lv_label_create(viewport_);
    lv_label_set_text(status_label_, "Offline tiles");
    lv_obj_set_style_text_color(status_label_, Theme::textTertiary(), 0);
    lv_obj_set_style_bg_color(status_label_, Theme::surfaceElevated(), 0);
    lv_obj_set_style_bg_opa(status_label_, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(status_label_, 3, 0);
    lv_obj_align(status_label_, LV_ALIGN_BOTTOM_LEFT, 4, -4);

    attribution_label_ = lv_label_create(viewport_);
    lv_label_set_text(attribution_label_, "© OpenStreetMap contributors");
    lv_obj_set_style_text_color(attribution_label_, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(attribution_label_, &lv_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(attribution_label_, Theme::surfaceElevated(), 0);
    lv_obj_set_style_bg_opa(attribution_label_, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(attribution_label_, 2, 0);
    lv_obj_align(attribution_label_, LV_ALIGN_BOTTOM_RIGHT, -3, -3);

    lv_obj_add_flag(screen_, LV_OBJ_FLAG_HIDDEN);
}

MapScreen::~MapScreen() {
    stopWorker();
    LVGL_LOCK();
    if (screen_) lv_obj_del(screen_);
    screen_ = nullptr;
    for (std::size_t index = 0; index < TILE_COUNT; ++index) {
        if (tile_pixels_[index]) heap_caps_free(tile_pixels_[index]);
        tile_pixels_[index] = nullptr;
    }
    if (compressed_staging_) heap_caps_free(compressed_staging_);
    compressed_staging_ = nullptr;
    if (state_mutex_) vSemaphoreDelete(state_mutex_);
    state_mutex_ = nullptr;
}

bool MapScreen::lockState(TickType_t ticks) {
    return state_mutex_ && xSemaphoreTake(state_mutex_, ticks) == pdTRUE;
}

void MapScreen::unlockState() {
    xSemaphoreGive(state_mutex_);
}

bool MapScreen::startWorker() {
    if (worker_started_) return true;
    if (!state_mutex_) return false;
    compressed_staging_ = static_cast<std::uint8_t*>(heap_caps_malloc(
        MAX_COMPRESSED_TILE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!compressed_staging_) return false;
    stop_requested_.store(false, std::memory_order_release);
    worker_exited_.store(false, std::memory_order_release);
    const BaseType_t created = xTaskCreatePinnedToCore(
        workerEntry, "map-tile-worker", 16384, this, 1, &worker_task_, 0);
    if (created != pdPASS) {
        worker_exited_.store(true, std::memory_order_release);
        worker_task_ = nullptr;
        heap_caps_free(compressed_staging_);
        compressed_staging_ = nullptr;
        return false;
    }
    worker_started_ = true;
    return true;
}

void MapScreen::stopWorker() {
    if (!worker_started_) return;
    stop_requested_.store(true, std::memory_order_release);
    if (worker_task_) xTaskNotifyGive(worker_task_);
    while (!worker_exited_.load(std::memory_order_acquire)) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    worker_task_ = nullptr;
    worker_started_ = false;
}

void MapScreen::workerEntry(void* context) {
    static_cast<MapScreen*>(context)->workerLoop();
}

void MapScreen::workerLoop() {
    Hardware::TDeck::TileStoreResult initialized = store_.initialize();
    store_initialized_ = initialized == Hardware::TDeck::TileStoreResult::OK;
    while (!stop_requested_.load(std::memory_order_acquire)) {
        Pyxis::MapTileRequest request{};
        bool have_request = false;
        if (lockState(pdMS_TO_TICKS(20))) {
            if (requests_released_) {
                have_request = presenter_.takeRequest(request);
            }
            unlockState();
        }
        if (!have_request) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        Pyxis::MapTileCompletion completion{};
        completion.generation = request.generation;
        completion.frame_epoch = request.frame_epoch;
        completion.slot_token = request.slot_token;
        completion.slot_index = request.slot_index;
        completion.key = request.key;
        completion.result = loadTile(request);
        if (lockState(portMAX_DELAY)) {
            (void)presenter_.publishCompletion(completion);
            unlockState();
        }
    }
    worker_exited_.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

Pyxis::MapTileLoadResult MapScreen::loadTile(
    const Pyxis::MapTileRequest& request) {
    if (!store_initialized_) {
        return Pyxis::MapTileLoadResult::STORAGE_UNAVAILABLE;
    }
    std::uint32_t size = 0U;
    Hardware::TDeck::TileStoreResult result =
        store_.beginGet(request.key, size);
    if (result == Hardware::TDeck::TileStoreResult::MISS) {
        return Pyxis::MapTileLoadResult::MISS;
    }
    if (result == Hardware::TDeck::TileStoreResult::STORAGE_UNAVAILABLE ||
        result == Hardware::TDeck::TileStoreResult::NOT_INITIALIZED) {
        return Pyxis::MapTileLoadResult::STORAGE_UNAVAILABLE;
    }
    if (result != Hardware::TDeck::TileStoreResult::OK) {
        return Pyxis::MapTileLoadResult::IO_ERROR;
    }
    if (size > MAX_COMPRESSED_TILE_BYTES) {
        store_.endGet();
        return Pyxis::MapTileLoadResult::TOO_LARGE;
    }
    std::size_t total = 0U;
    while (total < size &&
           !stop_requested_.load(std::memory_order_acquire)) {
        const std::size_t capacity =
            (size - total) < READ_CHUNK_BYTES ? (size - total) : READ_CHUNK_BYTES;
        std::size_t count = 0U;
        result = store_.readGetChunk(compressed_staging_ + total,
                                     capacity, count);
        if (result != Hardware::TDeck::TileStoreResult::OK || count == 0U) {
            store_.endGet();
            return result == Hardware::TDeck::TileStoreResult::STORAGE_UNAVAILABLE
                ? Pyxis::MapTileLoadResult::STORAGE_UNAVAILABLE
                : Pyxis::MapTileLoadResult::IO_ERROR;
        }
        total += count;
    }
    store_.endGet();
    if (total != size || stop_requested_.load(std::memory_order_acquire)) {
        return Pyxis::MapTileLoadResult::IO_ERROR;
    }

    unsigned char* rgb = nullptr;
    unsigned width = 0U;
    unsigned height = 0U;
    const unsigned decode_error = lodepng_decode24(
        &rgb, &width, &height, compressed_staging_, total);
    if (decode_error != 0U || rgb == nullptr || width != 256U || height != 256U) {
        if (rgb) lv_mem_free(rgb);
        return Pyxis::MapTileLoadResult::INVALID_PNG;
    }
    if (request.slot_index >= TILE_COUNT || !tile_pixels_[request.slot_index]) {
        lv_mem_free(rgb);
        return Pyxis::MapTileLoadResult::IO_ERROR;
    }
    lv_color_t* pixels = tile_pixels_[request.slot_index];
    for (std::size_t index = 0; index < TILE_PIXEL_COUNT; ++index) {
        const std::size_t source = index * 3U;
        pixels[index] = lv_color_make(rgb[source], rgb[source + 1U],
                                      rgb[source + 2U]);
    }
    lv_mem_free(rgb);
    return Pyxis::MapTileLoadResult::READY;
}

void MapScreen::serviceIo() {
    if (!worker_started_ && !startWorker()) return;
    if (worker_task_) xTaskNotifyGive(worker_task_);
}

void MapScreen::updateModel(const Pyxis::MapView::Request& request) {
    if (!lockState(pdMS_TO_TICKS(100))) return;
    has_location_fix_ = request.has_local_location;
    current_location_ = request.local_location;
    if (!center_initialized_ && request.has_local_location &&
        presenter_.recenter(true, request.local_location)) {
        center_initialized_ = true;
    }
    requests_released_ = false;
    (void)presenter_.buildFrame(request);
    unlockState();
}

void MapScreen::setPlaceholder(std::size_t index) {
    lv_obj_add_flag(tile_images_[index], LV_OBJ_FLAG_HIDDEN);
}

void MapScreen::applyFrame() {
    if (!lockState(pdMS_TO_TICKS(100))) return;
    const Pyxis::MapView::Frame& frame = presenter_.frame();
    for (std::size_t index = 0; index < TILE_COUNT; ++index) {
        const Pyxis::MapTileSlot& slot = presenter_.slot(index);
        if (slot.state == Pyxis::MapTileSlot::READY && tile_pixels_[index]) {
            lv_img_set_src(tile_images_[index], &tile_descriptors_[index]);
            lv_obj_set_pos(tile_images_[index],
                           static_cast<lv_coord_t>(std::lround(slot.screen_x)),
                           static_cast<lv_coord_t>(std::lround(slot.screen_y)));
            lv_obj_clear_flag(tile_images_[index], LV_OBJ_FLAG_HIDDEN);
        } else {
            setPlaceholder(index);
        }
    }
    for (std::size_t index = 0; index < MARKER_COUNT; ++index) {
        if (index >= frame.marker_count) {
            lv_obj_add_flag(approximation_halos_[index], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(markers_[index], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(marker_labels_[index], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const Pyxis::MapView::Marker& marker = frame.markers[index];
        const lv_coord_t x = static_cast<lv_coord_t>(std::lround(marker.screen_x));
        const lv_coord_t y = static_cast<lv_coord_t>(std::lround(marker.screen_y));
        if (marker.has_approx_radius && marker.approx_radius_pixels > 0.0) {
            const double bounded_radius = marker.approx_radius_pixels > 320.0
                ? 320.0 : (marker.approx_radius_pixels < 2.0
                    ? 2.0 : marker.approx_radius_pixels);
            const lv_coord_t radius = static_cast<lv_coord_t>(std::lround(bounded_radius));
            lv_obj_set_size(approximation_halos_[index], radius * 2, radius * 2);
            lv_obj_set_pos(approximation_halos_[index], x - radius, y - radius);
            lv_obj_clear_flag(approximation_halos_[index], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(approximation_halos_[index], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_pos(markers_[index], x - 5, y - 5);
        lv_obj_set_style_bg_color(
            markers_[index],
            marker.kind == Pyxis::MapView::MarkerKind::LOCAL
                ? Theme::info() : Theme::success(), 0);
        lv_obj_clear_flag(markers_[index], LV_OBJ_FLAG_HIDDEN);
        char label[8] = {};
        if (marker.kind == Pyxis::MapView::MarkerKind::LOCAL) {
            std::snprintf(label, sizeof(label), "me");
        } else {
            std::snprintf(label, sizeof(label), "%02x%02x",
                          marker.peer.bytes[Telemetry::PEER_ID_SIZE - 2U],
                          marker.peer.bytes[Telemetry::PEER_ID_SIZE - 1U]);
        }
        lv_label_set_text(marker_labels_[index], label);
        lv_obj_set_pos(marker_labels_[index], x + 6, y - 7);
        lv_obj_clear_flag(marker_labels_[index], LV_OBJ_FLAG_HIDDEN);
    }
    char zoom_text[12] = {};
    std::snprintf(zoom_text, sizeof(zoom_text), "z%lu",
                  static_cast<unsigned long>(presenter_.zoom()));
    lv_label_set_text(zoom_label_, zoom_text);
    // Non-ready images are now detached under LVGL_LOCK, so the worker may
    // safely decode into their permanent buffers without a draw race.
    requests_released_ = true;
    unlockState();
    if (worker_task_) xTaskNotifyGive(worker_task_);
}

void MapScreen::setStatusFor(Pyxis::MapTileLoadResult result) {
    switch (result) {
        case Pyxis::MapTileLoadResult::READY:
            lv_label_set_text(status_label_, "Offline");
            break;
        case Pyxis::MapTileLoadResult::MISS:
            lv_label_set_text(status_label_, "Tile unavailable");
            break;
        case Pyxis::MapTileLoadResult::STORAGE_UNAVAILABLE:
            lv_label_set_text(status_label_, "SD unavailable");
            break;
        case Pyxis::MapTileLoadResult::INVALID_PNG:
            lv_label_set_text(status_label_, "Bad tile");
            break;
        case Pyxis::MapTileLoadResult::TOO_LARGE:
            lv_label_set_text(status_label_, "Tile too large");
            break;
        case Pyxis::MapTileLoadResult::IO_ERROR:
            lv_label_set_text(status_label_, "Tile I/O error");
            break;
    }
}

bool MapScreen::applyOneCompletion() {
    if (!lockState(pdMS_TO_TICKS(100))) return false;
    Pyxis::MapTileCompletion completion{};
    const bool applied = presenter_.takeApplicableCompletion(completion);
    if (applied) {
        setStatusFor(completion.result);
        if (completion.result == Pyxis::MapTileLoadResult::READY &&
            completion.slot_index < TILE_COUNT) {
            const std::size_t index = completion.slot_index;
            lv_img_set_src(tile_images_[index], &tile_descriptors_[index]);
            const Pyxis::MapTileSlot& slot = presenter_.slot(index);
            lv_obj_set_pos(tile_images_[index],
                           static_cast<lv_coord_t>(std::lround(slot.screen_x)),
                           static_cast<lv_coord_t>(std::lround(slot.screen_y)));
            lv_obj_clear_flag(tile_images_[index], LV_OBJ_FLAG_HIDDEN);
        }
    }
    unlockState();
    return applied;
}

void MapScreen::show() {
    if (lockState(pdMS_TO_TICKS(100))) {
        presenter_.show();
        unlockState();
    }
    lv_obj_clear_flag(screen_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(screen_);
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        lv_group_add_obj(group, back_button_);
        lv_group_add_obj(group, zoom_out_button_);
        lv_group_add_obj(group, zoom_in_button_);
        lv_group_add_obj(group, recenter_button_);
        for (std::size_t i = 0; i < 4U; ++i) lv_group_add_obj(group, pan_buttons_[i]);
        lv_group_focus_obj(back_button_);
    }
}

void MapScreen::hide() {
    if (lockState(pdMS_TO_TICKS(100))) {
        presenter_.hide();
        unlockState();
    }
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        lv_group_remove_obj(back_button_);
        lv_group_remove_obj(zoom_out_button_);
        lv_group_remove_obj(zoom_in_button_);
        lv_group_remove_obj(recenter_button_);
        for (std::size_t i = 0; i < 4U; ++i) lv_group_remove_obj(pan_buttons_[i]);
    }
    lv_obj_add_flag(screen_, LV_OBJ_FLAG_HIDDEN);
}

MapScreen* MapScreen::fromEvent(lv_event_t* event) {
    return static_cast<MapScreen*>(lv_event_get_user_data(event));
}

void MapScreen::pan(double dx, double dy) {
    if (!lockState(pdMS_TO_TICKS(100))) return;
    center_initialized_ = true;
    (void)presenter_.panPixels(dx, dy);
    unlockState();
}

void MapScreen::onBack(lv_event_t* event) {
    MapScreen* screen = fromEvent(event);
    if (screen && screen->back_callback_) screen->back_callback_();
}

void MapScreen::onZoomIn(lv_event_t* event) {
    MapScreen* screen = fromEvent(event);
    if (screen && screen->lockState(pdMS_TO_TICKS(100))) {
        screen->center_initialized_ = true;
        (void)screen->presenter_.zoomBy(1);
        screen->unlockState();
    }
}

void MapScreen::onZoomOut(lv_event_t* event) {
    MapScreen* screen = fromEvent(event);
    if (screen && screen->lockState(pdMS_TO_TICKS(100))) {
        screen->center_initialized_ = true;
        (void)screen->presenter_.zoomBy(-1);
        screen->unlockState();
    }
}

void MapScreen::onRecenter(lv_event_t* event) {
    MapScreen* screen = fromEvent(event);
    if (!screen || !screen->lockState(pdMS_TO_TICKS(100))) return;
    const bool centered = screen->presenter_.recenter(
        screen->has_location_fix_, screen->current_location_);
    if (centered) screen->center_initialized_ = true;
    screen->unlockState();
    if (!centered) lv_label_set_text(screen->status_label_, "No GPS fix");
}

void MapScreen::onPan(lv_event_t* event) {
    MapScreen* screen = fromEvent(event);
    lv_obj_t* target = lv_event_get_current_target(event);
    if (!screen || !target) return;
    const std::size_t direction = reinterpret_cast<std::size_t>(
        lv_obj_get_user_data(target));
    switch (direction) {
        case 0U: screen->pan(0.0, -64.0); break;
        case 1U: screen->pan(0.0, 64.0); break;
        case 2U: screen->pan(-64.0, 0.0); break;
        case 3U: screen->pan(64.0, 0.0); break;
        default: break;
    }
}

void MapScreen::onMapPressed(lv_event_t* event) {
    MapScreen* screen = fromEvent(event);
    lv_indev_t* input = lv_indev_get_act();
    if (!screen || !input) return;
    screen->dragging_ = true;
    lv_indev_get_point(input, &screen->last_drag_point_);
}

void MapScreen::onMapPressing(lv_event_t* event) {
    MapScreen* screen = fromEvent(event);
    lv_indev_t* input = lv_indev_get_act();
    if (!screen || !input || !screen->dragging_) return;
    lv_point_t point{};
    lv_indev_get_point(input, &point);
    const double dx = static_cast<double>(screen->last_drag_point_.x - point.x);
    const double dy = static_cast<double>(screen->last_drag_point_.y - point.y);
    screen->last_drag_point_ = point;
    if (dx != 0.0 || dy != 0.0) screen->pan(dx, dy);
}

void MapScreen::onMapReleased(lv_event_t* event) {
    MapScreen* screen = fromEvent(event);
    if (screen) screen->dragging_ = false;
}

}  // namespace LXMF
}  // namespace UI

#endif  // ARDUINO
