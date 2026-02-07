// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "StatusScreen.h"
#include "Theme.h"

#ifdef ARDUINO

#include <WiFi.h>
#include "Log.h"
#include "../LVGL/LVGLInit.h"
#include "../LVGL/LVGLLock.h"

using namespace RNS;

namespace UI {
namespace LXMF {

StatusScreen::StatusScreen(lv_obj_t* parent)
    : _screen(nullptr), _header(nullptr), _content(nullptr), _btn_back(nullptr),
      _btn_share(nullptr), _label_identity_value(nullptr), _label_lxmf_value(nullptr),
      _label_wifi_status(nullptr), _label_wifi_ip(nullptr), _label_wifi_rssi(nullptr),
      _label_rns_status(nullptr), _label_ble_header(nullptr),
      _rns_connected(false), _ble_peer_count(0) {
    // Initialize BLE peer labels array
    for (size_t i = 0; i < MAX_BLE_PEERS; i++) {
        _label_ble_peers[i] = nullptr;
    }

    // Create screen object
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

    // Create UI components
    create_header();
    create_content();

    // Hide by default
    hide();

    TRACE("StatusScreen created");
}

StatusScreen::~StatusScreen() {
    LVGL_LOCK();
    if (_screen) {
        lv_obj_del(_screen);
    }
}

void StatusScreen::create_header() {
    _header = lv_obj_create(_screen);
    lv_obj_set_size(_header, LV_PCT(100), 36);
    lv_obj_align(_header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(_header, Theme::surfaceHeader(), 0);
    lv_obj_set_style_border_width(_header, 0, 0);
    lv_obj_set_style_radius(_header, 0, 0);
    lv_obj_set_style_pad_all(_header, 0, 0);

    // Back button
    _btn_back = lv_btn_create(_header);
    lv_obj_set_size(_btn_back, 50, 28);
    lv_obj_align(_btn_back, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_bg_color(_btn_back, Theme::btnSecondary(), 0);
    lv_obj_set_style_bg_color(_btn_back, Theme::btnSecondaryPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btn_back, on_back_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* label_back = lv_label_create(_btn_back);
    lv_label_set_text(label_back, LV_SYMBOL_LEFT);
    lv_obj_center(label_back);
    lv_obj_set_style_text_color(label_back, Theme::textSecondary(), 0);

    // Title
    lv_obj_t* title = lv_label_create(_header);
    lv_label_set_text(title, "Status");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 60, 0);
    lv_obj_set_style_text_color(title, Theme::textPrimary(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    // Share button (QR code icon) on the right
    _btn_share = lv_btn_create(_header);
    lv_obj_set_size(_btn_share, 60, 28);
    lv_obj_align(_btn_share, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(_btn_share, Theme::primary(), 0);
    lv_obj_set_style_bg_color(_btn_share, Theme::primaryPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btn_share, on_share_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* label_share = lv_label_create(_btn_share);
    lv_label_set_text(label_share, "Share");
    lv_obj_center(label_share);
    lv_obj_set_style_text_color(label_share, Theme::textPrimary(), 0);
}

void StatusScreen::create_content() {
    _content = lv_obj_create(_screen);
    lv_obj_set_size(_content, LV_PCT(100), 204);  // 240 - 36 header
    lv_obj_align(_content, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_pad_all(_content, 8, 0);
    lv_obj_set_style_bg_color(_content, Theme::surface(), 0);
    lv_obj_set_style_border_width(_content, 0, 0);
    lv_obj_set_style_radius(_content, 0, 0);

    // Enable vertical scrolling with flex layout
    lv_obj_set_flex_flow(_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_content, LV_SCROLLBAR_MODE_AUTO);

    // Identity section
    lv_obj_t* label_identity = lv_label_create(_content);
    lv_label_set_text(label_identity, "Identity:");
    lv_obj_set_style_text_color(label_identity, Theme::textMuted(), 0);

    _label_identity_value = lv_label_create(_content);
    lv_label_set_text(_label_identity_value, "Loading...");
    lv_obj_set_style_text_color(_label_identity_value, Theme::info(), 0);
    lv_obj_set_style_text_font(_label_identity_value, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_left(_label_identity_value, 8, 0);
    lv_obj_set_style_pad_bottom(_label_identity_value, 8, 0);

    // LXMF Address section
    lv_obj_t* label_lxmf = lv_label_create(_content);
    lv_label_set_text(label_lxmf, "LXMF Address:");
    lv_obj_set_style_text_color(label_lxmf, Theme::textMuted(), 0);

    _label_lxmf_value = lv_label_create(_content);
    lv_label_set_text(_label_lxmf_value, "Loading...");
    lv_obj_set_style_text_color(_label_lxmf_value, Theme::success(), 0);
    lv_obj_set_style_text_font(_label_lxmf_value, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_left(_label_lxmf_value, 8, 0);
    lv_obj_set_style_pad_bottom(_label_lxmf_value, 8, 0);

    // WiFi section
    _label_wifi_status = lv_label_create(_content);
    lv_label_set_text(_label_wifi_status, "WiFi: Checking...");
    lv_obj_set_style_text_color(_label_wifi_status, Theme::textPrimary(), 0);

    _label_wifi_ip = lv_label_create(_content);
    lv_label_set_text(_label_wifi_ip, "");
    lv_obj_set_style_text_color(_label_wifi_ip, Theme::textTertiary(), 0);
    lv_obj_set_style_pad_left(_label_wifi_ip, 8, 0);

    _label_wifi_rssi = lv_label_create(_content);
    lv_label_set_text(_label_wifi_rssi, "");
    lv_obj_set_style_text_color(_label_wifi_rssi, Theme::textTertiary(), 0);
    lv_obj_set_style_pad_left(_label_wifi_rssi, 8, 0);
    lv_obj_set_style_pad_bottom(_label_wifi_rssi, 8, 0);

    // RNS section
    _label_rns_status = lv_label_create(_content);
    lv_label_set_text(_label_rns_status, "RNS: Checking...");
    lv_obj_set_style_text_color(_label_rns_status, Theme::textPrimary(), 0);
    lv_obj_set_width(_label_rns_status, lv_pct(100));
    lv_label_set_long_mode(_label_rns_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_bottom(_label_rns_status, 8, 0);

    // BLE section header
    _label_ble_header = lv_label_create(_content);
    lv_label_set_text(_label_ble_header, "BLE: No peers");
    lv_obj_set_style_text_color(_label_ble_header, Theme::textPrimary(), 0);

    // Pre-create BLE peer labels (hidden until needed)
    for (size_t i = 0; i < MAX_BLE_PEERS; i++) {
        _label_ble_peers[i] = lv_label_create(_content);
        lv_label_set_text(_label_ble_peers[i], "");
        lv_obj_set_style_text_color(_label_ble_peers[i], Theme::textTertiary(), 0);
        lv_obj_set_style_text_font(_label_ble_peers[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_pad_left(_label_ble_peers[i], 8, 0);
        lv_obj_add_flag(_label_ble_peers[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void StatusScreen::set_identity_hash(const Bytes& hash) {
    LVGL_LOCK();
    _identity_hash = hash;
    update_labels();
}

void StatusScreen::set_lxmf_address(const Bytes& hash) {
    LVGL_LOCK();
    _lxmf_address = hash;
    update_labels();
}

void StatusScreen::set_rns_status(bool connected, const String& server_name) {
    LVGL_LOCK();
    _rns_connected = connected;
    _rns_server = server_name;
    update_labels();
}

void StatusScreen::set_ble_info(const BLEPeerInfo* peers, size_t count) {
    LVGL_LOCK();
    // Copy peer data to internal storage
    _ble_peer_count = (count <= MAX_BLE_PEERS) ? count : MAX_BLE_PEERS;
    for (size_t i = 0; i < _ble_peer_count; i++) {
        memcpy(&_ble_peers[i], &peers[i], sizeof(BLEPeerInfo));
    }
    update_labels();
}

void StatusScreen::refresh() {
    LVGL_LOCK();
    update_labels();
}

void StatusScreen::update_labels() {
    // Update identity - use stack buffer to avoid String fragmentation
    if (_identity_hash.size() > 0) {
        lv_label_set_text(_label_identity_value, _identity_hash.toHex().c_str());
    }

    // Update LXMF address - use stack buffer to avoid String fragmentation
    if (_lxmf_address.size() > 0) {
        lv_label_set_text(_label_lxmf_value, _lxmf_address.toHex().c_str());
    }

    // Update WiFi status - use snprintf to avoid String concatenation
    if (WiFi.status() == WL_CONNECTED) {
        lv_label_set_text(_label_wifi_status, "WiFi: Connected");
        lv_obj_set_style_text_color(_label_wifi_status, Theme::success(), 0);

        char ip_text[32];
        snprintf(ip_text, sizeof(ip_text), "IP: %s", WiFi.localIP().toString().c_str());
        lv_label_set_text(_label_wifi_ip, ip_text);

        char rssi_text[24];
        snprintf(rssi_text, sizeof(rssi_text), "RSSI: %d dBm", WiFi.RSSI());
        lv_label_set_text(_label_wifi_rssi, rssi_text);
    } else {
        lv_label_set_text(_label_wifi_status, "WiFi: Disconnected");
        lv_obj_set_style_text_color(_label_wifi_status, Theme::error(), 0);
        lv_label_set_text(_label_wifi_ip, "");
        lv_label_set_text(_label_wifi_rssi, "");
    }

    // Update RNS status - use snprintf to avoid String concatenation
    if (_rns_connected) {
        char rns_text[80];
        if (_rns_server.length() > 0) {
            snprintf(rns_text, sizeof(rns_text), "RNS: Connected (%s)", _rns_server.c_str());
        } else {
            snprintf(rns_text, sizeof(rns_text), "RNS: Connected");
        }
        lv_label_set_text(_label_rns_status, rns_text);
        lv_obj_set_style_text_color(_label_rns_status, Theme::success(), 0);
    } else {
        lv_label_set_text(_label_rns_status, "RNS: Disconnected");
        lv_obj_set_style_text_color(_label_rns_status, Theme::error(), 0);
    }

    // Update BLE peer info
    if (_ble_peer_count > 0) {
        char ble_header[32];
        snprintf(ble_header, sizeof(ble_header), "BLE: %zu peer%s",
                 _ble_peer_count, _ble_peer_count == 1 ? "" : "s");
        lv_label_set_text(_label_ble_header, ble_header);
        lv_obj_set_style_text_color(_label_ble_header, Theme::success(), 0);

        for (size_t i = 0; i < MAX_BLE_PEERS; i++) {
            if (i < _ble_peer_count) {
                // Format: "a1b2c3d4e5f6  -45 dBm\nAA:BB:CC:DD:EE:FF"
                char peer_text[64];
                if (_ble_peers[i].identity[0] != '\0') {
                    snprintf(peer_text, sizeof(peer_text), "%s  %d dBm\n%s",
                             _ble_peers[i].identity, _ble_peers[i].rssi, _ble_peers[i].mac);
                } else {
                    snprintf(peer_text, sizeof(peer_text), "(no identity)  %d dBm\n%s",
                             _ble_peers[i].rssi, _ble_peers[i].mac);
                }
                lv_label_set_text(_label_ble_peers[i], peer_text);
                lv_obj_clear_flag(_label_ble_peers[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(_label_ble_peers[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        lv_label_set_text(_label_ble_header, "BLE: No peers");
        lv_obj_set_style_text_color(_label_ble_header, Theme::textMuted(), 0);

        // Hide all peer labels
        for (size_t i = 0; i < MAX_BLE_PEERS; i++) {
            lv_obj_add_flag(_label_ble_peers[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void StatusScreen::set_back_callback(BackCallback callback) {
    _back_callback = callback;
}

void StatusScreen::set_share_callback(ShareCallback callback) {
    _share_callback = callback;
}

void StatusScreen::show() {
    LVGL_LOCK();
    refresh();  // Update status when shown
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_screen);

    // Add buttons to focus group for trackball navigation
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        if (_btn_back) {
            lv_group_add_obj(group, _btn_back);
        }
        if (_btn_share) {
            lv_group_add_obj(group, _btn_share);
        }
        lv_group_focus_obj(_btn_back);
    }
}

void StatusScreen::hide() {
    LVGL_LOCK();
    // Remove from focus group when hiding
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        if (_btn_back) {
            lv_group_remove_obj(_btn_back);
        }
        if (_btn_share) {
            lv_group_remove_obj(_btn_share);
        }
    }

    lv_obj_add_flag(_screen, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* StatusScreen::get_object() {
    return _screen;
}

void StatusScreen::on_back_clicked(lv_event_t* event) {
    StatusScreen* screen = (StatusScreen*)lv_event_get_user_data(event);

    if (screen->_back_callback) {
        screen->_back_callback();
    }
}

void StatusScreen::on_share_clicked(lv_event_t* event) {
    StatusScreen* screen = (StatusScreen*)lv_event_get_user_data(event);

    if (screen->_share_callback) {
        screen->_share_callback();
    }
}

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
