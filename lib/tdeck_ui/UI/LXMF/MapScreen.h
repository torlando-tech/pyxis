// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_MAPSCREEN_H
#define UI_LXMF_MAPSCREEN_H

#ifdef ARDUINO
#include <Arduino.h>
#include <lvgl.h>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "Bytes.h"

class TinyGPSPlus;

namespace UI {
namespace LXMF {

/**
 * Map Screen
 *
 * Displays offline OSM tiles from SD card with GPS position overlay
 * and peer location markers from telemetry data.
 *
 * Layout (320x240):
 * +-------------------------------------+
 * | <- Back  z:14  [+] [-]              | 36px header
 * +-------------------------------------+
 * |                                     |
 * |          2x2 tile grid              |
 * |       (512x512 clipped to           | 204px viewport
 * |        320x204 viewport)            |
 * |              [GPS dot]              |
 * +-------------------------------------+
 *
 * Tiles loaded from: S:tiles/{z}/{x}/{y}.png
 */
class MapScreen {
public:
    struct PeerLocation {
        RNS::Bytes peer_hash;
        double lat, lon;
        uint32_t timestamp;
    };

    using BackCallback = std::function<void()>;

    MapScreen(lv_obj_t* parent = nullptr);
    ~MapScreen();

    void set_back_callback(BackCallback callback) { _back_callback = callback; }
    void set_gps(TinyGPSPlus* gps) { _gps = gps; }

    void show();
    void hide();
    lv_obj_t* get_object() { return _screen; }

    /** Call from update() when GPS data changes */
    void update_gps_position();

    /** Update peer markers from telemetry data */
    void update_peer_locations(const PeerLocation* locations, size_t count);

private:
    lv_obj_t* _screen;
    lv_obj_t* _header;
    lv_obj_t* _viewport;
    lv_obj_t* _tile_imgs[4];    // 2x2 grid
    lv_obj_t* _gps_marker;
    lv_obj_t* _zoom_label;

    // Peer marker objects (up to 32)
    static const int MAX_PEER_MARKERS = 32;
    lv_obj_t* _peer_markers[MAX_PEER_MARKERS];
    lv_obj_t* _peer_labels[MAX_PEER_MARKERS];
    int _peer_marker_count;

    TinyGPSPlus* _gps;
    BackCallback _back_callback;

    // Map state
    double _center_lat;
    double _center_lon;
    int _zoom;
    bool _follow_gps;
    bool _has_gps_fix;

    // Currently loaded tile coordinates (to avoid redundant reloads)
    int _loaded_tile_x[4];
    int _loaded_tile_y[4];
    int _loaded_zoom;

    // Viewport dimensions
    static const int VIEWPORT_W = 320;
    static const int VIEWPORT_H = 204;
    static const int TILE_SIZE = 256;

    void create_header();
    void create_viewport();
    void update_tiles();
    void position_gps_marker();
    void position_peer_markers();
    void update_zoom_label();

    // Pan by pixel delta
    void pan(int dx, int dy);

    // Load a tile image into one of the 4 slots
    void load_tile(int slot, int tile_x, int tile_y, int z);

    // Touch drag state
    int _last_touch_x;
    int _last_touch_y;

    // Incremental tile loading — one tile per update cycle to avoid LVGL mutex timeout
    struct PendingTile {
        int slot, z, x, y;
    };
    PendingTile _pending_tiles[4];
    int _pending_count;

    // Async tile downloading
    struct TileRequest {
        int z, x, y;
    };
    QueueHandle_t _download_queue;
    TaskHandle_t _download_task;
    volatile bool _download_complete;

    static void download_task_func(void* param);

    // Event handlers
    static void on_back_clicked(lv_event_t* event);
    static void on_zoom_in_clicked(lv_event_t* event);
    static void on_zoom_out_clicked(lv_event_t* event);
    static void on_key_event(lv_event_t* event);
    static void on_touch_event(lv_event_t* event);
};

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
#endif // UI_LXMF_MAPSCREEN_H
