// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "MapScreen.h"

#ifdef ARDUINO

#include "Theme.h"
#include "TileMath.h"
#include "Log.h"
#include "../LVGL/LVGLInit.h"
#include "../LVGL/LVGLLock.h"
#include <TinyGPSPlus.h>

using namespace RNS;

namespace UI {
namespace LXMF {

MapScreen::MapScreen(lv_obj_t* parent)
    : _screen(nullptr), _header(nullptr), _viewport(nullptr),
      _gps_marker(nullptr), _zoom_label(nullptr),
      _peer_marker_count(0), _gps(nullptr),
      _center_lat(0.0), _center_lon(0.0), _zoom(14),
      _follow_gps(true), _has_gps_fix(false), _loaded_zoom(-1) {

    memset(_tile_imgs, 0, sizeof(_tile_imgs));
    memset(_peer_markers, 0, sizeof(_peer_markers));
    memset(_peer_labels, 0, sizeof(_peer_labels));
    memset(_loaded_tile_x, -1, sizeof(_loaded_tile_x));
    memset(_loaded_tile_y, -1, sizeof(_loaded_tile_y));

    LVGL_LOCK();

    if (parent) {
        _screen = lv_obj_create(parent);
    } else {
        _screen = lv_obj_create(lv_scr_act());
    }

    lv_obj_set_size(_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(_screen, Theme::surface(), 0);
    lv_obj_set_style_bg_opa(_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(_screen, 0, 0);
    lv_obj_set_style_border_width(_screen, 0, 0);
    lv_obj_set_style_radius(_screen, 0, 0);

    create_header();
    create_viewport();

    hide();

    TRACE("MapScreen created");
}

MapScreen::~MapScreen() {
    LVGL_LOCK();
    if (_screen) {
        lv_obj_del(_screen);
    }
}

void MapScreen::create_header() {
    _header = lv_obj_create(_screen);
    lv_obj_set_size(_header, LV_PCT(100), 36);
    lv_obj_align(_header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(_header, Theme::surfaceHeader(), 0);
    lv_obj_set_style_border_width(_header, 0, 0);
    lv_obj_set_style_radius(_header, 0, 0);
    lv_obj_set_style_pad_all(_header, 0, 0);

    // Back button
    lv_obj_t* btn_back = lv_btn_create(_header);
    lv_obj_set_size(btn_back, 50, 28);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(btn_back, Theme::btnSecondary(), 0);
    lv_obj_set_style_bg_color(btn_back, Theme::btnSecondaryPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn_back, on_back_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* label_back = lv_label_create(btn_back);
    lv_label_set_text(label_back, LV_SYMBOL_LEFT);
    lv_obj_center(label_back);
    lv_obj_set_style_text_color(label_back, Theme::textSecondary(), 0);

    // Zoom label
    _zoom_label = lv_label_create(_header);
    lv_label_set_text(_zoom_label, "z:14");
    lv_obj_align(_zoom_label, LV_ALIGN_LEFT_MID, 62, 0);
    lv_obj_set_style_text_color(_zoom_label, Theme::textPrimary(), 0);
    lv_obj_set_style_text_font(_zoom_label, &lv_font_montserrat_14, 0);

    // Zoom in button
    lv_obj_t* btn_zin = lv_btn_create(_header);
    lv_obj_set_size(btn_zin, 40, 28);
    lv_obj_align(btn_zin, LV_ALIGN_RIGHT_MID, -48, 0);
    lv_obj_set_style_bg_color(btn_zin, Theme::btnSecondary(), 0);
    lv_obj_set_style_bg_color(btn_zin, Theme::btnSecondaryPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn_zin, on_zoom_in_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* label_zin = lv_label_create(btn_zin);
    lv_label_set_text(label_zin, "+");
    lv_obj_center(label_zin);
    lv_obj_set_style_text_color(label_zin, Theme::textPrimary(), 0);

    // Zoom out button
    lv_obj_t* btn_zout = lv_btn_create(_header);
    lv_obj_set_size(btn_zout, 40, 28);
    lv_obj_align(btn_zout, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(btn_zout, Theme::btnSecondary(), 0);
    lv_obj_set_style_bg_color(btn_zout, Theme::btnSecondaryPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn_zout, on_zoom_out_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* label_zout = lv_label_create(btn_zout);
    lv_label_set_text(label_zout, "-");
    lv_obj_center(label_zout);
    lv_obj_set_style_text_color(label_zout, Theme::textPrimary(), 0);
}

void MapScreen::create_viewport() {
    _viewport = lv_obj_create(_screen);
    lv_obj_set_size(_viewport, VIEWPORT_W, VIEWPORT_H);
    lv_obj_align(_viewport, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(_viewport, lv_color_hex(0x1a1a2e), 0);  // Dark blue for missing tiles
    lv_obj_set_style_border_width(_viewport, 0, 0);
    lv_obj_set_style_radius(_viewport, 0, 0);
    lv_obj_set_style_pad_all(_viewport, 0, 0);
    lv_obj_clear_flag(_viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(_viewport, true, 0);

    // Create 4 tile image objects (2x2 grid)
    for (int i = 0; i < 4; i++) {
        _tile_imgs[i] = lv_img_create(_viewport);
        lv_obj_set_size(_tile_imgs[i], TILE_SIZE, TILE_SIZE);
        // Position will be set in update_tiles()
    }

    // GPS marker: colored circle on top of tiles
    _gps_marker = lv_obj_create(_viewport);
    lv_obj_set_size(_gps_marker, 12, 12);
    lv_obj_set_style_radius(_gps_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(_gps_marker, Theme::primary(), 0);
    lv_obj_set_style_bg_opa(_gps_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_gps_marker, 2, 0);
    lv_obj_set_style_border_color(_gps_marker, lv_color_white(), 0);
    lv_obj_set_style_pad_all(_gps_marker, 0, 0);
    lv_obj_add_flag(_gps_marker, LV_OBJ_FLAG_HIDDEN);

    // Register key event on viewport for pan/zoom
    lv_obj_add_flag(_viewport, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_viewport, on_key_event, LV_EVENT_KEY, this);
}

void MapScreen::show() {
    LVGL_LOCK();
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_screen);

    // Register viewport in focus group for key events
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        lv_group_add_obj(group, _viewport);
        lv_group_focus_obj(_viewport);
    }

    // If we have GPS fix and follow mode, center on GPS
    if (_follow_gps && _gps && _gps->location.isValid()) {
        _center_lat = _gps->location.lat();
        _center_lon = _gps->location.lng();
        _has_gps_fix = true;
    }

    update_tiles();
    position_gps_marker();
    position_peer_markers();
}

void MapScreen::hide() {
    LVGL_LOCK();
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        lv_group_remove_obj(_viewport);
    }
    lv_obj_add_flag(_screen, LV_OBJ_FLAG_HIDDEN);
}

void MapScreen::update_gps_position() {
    if (!_gps) return;

    if (_gps->location.isValid()) {
        _has_gps_fix = true;
        if (_follow_gps) {
            _center_lat = _gps->location.lat();
            _center_lon = _gps->location.lng();
            update_tiles();
        }
        position_gps_marker();
    }
}

void MapScreen::update_peer_locations(const PeerLocation* locations, size_t count) {
    LVGL_LOCK();

    // Clean up old markers
    for (int i = 0; i < _peer_marker_count; i++) {
        if (_peer_markers[i]) lv_obj_del(_peer_markers[i]);
        if (_peer_labels[i]) lv_obj_del(_peer_labels[i]);
        _peer_markers[i] = nullptr;
        _peer_labels[i] = nullptr;
    }

    _peer_marker_count = (count > MAX_PEER_MARKERS) ? MAX_PEER_MARKERS : (int)count;

    for (int i = 0; i < _peer_marker_count; i++) {
        // Create marker dot
        _peer_markers[i] = lv_obj_create(_viewport);
        lv_obj_set_size(_peer_markers[i], 10, 10);
        lv_obj_set_style_radius(_peer_markers[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(_peer_markers[i], Theme::success(), 0);
        lv_obj_set_style_bg_opa(_peer_markers[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(_peer_markers[i], 1, 0);
        lv_obj_set_style_border_color(_peer_markers[i], lv_color_white(), 0);
        lv_obj_set_style_pad_all(_peer_markers[i], 0, 0);

        // Create name label
        _peer_labels[i] = lv_label_create(_viewport);
        char name[12];
        snprintf(name, sizeof(name), "%.6s", locations[i].peer_hash.toHex().c_str());
        lv_label_set_text(_peer_labels[i], name);
        lv_obj_set_style_text_color(_peer_labels[i], Theme::textPrimary(), 0);
        lv_obj_set_style_text_font(_peer_labels[i], &lv_font_montserrat_12, 0);
    }

    position_peer_markers();
}

void MapScreen::update_tiles() {
    // Convert center lat/lon to global pixel coordinates
    double center_px, center_py;
    TileMath::latlon_to_pixel(_center_lat, _center_lon, _zoom, center_px, center_py);

    // Top-left corner of viewport in global pixels
    double vp_left = center_px - VIEWPORT_W / 2.0;
    double vp_top = center_py - VIEWPORT_H / 2.0;

    // Which tile contains the top-left corner
    int base_tile_x = (int)floor(vp_left / TILE_SIZE);
    int base_tile_y = (int)floor(vp_top / TILE_SIZE);

    // Pixel offset of base tile relative to viewport
    int offset_x = (int)(base_tile_x * TILE_SIZE - vp_left);
    int offset_y = (int)(base_tile_y * TILE_SIZE - vp_top);

    // Position the 2x2 tile grid
    // Slot layout: [0]=top-left, [1]=top-right, [2]=bottom-left, [3]=bottom-right
    int tile_coords[4][2] = {
        {base_tile_x,     base_tile_y},
        {base_tile_x + 1, base_tile_y},
        {base_tile_x,     base_tile_y + 1},
        {base_tile_x + 1, base_tile_y + 1}
    };

    for (int i = 0; i < 4; i++) {
        int col = i % 2;
        int row = i / 2;
        int px = offset_x + col * TILE_SIZE;
        int py = offset_y + row * TILE_SIZE;
        lv_obj_set_pos(_tile_imgs[i], px, py);

        // Only reload if tile changed
        if (_loaded_zoom != _zoom ||
            _loaded_tile_x[i] != tile_coords[i][0] ||
            _loaded_tile_y[i] != tile_coords[i][1]) {
            load_tile(i, tile_coords[i][0], tile_coords[i][1], _zoom);
            _loaded_tile_x[i] = tile_coords[i][0];
            _loaded_tile_y[i] = tile_coords[i][1];
        }
    }
    _loaded_zoom = _zoom;
}

void MapScreen::position_gps_marker() {
    if (!_gps || !_gps->location.isValid()) {
        lv_obj_add_flag(_gps_marker, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    double gps_px, gps_py;
    TileMath::latlon_to_pixel(_gps->location.lat(), _gps->location.lng(), _zoom, gps_px, gps_py);

    double center_px, center_py;
    TileMath::latlon_to_pixel(_center_lat, _center_lon, _zoom, center_px, center_py);

    // Position relative to viewport center
    int screen_x = (int)(gps_px - center_px) + VIEWPORT_W / 2 - 6;  // -6 for marker center
    int screen_y = (int)(gps_py - center_py) + VIEWPORT_H / 2 - 6;

    // Only show if within viewport
    if (screen_x >= -12 && screen_x <= VIEWPORT_W &&
        screen_y >= -12 && screen_y <= VIEWPORT_H) {
        lv_obj_set_pos(_gps_marker, screen_x, screen_y);
        lv_obj_clear_flag(_gps_marker, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(_gps_marker);
    } else {
        lv_obj_add_flag(_gps_marker, LV_OBJ_FLAG_HIDDEN);
    }
}

void MapScreen::position_peer_markers() {
    // This will be called with stored peer location data
    // For now peer positions are set via update_peer_locations
    // which creates the markers. Here we just reposition based on
    // current center/zoom if the map has moved.

    // Peer positions are stored externally; this method repositions existing markers.
    // Actual position update requires the PeerLocation data which is stored
    // in TelemetryManager (will be wired in Phase 5).
}

void MapScreen::update_zoom_label() {
    char buf[8];
    snprintf(buf, sizeof(buf), "z:%d", _zoom);
    lv_label_set_text(_zoom_label, buf);
}

void MapScreen::pan(int dx, int dy) {
    _follow_gps = false;  // Manual pan disables follow mode

    double center_px, center_py;
    TileMath::latlon_to_pixel(_center_lat, _center_lon, _zoom, center_px, center_py);

    center_px += dx;
    center_py += dy;

    TileMath::pixel_to_latlon(center_px, center_py, _zoom, _center_lat, _center_lon);

    update_tiles();
    position_gps_marker();
    position_peer_markers();
}

void MapScreen::load_tile(int slot, int tile_x, int tile_y, int z) {
    // Build tile path: "S:tiles/{z}/{x}/{y}.png"
    char path[64];
    snprintf(path, sizeof(path), "S:tiles/%d/%d/%d.png", z, tile_x, tile_y);

    lv_img_set_src(_tile_imgs[slot], path);
}

void MapScreen::on_back_clicked(lv_event_t* event) {
    MapScreen* screen = (MapScreen*)lv_event_get_user_data(event);
    if (screen->_back_callback) {
        screen->_back_callback();
    }
}

void MapScreen::on_zoom_in_clicked(lv_event_t* event) {
    MapScreen* screen = (MapScreen*)lv_event_get_user_data(event);
    if (screen->_zoom < 19) {
        screen->_zoom++;
        screen->update_zoom_label();
        // Invalidate loaded tiles to force reload
        screen->_loaded_zoom = -1;
        screen->update_tiles();
        screen->position_gps_marker();
        screen->position_peer_markers();
    }
}

void MapScreen::on_zoom_out_clicked(lv_event_t* event) {
    MapScreen* screen = (MapScreen*)lv_event_get_user_data(event);
    if (screen->_zoom > 1) {
        screen->_zoom--;
        screen->update_zoom_label();
        screen->_loaded_zoom = -1;
        screen->update_tiles();
        screen->position_gps_marker();
        screen->position_peer_markers();
    }
}

void MapScreen::on_key_event(lv_event_t* event) {
    MapScreen* screen = (MapScreen*)lv_event_get_user_data(event);
    uint32_t key = lv_event_get_key(event);

    const int PAN_STEP = 32;  // pixels per key press

    switch (key) {
        case LV_KEY_UP:
            screen->pan(0, -PAN_STEP);
            break;
        case LV_KEY_DOWN:
            screen->pan(0, PAN_STEP);
            break;
        case LV_KEY_LEFT:
            screen->pan(-PAN_STEP, 0);
            break;
        case LV_KEY_RIGHT:
            screen->pan(PAN_STEP, 0);
            break;
        case '+':
        case '=':
            if (screen->_zoom < 19) {
                screen->_zoom++;
                screen->update_zoom_label();
                screen->_loaded_zoom = -1;
                screen->update_tiles();
                screen->position_gps_marker();
                screen->position_peer_markers();
            }
            break;
        case '-':
            if (screen->_zoom > 1) {
                screen->_zoom--;
                screen->update_zoom_label();
                screen->_loaded_zoom = -1;
                screen->update_tiles();
                screen->position_gps_marker();
                screen->position_peer_markers();
            }
            break;
        case 'c':
        case 'C':
            // Re-center on GPS
            screen->_follow_gps = true;
            if (screen->_gps && screen->_gps->location.isValid()) {
                screen->_center_lat = screen->_gps->location.lat();
                screen->_center_lon = screen->_gps->location.lng();
                screen->update_tiles();
                screen->position_gps_marker();
                screen->position_peer_markers();
            }
            break;
        default:
            break;
    }
}

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
