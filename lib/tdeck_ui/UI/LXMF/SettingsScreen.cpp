// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "SettingsScreen.h"
#include "Theme.h"

#ifdef ARDUINO

#include <WiFi.h>
#include "../LVGL/LVGLLock.h"
#include <microReticulum/Log.h>
#include "../LVGL/LVGLInit.h"
#include "../TextAreaHelper.h"

using namespace RNS;

namespace UI {
namespace LXMF {

// NVS keys
static const char* NVS_NAMESPACE = "settings";
static const char* KEY_WIFI_SSID = "wifi_ssid";
static const char* KEY_WIFI_PASS = "wifi_pass";
static const char* KEY_TCP_HOST = "tcp_host";
static const char* KEY_TCP_PORT = "tcp_port";
static const char* KEY_DISPLAY_NAME = "disp_name";
static const char* KEY_BRIGHTNESS = "brightness";
static const char* KEY_KB_LIGHT = "kb_light";
static const char* KEY_TIMEOUT = "timeout";
static const char* KEY_ANNOUNCE_INT = "announce";
static const char* KEY_SYNC_INT = "sync_int";
static const char* KEY_GPS_SYNC = "gps_sync";
static const char* KEY_TRANSPORT_ENABLED = "transport";
// Notification settings
static const char* KEY_NOTIF_SND = "notif_snd";
static const char* KEY_NOTIF_VOL = "notif_vol";
// Interface settings
static const char* KEY_TCP_ENABLED = "tcp_en";
static const char* KEY_LORA_ENABLED = "lora_en";
static const char* KEY_LORA_FREQ = "lora_freq";
static const char* KEY_LORA_BW = "lora_bw";
static const char* KEY_LORA_SF = "lora_sf";
static const char* KEY_LORA_CR = "lora_cr";
static const char* KEY_LORA_POWER = "lora_pwr";
static const char* KEY_AUTO_ENABLED = "auto_en";
static const char* KEY_BLE_ENABLED = "ble_en";
// Propagation settings
static const char* KEY_PROP_AUTO = "prop_auto";
static const char* KEY_PROP_NODE = "prop_node";
static const char* KEY_PROP_FALLBACK = "prop_fall";
static const char* KEY_PROP_ONLY = "prop_only";

SettingsScreen::SettingsScreen(lv_obj_t* parent)
    : _screen(nullptr), _header(nullptr), _content(nullptr),
      _btn_back(nullptr), _btn_save(nullptr),
      _ta_wifi_ssid(nullptr), _ta_wifi_password(nullptr),
      _ta_tcp_host(nullptr), _ta_tcp_port(nullptr), _btn_reconnect(nullptr),
      _ta_display_name(nullptr),
      _slider_brightness(nullptr), _label_brightness_value(nullptr), _switch_kb_light(nullptr), _dropdown_timeout(nullptr),
      _btn_status(nullptr),
      _switch_tcp_enabled(nullptr), _switch_lora_enabled(nullptr),
      _ta_lora_frequency(nullptr), _dropdown_lora_bandwidth(nullptr),
      _dropdown_lora_sf(nullptr), _dropdown_lora_cr(nullptr),
      _slider_lora_power(nullptr), _label_lora_power_value(nullptr),
      _lora_params_container(nullptr), _switch_auto_enabled(nullptr), _switch_ble_enabled(nullptr),
      _ta_announce_interval(nullptr), _ta_sync_interval(nullptr), _switch_gps_sync(nullptr),
      _switch_transport_enabled(nullptr), _transport_warning_modal(nullptr),
      _transport_modal_group(nullptr), _transport_enable_confirmed(false),
      _btn_propagation_nodes(nullptr), _switch_prop_fallback(nullptr), _switch_prop_only(nullptr),
      _save_state(0U), _apply_retry_at_ms(0U) {
    LVGL_LOCK();

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

    // Load settings from NVS
    load_settings();

    // Create UI components
    create_header();
    create_content();

    // Update UI with loaded settings
    update_ui_from_settings();

    // Hide by default
    hide();

    TRACE("SettingsScreen created");
}

SettingsScreen::~SettingsScreen() {
    LVGL_LOCK();
    if (_screen) {
        lv_obj_del(_screen);
    }
}

void SettingsScreen::create_header() {
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
    lv_label_set_text(title, "Settings");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 60, 0);
    lv_obj_set_style_text_color(title, Theme::textPrimary(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    // Save button
    _btn_save = lv_btn_create(_header);
    lv_obj_set_size(_btn_save, 55, 28);
    lv_obj_align(_btn_save, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(_btn_save, Theme::primary(), 0);
    lv_obj_set_style_bg_color(_btn_save, Theme::primaryPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btn_save, on_save_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* label_save = lv_label_create(_btn_save);
    lv_label_set_text(label_save, "Save");
    lv_obj_center(label_save);
    lv_obj_set_style_text_color(label_save, Theme::textPrimary(), 0);
}

void SettingsScreen::create_content() {
    _content = lv_obj_create(_screen);
    lv_obj_set_size(_content, LV_PCT(100), 204);  // 240 - 36 header
    lv_obj_align(_content, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_pad_all(_content, 4, 0);
    lv_obj_set_style_pad_gap(_content, 2, 0);
    lv_obj_set_style_bg_color(_content, Theme::surface(), 0);
    lv_obj_set_style_border_width(_content, 0, 0);
    lv_obj_set_style_radius(_content, 0, 0);

    // Enable vertical scrolling with flex layout
    lv_obj_set_flex_flow(_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_content, LV_SCROLLBAR_MODE_AUTO);

    // Create sections. Order is by frequency of use: the everyday controls
    // come first, the rare/advanced ones last. Live status readouts no longer
    // live here — the Status link at the top opens the Status screen (Route::STATUS).
    create_general_section(_content);
    create_notifications_section(_content);
    create_network_section(_content);
    create_radio_section(_content);
    create_delivery_section(_content);
    create_advanced_section(_content);
    // This dangerous opt-in must remain the final Settings section.
    create_transport_mode_section(_content);
}

lv_obj_t* SettingsScreen::create_section_header(lv_obj_t* parent, const char* title) {
    lv_obj_t* header = lv_label_create(parent);
    lv_label_set_text(header, title);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_style_text_color(header, Theme::info(), 0);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_top(header, 6, 0);
    lv_obj_set_style_pad_bottom(header, 2, 0);
    return header;
}

lv_obj_t* SettingsScreen::create_label_row(lv_obj_t* parent, const char* text) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_color(label, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    return label;
}

lv_obj_t* SettingsScreen::create_text_input(lv_obj_t* parent, const char* placeholder,
                                             bool password, int max_len) {
    lv_obj_t* ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_height(ta, 28);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    if (password) {
        lv_textarea_set_password_mode(ta, true);
    }
    lv_obj_set_style_bg_color(ta, Theme::surfaceInput(), 0);
    lv_obj_set_style_text_color(ta, Theme::textPrimary(), 0);
    lv_obj_set_style_border_color(ta, Theme::border(), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, 4, 0);
    lv_obj_set_style_pad_all(ta, 4, 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    // Add to input group for keyboard navigation
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        lv_group_add_obj(group, ta);
    }
    // Enable paste on long-press
    TextAreaHelper::enable_paste(ta);
    return ta;
}

void SettingsScreen::create_network_section(lv_obj_t* parent) {
    create_section_header(parent, "== Network ==");

    create_label_row(parent, "WiFi SSID:");
    _ta_wifi_ssid = create_text_input(parent, "Enter SSID", false, 32);

    create_label_row(parent, "WiFi Password:");
    _ta_wifi_password = create_text_input(parent, "Enter password", true, 64);

    create_label_row(parent, "TCP Server:");
    _ta_tcp_host = create_text_input(parent, "IP or hostname", false, 64);

    // Port and reconnect row
    lv_obj_t* port_row = lv_obj_create(parent);
    lv_obj_set_width(port_row, LV_PCT(100));
    lv_obj_set_height(port_row, 32);
    lv_obj_set_style_bg_opa(port_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(port_row, 0, 0);
    lv_obj_set_style_pad_all(port_row, 0, 0);
    lv_obj_clear_flag(port_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* port_label = lv_label_create(port_row);
    lv_label_set_text(port_label, "Port:");
    lv_obj_align(port_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(port_label, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(port_label, &lv_font_montserrat_14, 0);

    _ta_tcp_port = lv_textarea_create(port_row);
    lv_obj_set_size(_ta_tcp_port, 60, 26);
    lv_obj_align(_ta_tcp_port, LV_ALIGN_LEFT_MID, 35, 0);
    lv_textarea_set_one_line(_ta_tcp_port, true);
    lv_textarea_set_max_length(_ta_tcp_port, 5);
    lv_textarea_set_accepted_chars(_ta_tcp_port, "0123456789");
    lv_obj_set_style_bg_color(_ta_tcp_port, Theme::surfaceInput(), 0);
    lv_obj_set_style_text_color(_ta_tcp_port, Theme::textPrimary(), 0);
    lv_obj_set_style_border_color(_ta_tcp_port, Theme::border(), 0);
    lv_obj_set_style_border_width(_ta_tcp_port, 1, 0);
    lv_obj_set_style_radius(_ta_tcp_port, 4, 0);
    lv_obj_set_style_pad_all(_ta_tcp_port, 4, 0);
    lv_obj_set_style_text_font(_ta_tcp_port, &lv_font_montserrat_14, 0);
    // Add to input group for keyboard navigation
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        lv_group_add_obj(group, _ta_tcp_port);
    }
    // Enable paste on long-press
    TextAreaHelper::enable_paste(_ta_tcp_port);

    _btn_reconnect = lv_btn_create(port_row);
    lv_obj_set_size(_btn_reconnect, 80, 26);
    lv_obj_align(_btn_reconnect, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(_btn_reconnect, Theme::primary(), 0);
    lv_obj_set_style_bg_color(_btn_reconnect, Theme::primaryPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btn_reconnect, on_reconnect_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* label_reconnect = lv_label_create(_btn_reconnect);
    lv_label_set_text(label_reconnect, "Reconnect");
    lv_obj_center(label_reconnect);
    lv_obj_set_style_text_color(label_reconnect, Theme::textPrimary(), 0);
    lv_obj_set_style_text_font(label_reconnect, &lv_font_montserrat_14, 0);
    // Auto Discovery row
    lv_obj_t* auto_row = lv_obj_create(parent);
    lv_obj_set_width(auto_row, LV_PCT(100));
    lv_obj_set_height(auto_row, 28);
    lv_obj_set_style_bg_opa(auto_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(auto_row, 0, 0);
    lv_obj_set_style_pad_all(auto_row, 0, 0);
    lv_obj_clear_flag(auto_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* auto_label = lv_label_create(auto_row);
    lv_label_set_text(auto_label, "Auto Discovery:");
    lv_obj_align(auto_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(auto_label, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(auto_label, &lv_font_montserrat_14, 0);

    _switch_auto_enabled = lv_switch_create(auto_row);
    lv_obj_set_size(_switch_auto_enabled, 40, 20);
    lv_obj_align(_switch_auto_enabled, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(_switch_auto_enabled, Theme::border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_switch_auto_enabled, Theme::primary(), LV_PART_INDICATOR | LV_STATE_CHECKED);

    // BLE P2P row
    lv_obj_t* ble_row = lv_obj_create(parent);
    lv_obj_set_width(ble_row, LV_PCT(100));
    lv_obj_set_height(ble_row, 28);
    lv_obj_set_style_bg_opa(ble_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ble_row, 0, 0);
    lv_obj_set_style_pad_all(ble_row, 0, 0);
    lv_obj_clear_flag(ble_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* ble_label = lv_label_create(ble_row);
    lv_label_set_text(ble_label, "BLE P2P:");
    lv_obj_align(ble_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(ble_label, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(ble_label, &lv_font_montserrat_14, 0);

    _switch_ble_enabled = lv_switch_create(ble_row);
    lv_obj_set_size(_switch_ble_enabled, 40, 20);
    lv_obj_align(_switch_ble_enabled, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(_switch_ble_enabled, Theme::border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_switch_ble_enabled, Theme::primary(), LV_PART_INDICATOR | LV_STATE_CHECKED);

    // TCP Enable row
    lv_obj_t* tcp_row = lv_obj_create(parent);
    lv_obj_set_width(tcp_row, LV_PCT(100));
    lv_obj_set_height(tcp_row, 28);
    lv_obj_set_style_bg_opa(tcp_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tcp_row, 0, 0);
    lv_obj_set_style_pad_all(tcp_row, 0, 0);
    lv_obj_clear_flag(tcp_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* tcp_label = lv_label_create(tcp_row);
    lv_label_set_text(tcp_label, "TCP Interface:");
    lv_obj_align(tcp_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(tcp_label, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(tcp_label, &lv_font_montserrat_14, 0);

    _switch_tcp_enabled = lv_switch_create(tcp_row);
    lv_obj_set_size(_switch_tcp_enabled, 40, 20);
    lv_obj_align(_switch_tcp_enabled, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(_switch_tcp_enabled, Theme::border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_switch_tcp_enabled, Theme::primary(), LV_PART_INDICATOR | LV_STATE_CHECKED);


}

void SettingsScreen::create_general_section(lv_obj_t* parent) {
    // Status link: live readouts (GPS fix, system storage/RAM, network state)
    // moved to the Status screen (Route::STATUS). Settings stays actionable-only,
    // which also removes the per-second refresh work from this scroll view.
    _btn_status = lv_btn_create(parent);
    lv_obj_set_width(_btn_status, LV_PCT(100));
    lv_obj_set_height(_btn_status, 30);
    lv_obj_set_style_bg_color(_btn_status, Theme::surfaceInput(), 0);
    lv_obj_set_style_bg_color(_btn_status, Theme::surfaceElevated(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btn_status, on_status_clicked, LV_EVENT_CLICKED, this);
    lv_obj_t* status_label = lv_label_create(_btn_status);
    lv_label_set_text(status_label, "Status  " LV_SYMBOL_RIGHT);
    lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(status_label, Theme::textPrimary(), 0);
    {
        lv_group_t* g = LVGL::LVGLInit::get_default_group();
        if (g) lv_group_add_obj(g, _btn_status);
    }

    create_section_header(parent, "== General ==");

    create_label_row(parent, "Display Name:");
    _ta_display_name = create_text_input(parent, "Your name", false, 32);

    // Brightness row
    lv_obj_t* brightness_row = lv_obj_create(parent);
    lv_obj_set_width(brightness_row, LV_PCT(100));
    lv_obj_set_height(brightness_row, 28);
    lv_obj_set_style_bg_opa(brightness_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(brightness_row, 0, 0);
    lv_obj_set_style_pad_all(brightness_row, 0, 0);
    lv_obj_clear_flag(brightness_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* bright_label = lv_label_create(brightness_row);
    lv_label_set_text(bright_label, "Brightness:");
    lv_obj_align(bright_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(bright_label, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(bright_label, &lv_font_montserrat_14, 0);

    _slider_brightness = lv_slider_create(brightness_row);
    lv_obj_set_size(_slider_brightness, 120, 10);
    lv_obj_align(_slider_brightness, LV_ALIGN_LEFT_MID, 95, 0);
    lv_slider_set_range(_slider_brightness, 10, 255);
    lv_obj_set_style_bg_color(_slider_brightness, Theme::border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_slider_brightness, Theme::info(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_slider_brightness, Theme::textPrimary(), LV_PART_KNOB);
    lv_obj_add_event_cb(_slider_brightness, on_brightness_changed, LV_EVENT_VALUE_CHANGED, this);

    _label_brightness_value = lv_label_create(brightness_row);
    lv_label_set_text(_label_brightness_value, "180");
    lv_obj_align(_label_brightness_value, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_color(_label_brightness_value, Theme::textPrimary(), 0);
    lv_obj_set_style_text_font(_label_brightness_value, &lv_font_montserrat_14, 0);

    // Keyboard light row
    lv_obj_t* kb_light_row = lv_obj_create(parent);
    lv_obj_set_width(kb_light_row, LV_PCT(100));
    lv_obj_set_height(kb_light_row, 28);
    lv_obj_set_style_bg_opa(kb_light_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(kb_light_row, 0, 0);
    lv_obj_set_style_pad_all(kb_light_row, 0, 0);
    lv_obj_clear_flag(kb_light_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* kb_light_label = lv_label_create(kb_light_row);
    lv_label_set_text(kb_light_label, "Keyboard Light:");
    lv_obj_align(kb_light_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(kb_light_label, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(kb_light_label, &lv_font_montserrat_14, 0);

    _switch_kb_light = lv_switch_create(kb_light_row);
    lv_obj_set_size(_switch_kb_light, 40, 20);
    lv_obj_align(_switch_kb_light, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(_switch_kb_light, Theme::border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_switch_kb_light, Theme::primary(), LV_PART_INDICATOR | LV_STATE_CHECKED);

    // Timeout row
    lv_obj_t* timeout_row = lv_obj_create(parent);
    lv_obj_set_width(timeout_row, LV_PCT(100));
    lv_obj_set_height(timeout_row, 28);
    lv_obj_set_style_bg_opa(timeout_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(timeout_row, 0, 0);
    lv_obj_set_style_pad_all(timeout_row, 0, 0);
    lv_obj_clear_flag(timeout_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* timeout_label = lv_label_create(timeout_row);
    lv_label_set_text(timeout_label, "Screen Timeout:");
    lv_obj_align(timeout_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(timeout_label, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(timeout_label, &lv_font_montserrat_14, 0);

    _dropdown_timeout = lv_dropdown_create(timeout_row);
    lv_dropdown_set_options(_dropdown_timeout, "30 sec\n1 min\n5 min\nNever");
    lv_obj_set_size(_dropdown_timeout, 90, 28);
    lv_obj_align(_dropdown_timeout, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(_dropdown_timeout, Theme::surfaceInput(), 0);
    lv_obj_set_style_text_color(_dropdown_timeout, Theme::textPrimary(), 0);
    lv_obj_set_style_border_color(_dropdown_timeout, Theme::border(), 0);
    lv_obj_set_style_text_font(_dropdown_timeout, &lv_font_montserrat_14, 0);
}

void SettingsScreen::create_notifications_section(lv_obj_t* parent) {
    create_section_header(parent, "== Notifications ==");

    // Sound enabled row
    lv_obj_t* sound_row = lv_obj_create(parent);
    lv_obj_set_width(sound_row, LV_PCT(100));
    lv_obj_set_height(sound_row, 28);
    lv_obj_set_style_bg_opa(sound_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sound_row, 0, 0);
    lv_obj_set_style_pad_all(sound_row, 0, 0);
    lv_obj_clear_flag(sound_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* sound_label = lv_label_create(sound_row);
    lv_label_set_text(sound_label, "Message Sound:");
    lv_obj_align(sound_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(sound_label, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(sound_label, &lv_font_montserrat_14, 0);

    _switch_notification_sound = lv_switch_create(sound_row);
    lv_obj_set_size(_switch_notification_sound, 40, 20);
    lv_obj_align(_switch_notification_sound, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(_switch_notification_sound, Theme::border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_switch_notification_sound, Theme::primary(), LV_PART_INDICATOR | LV_STATE_CHECKED);

    // Volume row
    lv_obj_t* volume_row = lv_obj_create(parent);
    lv_obj_set_width(volume_row, LV_PCT(100));
    lv_obj_set_height(volume_row, 28);
    lv_obj_set_style_bg_opa(volume_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(volume_row, 0, 0);
    lv_obj_set_style_pad_all(volume_row, 0, 0);
    lv_obj_clear_flag(volume_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* volume_label = lv_label_create(volume_row);
    lv_label_set_text(volume_label, "Volume:");
    lv_obj_align(volume_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(volume_label, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(volume_label, &lv_font_montserrat_14, 0);

    _slider_notification_volume = lv_slider_create(volume_row);
    lv_obj_set_size(_slider_notification_volume, 120, 10);
    lv_obj_align(_slider_notification_volume, LV_ALIGN_LEFT_MID, 65, 0);
    lv_slider_set_range(_slider_notification_volume, 0, 100);
    lv_obj_set_style_bg_color(_slider_notification_volume, Theme::border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_slider_notification_volume, Theme::info(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_slider_notification_volume, Theme::textPrimary(), LV_PART_KNOB);
    lv_obj_add_event_cb(_slider_notification_volume, on_notification_volume_changed, LV_EVENT_VALUE_CHANGED, this);

    _label_notification_volume_value = lv_label_create(volume_row);
    lv_label_set_text(_label_notification_volume_value, "50");
    lv_obj_align(_label_notification_volume_value, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_color(_label_notification_volume_value, Theme::textPrimary(), 0);
    lv_obj_set_style_text_font(_label_notification_volume_value, &lv_font_montserrat_14, 0);
}

void SettingsScreen::create_radio_section(lv_obj_t* parent) {
    create_section_header(parent, "== Radio ==");

    // LoRa Enable row
    lv_obj_t* lora_row = lv_obj_create(parent);
    lv_obj_set_width(lora_row, LV_PCT(100));
    lv_obj_set_height(lora_row, 28);
    lv_obj_set_style_bg_opa(lora_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lora_row, 0, 0);
    lv_obj_set_style_pad_all(lora_row, 0, 0);
    lv_obj_clear_flag(lora_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lora_label = lv_label_create(lora_row);
    lv_label_set_text(lora_label, "LoRa Interface:");
    lv_obj_align(lora_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(lora_label, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(lora_label, &lv_font_montserrat_14, 0);

    _switch_lora_enabled = lv_switch_create(lora_row);
    lv_obj_set_size(_switch_lora_enabled, 40, 20);
    lv_obj_align(_switch_lora_enabled, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(_switch_lora_enabled, Theme::border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_switch_lora_enabled, Theme::primary(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(_switch_lora_enabled, on_lora_enabled_changed, LV_EVENT_VALUE_CHANGED, this);

    // LoRa parameters container (shown/hidden based on LoRa enabled)
    _lora_params_container = lv_obj_create(parent);
    lv_obj_set_width(_lora_params_container, LV_PCT(100));
    lv_obj_set_height(_lora_params_container, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(_lora_params_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_lora_params_container, 0, 0);
    lv_obj_set_style_pad_all(_lora_params_container, 0, 0);
    lv_obj_set_style_pad_gap(_lora_params_container, 2, 0);
    lv_obj_set_flex_flow(_lora_params_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(_lora_params_container, LV_OBJ_FLAG_SCROLLABLE);

    // Frequency row
    lv_obj_t* freq_row = lv_obj_create(_lora_params_container);
    lv_obj_set_width(freq_row, LV_PCT(100));
    lv_obj_set_height(freq_row, 28);
    lv_obj_set_style_bg_opa(freq_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(freq_row, 0, 0);
    lv_obj_set_style_pad_all(freq_row, 0, 0);
    lv_obj_clear_flag(freq_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* freq_label = lv_label_create(freq_row);
    lv_label_set_text(freq_label, "  Frequency:");
    lv_obj_align(freq_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(freq_label, lv_color_hex(0x909090), 0);
    lv_obj_set_style_text_font(freq_label, &lv_font_montserrat_14, 0);

    _ta_lora_frequency = lv_textarea_create(freq_row);
    lv_obj_set_size(_ta_lora_frequency, 80, 24);
    lv_obj_align(_ta_lora_frequency, LV_ALIGN_RIGHT_MID, -30, 0);
    lv_textarea_set_one_line(_ta_lora_frequency, true);
    lv_textarea_set_max_length(_ta_lora_frequency, 8);
    lv_textarea_set_accepted_chars(_ta_lora_frequency, "0123456789.");
    lv_obj_set_style_bg_color(_ta_lora_frequency, Theme::surfaceInput(), 0);
    lv_obj_set_style_text_color(_ta_lora_frequency, Theme::textPrimary(), 0);
    lv_obj_set_style_border_color(_ta_lora_frequency, Theme::border(), 0);
    lv_obj_set_style_border_width(_ta_lora_frequency, 1, 0);
    lv_obj_set_style_radius(_ta_lora_frequency, 4, 0);
    lv_obj_set_style_pad_all(_ta_lora_frequency, 4, 0);
    lv_obj_set_style_text_font(_ta_lora_frequency, &lv_font_montserrat_14, 0);
    // Enable paste on long-press
    TextAreaHelper::enable_paste(_ta_lora_frequency);

    lv_obj_t* mhz_label = lv_label_create(freq_row);
    lv_label_set_text(mhz_label, "MHz");
    lv_obj_align(mhz_label, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_color(mhz_label, Theme::textMuted(), 0);
    lv_obj_set_style_text_font(mhz_label, &lv_font_montserrat_14, 0);

    // Bandwidth dropdown row
    lv_obj_t* bw_row = lv_obj_create(_lora_params_container);
    lv_obj_set_width(bw_row, LV_PCT(100));
    lv_obj_set_height(bw_row, 28);
    lv_obj_set_style_bg_opa(bw_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bw_row, 0, 0);
    lv_obj_set_style_pad_all(bw_row, 0, 0);
    lv_obj_clear_flag(bw_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* bw_label = lv_label_create(bw_row);
    lv_label_set_text(bw_label, "  Bandwidth:");
    lv_obj_align(bw_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(bw_label, lv_color_hex(0x909090), 0);
    lv_obj_set_style_text_font(bw_label, &lv_font_montserrat_14, 0);

    _dropdown_lora_bandwidth = lv_dropdown_create(bw_row);
    lv_dropdown_set_options(_dropdown_lora_bandwidth, "62.5 kHz\n125 kHz\n250 kHz\n500 kHz");
    lv_obj_set_size(_dropdown_lora_bandwidth, 100, 28);
    lv_obj_align(_dropdown_lora_bandwidth, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(_dropdown_lora_bandwidth, Theme::surfaceInput(), 0);
    lv_obj_set_style_text_color(_dropdown_lora_bandwidth, Theme::textPrimary(), 0);
    lv_obj_set_style_border_color(_dropdown_lora_bandwidth, Theme::border(), 0);
    lv_obj_set_style_text_font(_dropdown_lora_bandwidth, &lv_font_montserrat_14, 0);

    // SF/CR row
    lv_obj_t* sfcr_row = lv_obj_create(_lora_params_container);
    lv_obj_set_width(sfcr_row, LV_PCT(100));
    lv_obj_set_height(sfcr_row, 28);
    lv_obj_set_style_bg_opa(sfcr_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sfcr_row, 0, 0);
    lv_obj_set_style_pad_all(sfcr_row, 0, 0);
    lv_obj_clear_flag(sfcr_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* sf_label = lv_label_create(sfcr_row);
    lv_label_set_text(sf_label, "  SF:");
    lv_obj_align(sf_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(sf_label, lv_color_hex(0x909090), 0);
    lv_obj_set_style_text_font(sf_label, &lv_font_montserrat_14, 0);

    _dropdown_lora_sf = lv_dropdown_create(sfcr_row);
    lv_dropdown_set_options(_dropdown_lora_sf, "5\n6\n7\n8\n9\n10\n11\n12");
    lv_obj_set_size(_dropdown_lora_sf, 50, 28);
    lv_obj_align(_dropdown_lora_sf, LV_ALIGN_LEFT_MID, 30, 0);
    lv_obj_set_style_bg_color(_dropdown_lora_sf, Theme::surfaceInput(), 0);
    lv_obj_set_style_text_color(_dropdown_lora_sf, Theme::textPrimary(), 0);
    lv_obj_set_style_border_color(_dropdown_lora_sf, Theme::border(), 0);
    lv_obj_set_style_text_font(_dropdown_lora_sf, &lv_font_montserrat_14, 0);

    lv_obj_t* cr_label = lv_label_create(sfcr_row);
    lv_label_set_text(cr_label, "CR:");
    lv_obj_align(cr_label, LV_ALIGN_LEFT_MID, 90, 0);
    lv_obj_set_style_text_color(cr_label, lv_color_hex(0x909090), 0);
    lv_obj_set_style_text_font(cr_label, &lv_font_montserrat_14, 0);

    _dropdown_lora_cr = lv_dropdown_create(sfcr_row);
    lv_dropdown_set_options(_dropdown_lora_cr, "4/5\n4/6\n4/7\n4/8");
    lv_obj_set_size(_dropdown_lora_cr, 55, 28);
    lv_obj_align(_dropdown_lora_cr, LV_ALIGN_LEFT_MID, 115, 0);
    lv_obj_set_style_bg_color(_dropdown_lora_cr, Theme::surfaceInput(), 0);
    lv_obj_set_style_text_color(_dropdown_lora_cr, Theme::textPrimary(), 0);
    lv_obj_set_style_border_color(_dropdown_lora_cr, Theme::border(), 0);
    lv_obj_set_style_text_font(_dropdown_lora_cr, &lv_font_montserrat_14, 0);

    // TX Power row
    lv_obj_t* power_row = lv_obj_create(_lora_params_container);
    lv_obj_set_width(power_row, LV_PCT(100));
    lv_obj_set_height(power_row, 28);
    lv_obj_set_style_bg_opa(power_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(power_row, 0, 0);
    lv_obj_set_style_pad_all(power_row, 0, 0);
    lv_obj_clear_flag(power_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* power_label = lv_label_create(power_row);
    lv_label_set_text(power_label, "  TX Power:");
    lv_obj_align(power_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(power_label, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(power_label, &lv_font_montserrat_14, 0);

    _slider_lora_power = lv_slider_create(power_row);
    lv_obj_set_size(_slider_lora_power, 100, 10);
    lv_obj_align(_slider_lora_power, LV_ALIGN_LEFT_MID, 75, 0);
    lv_slider_set_range(_slider_lora_power, 2, 22);
    lv_obj_set_style_bg_color(_slider_lora_power, Theme::border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_slider_lora_power, Theme::primary(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(_slider_lora_power, Theme::textPrimary(), LV_PART_KNOB);
    lv_obj_add_event_cb(_slider_lora_power, on_lora_power_changed, LV_EVENT_VALUE_CHANGED, this);

    _label_lora_power_value = lv_label_create(power_row);
    lv_label_set_text(_label_lora_power_value, "17 dBm");
    lv_obj_align(_label_lora_power_value, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_color(_label_lora_power_value, Theme::textPrimary(), 0);
    lv_obj_set_style_text_font(_label_lora_power_value, &lv_font_montserrat_14, 0);

    // Initially hide LoRa params if not enabled
    lv_obj_add_flag(_lora_params_container, LV_OBJ_FLAG_HIDDEN);
}

void SettingsScreen::create_delivery_section(lv_obj_t* parent) {
    create_section_header(parent, "== Delivery ==");

    // Propagation Nodes button row
    lv_obj_t* prop_nodes_row = lv_obj_create(parent);
    lv_obj_set_width(prop_nodes_row, LV_PCT(100));
    lv_obj_set_height(prop_nodes_row, 32);
    lv_obj_set_style_bg_opa(prop_nodes_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(prop_nodes_row, 0, 0);
    lv_obj_set_style_pad_all(prop_nodes_row, 0, 0);
    lv_obj_clear_flag(prop_nodes_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* prop_label = lv_label_create(prop_nodes_row);
    lv_label_set_text(prop_label, "Propagation Nodes:");
    lv_obj_align(prop_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(prop_label, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(prop_label, &lv_font_montserrat_14, 0);

    _btn_propagation_nodes = lv_btn_create(prop_nodes_row);
    lv_obj_set_size(_btn_propagation_nodes, 70, 26);
    lv_obj_align(_btn_propagation_nodes, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(_btn_propagation_nodes, Theme::primary(), 0);
    lv_obj_set_style_bg_color(_btn_propagation_nodes, Theme::primaryPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btn_propagation_nodes, on_propagation_nodes_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* btn_label = lv_label_create(_btn_propagation_nodes);
    lv_label_set_text(btn_label, "View");
    lv_obj_center(btn_label);
    lv_obj_set_style_text_color(btn_label, Theme::textPrimary(), 0);
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_14, 0);

    // Fallback to Propagation switch row
    lv_obj_t* fallback_row = lv_obj_create(parent);
    lv_obj_set_width(fallback_row, LV_PCT(100));
    lv_obj_set_height(fallback_row, 28);
    lv_obj_set_style_bg_opa(fallback_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(fallback_row, 0, 0);
    lv_obj_set_style_pad_all(fallback_row, 0, 0);
    lv_obj_clear_flag(fallback_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* fallback_label = lv_label_create(fallback_row);
    lv_label_set_text(fallback_label, "Fallback to Prop:");
    lv_obj_align(fallback_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(fallback_label, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(fallback_label, &lv_font_montserrat_14, 0);

    _switch_prop_fallback = lv_switch_create(fallback_row);
    lv_obj_set_size(_switch_prop_fallback, 40, 20);
    lv_obj_align(_switch_prop_fallback, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(_switch_prop_fallback, Theme::border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_switch_prop_fallback, Theme::primary(), LV_PART_INDICATOR | LV_STATE_CHECKED);

    // Propagation Only switch row
    lv_obj_t* prop_only_row = lv_obj_create(parent);
    lv_obj_set_width(prop_only_row, LV_PCT(100));
    lv_obj_set_height(prop_only_row, 28);
    lv_obj_set_style_bg_opa(prop_only_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(prop_only_row, 0, 0);
    lv_obj_set_style_pad_all(prop_only_row, 0, 0);
    lv_obj_clear_flag(prop_only_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* prop_only_label = lv_label_create(prop_only_row);
    lv_label_set_text(prop_only_label, "Propagation Only:");
    lv_obj_align(prop_only_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(prop_only_label, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(prop_only_label, &lv_font_montserrat_14, 0);

    _switch_prop_only = lv_switch_create(prop_only_row);
    lv_obj_set_size(_switch_prop_only, 40, 20);
    lv_obj_align(_switch_prop_only, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(_switch_prop_only, Theme::border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_switch_prop_only, Theme::primary(), LV_PART_INDICATOR | LV_STATE_CHECKED);
}

void SettingsScreen::create_advanced_section(lv_obj_t* parent) {
    create_section_header(parent, "== Advanced ==");

    // Announce interval row
    lv_obj_t* announce_row = lv_obj_create(parent);
    lv_obj_set_width(announce_row, LV_PCT(100));
    lv_obj_set_height(announce_row, 28);
    lv_obj_set_style_bg_opa(announce_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(announce_row, 0, 0);
    lv_obj_set_style_pad_all(announce_row, 0, 0);
    lv_obj_clear_flag(announce_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* announce_label = lv_label_create(announce_row);
    lv_label_set_text(announce_label, "Announce Interval (min):");
    lv_obj_align(announce_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(announce_label, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(announce_label, &lv_font_montserrat_14, 0);

    _ta_announce_interval = lv_textarea_create(announce_row);
    lv_obj_set_size(_ta_announce_interval, 50, 24);
    lv_obj_align(_ta_announce_interval, LV_ALIGN_RIGHT_MID, -30, 0);
    lv_textarea_set_one_line(_ta_announce_interval, true);
    lv_textarea_set_max_length(_ta_announce_interval, 5);
    lv_textarea_set_accepted_chars(_ta_announce_interval, "0123456789");
    lv_obj_set_style_bg_color(_ta_announce_interval, Theme::surfaceInput(), 0);
    lv_obj_set_style_text_color(_ta_announce_interval, Theme::textPrimary(), 0);
    lv_obj_set_style_border_color(_ta_announce_interval, Theme::border(), 0);
    lv_obj_set_style_border_width(_ta_announce_interval, 1, 0);
    lv_obj_set_style_radius(_ta_announce_interval, 4, 0);
    lv_obj_set_style_pad_all(_ta_announce_interval, 4, 0);
    lv_obj_set_style_text_font(_ta_announce_interval, &lv_font_montserrat_14, 0);
    // Add to input group for keyboard navigation
    lv_group_t* grp = LVGL::LVGLInit::get_default_group();
    if (grp) {
        lv_group_add_obj(grp, _ta_announce_interval);
    }
    // Enable paste on long-press
    TextAreaHelper::enable_paste(_ta_announce_interval);

    lv_obj_t* sec_label = lv_label_create(announce_row);
    lv_label_set_text(sec_label, "sec");
    lv_obj_align(sec_label, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_color(sec_label, Theme::textMuted(), 0);
    lv_obj_set_style_text_font(sec_label, &lv_font_montserrat_14, 0);

    // Sync interval row (for propagation node sync)
    lv_obj_t* sync_row = lv_obj_create(parent);
    lv_obj_set_width(sync_row, LV_PCT(100));
    lv_obj_set_height(sync_row, 28);
    lv_obj_set_style_bg_opa(sync_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sync_row, 0, 0);
    lv_obj_set_style_pad_all(sync_row, 0, 0);
    lv_obj_clear_flag(sync_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* sync_label = lv_label_create(sync_row);
    lv_label_set_text(sync_label, "Prop Sync Interval (hrs):");
    lv_obj_align(sync_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(sync_label, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(sync_label, &lv_font_montserrat_14, 0);

    _ta_sync_interval = lv_textarea_create(sync_row);
    lv_obj_set_size(_ta_sync_interval, 50, 24);
    lv_obj_align(_ta_sync_interval, LV_ALIGN_RIGHT_MID, -30, 0);
    lv_textarea_set_one_line(_ta_sync_interval, true);
    lv_textarea_set_max_length(_ta_sync_interval, 5);
    lv_textarea_set_accepted_chars(_ta_sync_interval, "0123456789");
    lv_obj_set_style_bg_color(_ta_sync_interval, Theme::surfaceInput(), 0);
    lv_obj_set_style_text_color(_ta_sync_interval, Theme::textPrimary(), 0);
    lv_obj_set_style_border_color(_ta_sync_interval, Theme::border(), 0);
    lv_obj_set_style_border_width(_ta_sync_interval, 1, 0);
    lv_obj_set_style_radius(_ta_sync_interval, 4, 0);
    lv_obj_set_style_pad_all(_ta_sync_interval, 4, 0);
    lv_obj_set_style_text_font(_ta_sync_interval, &lv_font_montserrat_14, 0);
    if (grp) {
        lv_group_add_obj(grp, _ta_sync_interval);
    }
    TextAreaHelper::enable_paste(_ta_sync_interval);

    lv_obj_t* min_label = lv_label_create(sync_row);
    lv_label_set_text(min_label, "min");
    lv_obj_align(min_label, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_color(min_label, Theme::textMuted(), 0);
    lv_obj_set_style_text_font(min_label, &lv_font_montserrat_14, 0);

    // GPS sync row
    lv_obj_t* gps_sync_row = lv_obj_create(parent);
    lv_obj_set_width(gps_sync_row, LV_PCT(100));
    lv_obj_set_height(gps_sync_row, 28);
    lv_obj_set_style_bg_opa(gps_sync_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gps_sync_row, 0, 0);
    lv_obj_set_style_pad_all(gps_sync_row, 0, 0);
    lv_obj_clear_flag(gps_sync_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* gps_sync_label = lv_label_create(gps_sync_row);
    lv_label_set_text(gps_sync_label, "GPS Time Sync:");
    lv_obj_align(gps_sync_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(gps_sync_label, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(gps_sync_label, &lv_font_montserrat_14, 0);

    _switch_gps_sync = lv_switch_create(gps_sync_row);
    lv_obj_set_size(_switch_gps_sync, 40, 20);
    lv_obj_align(_switch_gps_sync, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(_switch_gps_sync, Theme::border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_switch_gps_sync, Theme::primary(), LV_PART_INDICATOR | LV_STATE_CHECKED);

}

void SettingsScreen::create_transport_mode_section(lv_obj_t* parent) {
    create_section_header(parent, "== DANGER: Transport Mode ==");

    lv_obj_t* warning = lv_label_create(parent);
    lv_obj_set_width(warning, LV_PCT(100));
    lv_label_set_long_mode(warning, LV_LABEL_LONG_WRAP);
    lv_label_set_text(
        warning,
        "NOT RECOMMENDED. Transport mode routes other nodes' Reticulum traffic "
        "across every enabled interface. It can saturate LoRa airtime, drain the battery, "
        "and turn this handheld into network infrastructure. Enable only if you understand "
        "Reticulum routing and intend this device to be a transport node. Takes effect after reboot.");
    lv_obj_set_style_text_color(warning, Theme::error(), 0);
    lv_obj_set_style_text_font(warning, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_bottom(warning, 4, 0);

    // Keep this row as the final object in Settings so the opt-in cannot be
    // mistaken for an ordinary interface switch higher in the screen.
    lv_obj_t* transport_row = lv_obj_create(parent);
    lv_obj_set_width(transport_row, LV_PCT(100));
    lv_obj_set_height(transport_row, 32);
    lv_obj_set_style_bg_opa(transport_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(transport_row, 0, 0);
    lv_obj_set_style_pad_all(transport_row, 0, 0);
    lv_obj_clear_flag(transport_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* label = lv_label_create(transport_row);
    lv_label_set_text(label, "Enable Transport Node:");
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(label, Theme::error(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);

    _switch_transport_enabled = lv_switch_create(transport_row);
    lv_obj_set_size(_switch_transport_enabled, 40, 20);
    lv_obj_align(_switch_transport_enabled, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(_switch_transport_enabled, Theme::border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_switch_transport_enabled, Theme::error(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(_switch_transport_enabled, on_transport_enabled_changed, LV_EVENT_VALUE_CHANGED, this);

    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        lv_group_add_obj(group, _switch_transport_enabled);
    }
}

void SettingsScreen::show_transport_warning() {
    if (_transport_warning_modal) return;

    _transport_warning_modal = lv_obj_create(_screen);
    lv_obj_set_size(_transport_warning_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_center(_transport_warning_modal);
    lv_obj_set_style_bg_color(_transport_warning_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_transport_warning_modal, LV_OPA_80, 0);
    lv_obj_set_style_border_width(_transport_warning_modal, 0, 0);
    lv_obj_set_style_radius(_transport_warning_modal, 0, 0);
    lv_obj_set_style_pad_all(_transport_warning_modal, 8, 0);
    lv_obj_clear_flag(_transport_warning_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_transport_warning_modal, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* panel = lv_obj_create(_transport_warning_modal);
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, Theme::surface(), 0);
    lv_obj_set_style_border_color(panel, Theme::error(), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "DANGER: Transport Mode");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_color(title, Theme::error(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    lv_obj_t* text = lv_label_create(panel);
    lv_obj_set_width(text, LV_PCT(100));
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(
        text,
        "This is NOT RECOMMENDED. The device will relay other nodes' traffic and may "
        "saturate LoRa airtime, drain the battery, and disrupt nearby Reticulum users.\n\n"
        "Enable only if you understand the consequences and deliberately want this T-Deck "
        "to operate as network infrastructure. Takes effect after reboot.");
    lv_obj_align(text, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_obj_set_style_text_color(text, Theme::textPrimary(), 0);
    lv_obj_set_style_text_font(text, &lv_font_montserrat_12, 0);

    lv_obj_t* cancel = lv_btn_create(panel);
    lv_obj_set_size(cancel, 100, 32);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, Theme::btnSecondary(), 0);
    lv_obj_add_event_cb(cancel, on_transport_cancel_enable, LV_EVENT_CLICKED, this);
    lv_obj_t* cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);

    lv_obj_t* confirm = lv_btn_create(panel);
    lv_obj_set_size(confirm, 145, 32);
    lv_obj_align(confirm, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(confirm, Theme::error(), 0);
    lv_obj_add_event_cb(confirm, on_transport_confirm_enable, LV_EVENT_CLICKED, this);
    lv_obj_t* confirm_label = lv_label_create(confirm);
    lv_label_set_text(confirm_label, "ENABLE ANYWAY");
    lv_obj_center(confirm_label);

    _transport_modal_group = lv_group_create();
    if (_transport_modal_group) {
        lv_group_add_obj(_transport_modal_group, cancel);
        lv_group_add_obj(_transport_modal_group, confirm);
        lv_group_focus_obj(cancel);

        // Isolate keyboard and trackball focus inside the warning. Otherwise
        // users can navigate into and activate Settings controls behind the
        // still-visible modal.
        lv_indev_t* keyboard = LVGL::LVGLInit::get_keyboard();
        lv_indev_t* trackball = LVGL::LVGLInit::get_trackball();
        if (keyboard) lv_indev_set_group(keyboard, _transport_modal_group);
        if (trackball) lv_indev_set_group(trackball, _transport_modal_group);
    }
}

void SettingsScreen::close_transport_warning() {
    if (!_transport_warning_modal) return;

    lv_group_t* default_group = LVGL::LVGLInit::get_default_group();
    lv_indev_t* keyboard = LVGL::LVGLInit::get_keyboard();
    lv_indev_t* trackball = LVGL::LVGLInit::get_trackball();
    if (keyboard) lv_indev_set_group(keyboard, default_group);
    if (trackball) lv_indev_set_group(trackball, default_group);

    if (_transport_modal_group) {
        lv_group_del(_transport_modal_group);
        _transport_modal_group = nullptr;
    }

    lv_obj_t* modal = _transport_warning_modal;
    _transport_warning_modal = nullptr;
    lv_obj_del_async(modal);

    if (default_group && _switch_transport_enabled) {
        lv_group_focus_obj(_switch_transport_enabled);
    }
}

void SettingsScreen::load_settings() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);  // read-only

    _settings.wifi_ssid = prefs.getString(KEY_WIFI_SSID, "");
    _settings.wifi_password = prefs.getString(KEY_WIFI_PASS, "");
    _settings.tcp_host = prefs.getString(KEY_TCP_HOST, "sideband.connect.reticulum.network");
    _settings.tcp_port = prefs.getUShort(KEY_TCP_PORT, 4965);
    _settings.display_name = prefs.getString(KEY_DISPLAY_NAME, "");
    _settings.brightness = prefs.getUChar(KEY_BRIGHTNESS, 180);
    _settings.keyboard_light = prefs.getBool(KEY_KB_LIGHT, false);
    _settings.screen_timeout = prefs.getUShort(KEY_TIMEOUT, 60);
    _settings.announce_interval = prefs.getUInt(KEY_ANNOUNCE_INT, 14400);  // Default 14400s = 4 hours
    _settings.sync_interval = prefs.getUInt(KEY_SYNC_INT, 14400);  // Default 14400s = 4 hours
    _settings.gps_time_sync = prefs.getBool(KEY_GPS_SYNC, true);
    _settings.transport_enabled = prefs.getBool(KEY_TRANSPORT_ENABLED, false);

    // Notification settings
    _settings.notification_sound = prefs.getBool(KEY_NOTIF_SND, true);
    _settings.notification_volume = prefs.getUChar(KEY_NOTIF_VOL, 10);

    // Interface settings
    _settings.tcp_enabled = prefs.getBool(KEY_TCP_ENABLED, true);
    _settings.lora_enabled = prefs.getBool(KEY_LORA_ENABLED, false);
    _settings.lora_frequency = prefs.getFloat(KEY_LORA_FREQ, 927.25f);
    _settings.lora_bandwidth = prefs.getFloat(KEY_LORA_BW, 62.5f);
    // Validate bandwidth - SX1262 valid values: 7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125, 250, 500
    // If saved value is invalid (like 50 kHz), correct to nearest valid value
    if (_settings.lora_bandwidth < 60.0f) {
        _settings.lora_bandwidth = 62.5f;  // 50 kHz -> 62.5 kHz
    }
    _settings.lora_sf = prefs.getUChar(KEY_LORA_SF, 7);
    _settings.lora_cr = prefs.getUChar(KEY_LORA_CR, 5);
    _settings.lora_power = prefs.getChar(KEY_LORA_POWER, 17);
    _settings.auto_enabled = prefs.getBool(KEY_AUTO_ENABLED, false);
    _settings.ble_enabled = prefs.getBool(KEY_BLE_ENABLED, false);

    // Propagation settings
    _settings.prop_auto_select = prefs.getBool(KEY_PROP_AUTO, true);
    _settings.prop_selected_node = prefs.getString(KEY_PROP_NODE, "");
    _settings.prop_fallback_enabled = prefs.getBool(KEY_PROP_FALLBACK, true);
    _settings.prop_only = prefs.getBool(KEY_PROP_ONLY, false);

    prefs.end();

    DEBUG("Settings loaded from NVS");
}

void SettingsScreen::save_settings() {
    update_settings_from_ui();
    std::uint8_t expected = SAVE_IDLE;
    if (!_save_state.compare_exchange_strong(
            expected, SAVE_PROCESSING, std::memory_order_acq_rel)) {
        WARNING("Settings save already pending");
        return;
    }
    _pending_save_settings = _settings;
    _save_state.store(SAVE_PENDING, std::memory_order_release);
}

void SettingsScreen::service_pending_save() {
    std::uint8_t expected = _save_state.load(std::memory_order_acquire);
    if (expected != SAVE_PENDING && expected != SAVE_APPLY_RETRY) return;
    if (expected == SAVE_APPLY_RETRY &&
        static_cast<std::int32_t>(millis() - _apply_retry_at_ms) < 0) return;
    const bool needs_persistence = expected == SAVE_PENDING;
    if (!_save_state.compare_exchange_strong(
            expected, SAVE_PROCESSING, std::memory_order_acq_rel)) return;
    const AppSettings settings = _pending_save_settings;

    if (needs_persistence) {
        Preferences prefs;
        prefs.begin(NVS_NAMESPACE, false);
        prefs.putString(KEY_WIFI_SSID, settings.wifi_ssid);
        prefs.putString(KEY_WIFI_PASS, settings.wifi_password);
        prefs.putString(KEY_TCP_HOST, settings.tcp_host);
        prefs.putUShort(KEY_TCP_PORT, settings.tcp_port);
        prefs.putString(KEY_DISPLAY_NAME, settings.display_name);
        prefs.putUChar(KEY_BRIGHTNESS, settings.brightness);
        prefs.putBool(KEY_KB_LIGHT, settings.keyboard_light);
        prefs.putUShort(KEY_TIMEOUT, settings.screen_timeout);
        prefs.putUInt(KEY_ANNOUNCE_INT, settings.announce_interval);
        prefs.putUInt(KEY_SYNC_INT, settings.sync_interval);
        prefs.putBool(KEY_GPS_SYNC, settings.gps_time_sync);
        prefs.putBool(KEY_TRANSPORT_ENABLED, settings.transport_enabled);
        prefs.putBool(KEY_NOTIF_SND, settings.notification_sound);
        prefs.putUChar(KEY_NOTIF_VOL, settings.notification_volume);
        prefs.putBool(KEY_TCP_ENABLED, settings.tcp_enabled);
        prefs.putBool(KEY_LORA_ENABLED, settings.lora_enabled);
        prefs.putFloat(KEY_LORA_FREQ, settings.lora_frequency);
        prefs.putFloat(KEY_LORA_BW, settings.lora_bandwidth);
        prefs.putUChar(KEY_LORA_SF, settings.lora_sf);
        prefs.putUChar(KEY_LORA_CR, settings.lora_cr);
        prefs.putChar(KEY_LORA_POWER, settings.lora_power);
        prefs.putBool(KEY_AUTO_ENABLED, settings.auto_enabled);
        prefs.putBool(KEY_BLE_ENABLED, settings.ble_enabled);
        prefs.putBool(KEY_PROP_AUTO, settings.prop_auto_select);
        prefs.putString(KEY_PROP_NODE, settings.prop_selected_node);
        prefs.putBool(KEY_PROP_FALLBACK, settings.prop_fallback_enabled);
        prefs.putBool(KEY_PROP_ONLY, settings.prop_only);
        prefs.end();
        INFO("Settings saved to NVS");
    }

    const bool applied = !_save_callback || _save_callback(settings);
    if (!applied) _apply_retry_at_ms = millis() + 1000U;
    _save_state.store(applied ? SAVE_IDLE : SAVE_APPLY_RETRY,
                      std::memory_order_release);
}

void SettingsScreen::update_ui_from_settings() {
    LVGL_LOCK();
    if (_ta_wifi_ssid) {
        lv_textarea_set_text(_ta_wifi_ssid, _settings.wifi_ssid.c_str());
    }
    if (_ta_wifi_password) {
        lv_textarea_set_text(_ta_wifi_password, _settings.wifi_password.c_str());
    }
    if (_ta_tcp_host) {
        lv_textarea_set_text(_ta_tcp_host, _settings.tcp_host.c_str());
    }
    if (_ta_tcp_port) {
        lv_textarea_set_text(_ta_tcp_port, String(_settings.tcp_port).c_str());
    }
    if (_ta_display_name) {
        lv_textarea_set_text(_ta_display_name, _settings.display_name.c_str());
    }
    if (_slider_brightness) {
        lv_slider_set_value(_slider_brightness, _settings.brightness, LV_ANIM_OFF);
        if (_label_brightness_value) {
            lv_label_set_text(_label_brightness_value, String(_settings.brightness).c_str());
        }
    }
    if (_switch_kb_light) {
        if (_settings.keyboard_light) {
            lv_obj_add_state(_switch_kb_light, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(_switch_kb_light, LV_STATE_CHECKED);
        }
    }

    // Notification settings
    if (_switch_notification_sound) {
        if (_settings.notification_sound) {
            lv_obj_add_state(_switch_notification_sound, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(_switch_notification_sound, LV_STATE_CHECKED);
        }
    }
    if (_slider_notification_volume) {
        lv_slider_set_value(_slider_notification_volume, _settings.notification_volume, LV_ANIM_OFF);
        if (_label_notification_volume_value) {
            lv_label_set_text(_label_notification_volume_value, String(_settings.notification_volume).c_str());
        }
    }

    if (_dropdown_timeout) {
        // Map timeout to dropdown index
        int idx = 1;  // default 1 min
        if (_settings.screen_timeout == 30) idx = 0;
        else if (_settings.screen_timeout == 60) idx = 1;
        else if (_settings.screen_timeout == 300) idx = 2;
        else if (_settings.screen_timeout == 0) idx = 3;
        lv_dropdown_set_selected(_dropdown_timeout, idx);
    }
    if (_ta_announce_interval) {
        lv_textarea_set_text(_ta_announce_interval, String(_settings.announce_interval / 60).c_str());  // stored seconds -> shown minutes
    }
    if (_ta_sync_interval) {
        // Display in hours (stored in seconds)
        lv_textarea_set_text(_ta_sync_interval, String(_settings.sync_interval / 3600).c_str());  // stored seconds -> shown hours
    }
    if (_switch_gps_sync) {
        if (_settings.gps_time_sync) {
            lv_obj_add_state(_switch_gps_sync, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(_switch_gps_sync, LV_STATE_CHECKED);
        }
    }
    if (_switch_transport_enabled) {
        // Bypass the user-confirmation handler while reflecting persisted state.
        _transport_enable_confirmed = true;
        if (_settings.transport_enabled) {
            lv_obj_add_state(_switch_transport_enabled, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(_switch_transport_enabled, LV_STATE_CHECKED);
        }
        _transport_enable_confirmed = false;
    }

    // Interface settings
    if (_switch_tcp_enabled) {
        if (_settings.tcp_enabled) {
            lv_obj_add_state(_switch_tcp_enabled, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(_switch_tcp_enabled, LV_STATE_CHECKED);
        }
    }
    if (_switch_lora_enabled) {
        if (_settings.lora_enabled) {
            lv_obj_add_state(_switch_lora_enabled, LV_STATE_CHECKED);
            if (_lora_params_container) {
                lv_obj_clear_flag(_lora_params_container, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_clear_state(_switch_lora_enabled, LV_STATE_CHECKED);
            if (_lora_params_container) {
                lv_obj_add_flag(_lora_params_container, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    if (_ta_lora_frequency) {
        char freq_str[16];
        snprintf(freq_str, sizeof(freq_str), "%.2f", _settings.lora_frequency);
        lv_textarea_set_text(_ta_lora_frequency, freq_str);
    }
    if (_dropdown_lora_bandwidth) {
        // Map bandwidth to index: 62.5=0, 125=1, 250=2, 500=3
        int idx = 0;
        if (_settings.lora_bandwidth < 100.0f) idx = 0;       // 62.5 kHz
        else if (_settings.lora_bandwidth < 200.0f) idx = 1;  // 125 kHz
        else if (_settings.lora_bandwidth < 400.0f) idx = 2;  // 250 kHz
        else idx = 3;  // 500 kHz
        lv_dropdown_set_selected(_dropdown_lora_bandwidth, idx);
    }
    if (_dropdown_lora_sf) {
        // SF 5-12 maps to index 0-7
        lv_dropdown_set_selected(_dropdown_lora_sf, _settings.lora_sf - 5);
    }
    if (_dropdown_lora_cr) {
        // CR 5-8 maps to index 0-3
        lv_dropdown_set_selected(_dropdown_lora_cr, _settings.lora_cr - 5);
    }
    if (_slider_lora_power) {
        lv_slider_set_value(_slider_lora_power, _settings.lora_power, LV_ANIM_OFF);
        if (_label_lora_power_value) {
            char pwr_str[16];
            snprintf(pwr_str, sizeof(pwr_str), "%d dBm", _settings.lora_power);
            lv_label_set_text(_label_lora_power_value, pwr_str);
        }
    }
    if (_switch_auto_enabled) {
        if (_settings.auto_enabled) {
            lv_obj_add_state(_switch_auto_enabled, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(_switch_auto_enabled, LV_STATE_CHECKED);
        }
    }
    if (_switch_ble_enabled) {
        if (_settings.ble_enabled) {
            lv_obj_add_state(_switch_ble_enabled, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(_switch_ble_enabled, LV_STATE_CHECKED);
        }
    }

    // Propagation settings
    if (_switch_prop_fallback) {
        if (_settings.prop_fallback_enabled) {
            lv_obj_add_state(_switch_prop_fallback, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(_switch_prop_fallback, LV_STATE_CHECKED);
        }
    }
    if (_switch_prop_only) {
        if (_settings.prop_only) {
            lv_obj_add_state(_switch_prop_only, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(_switch_prop_only, LV_STATE_CHECKED);
        }
    }
}

void SettingsScreen::update_settings_from_ui() {
    LVGL_LOCK();
    if (_ta_wifi_ssid) {
        _settings.wifi_ssid = lv_textarea_get_text(_ta_wifi_ssid);
    }
    if (_ta_wifi_password) {
        _settings.wifi_password = lv_textarea_get_text(_ta_wifi_password);
    }
    if (_ta_tcp_host) {
        _settings.tcp_host = lv_textarea_get_text(_ta_tcp_host);
    }
    if (_ta_tcp_port) {
        _settings.tcp_port = String(lv_textarea_get_text(_ta_tcp_port)).toInt();
    }
    if (_ta_display_name) {
        _settings.display_name = lv_textarea_get_text(_ta_display_name);
    }
    if (_slider_brightness) {
        _settings.brightness = lv_slider_get_value(_slider_brightness);
    }
    if (_switch_kb_light) {
        _settings.keyboard_light = lv_obj_has_state(_switch_kb_light, LV_STATE_CHECKED);
    }

    // Notification settings
    if (_switch_notification_sound) {
        _settings.notification_sound = lv_obj_has_state(_switch_notification_sound, LV_STATE_CHECKED);
    }
    if (_slider_notification_volume) {
        _settings.notification_volume = lv_slider_get_value(_slider_notification_volume);
    }

    if (_dropdown_timeout) {
        int idx = lv_dropdown_get_selected(_dropdown_timeout);
        switch (idx) {
            case 0: _settings.screen_timeout = 30; break;
            case 1: _settings.screen_timeout = 60; break;
            case 2: _settings.screen_timeout = 300; break;
            case 3: _settings.screen_timeout = 0; break;
        }
    }
    if (_ta_announce_interval) {
        _settings.announce_interval = String(lv_textarea_get_text(_ta_announce_interval)).toInt() * 60;  // minutes -> seconds
    }
    if (_ta_sync_interval) {
        // UI shows hours, store as seconds
        _settings.sync_interval = String(lv_textarea_get_text(_ta_sync_interval)).toInt() * 3600;  // hours -> seconds
    }
    if (_switch_gps_sync) {
        _settings.gps_time_sync = lv_obj_has_state(_switch_gps_sync, LV_STATE_CHECKED);
    }
    if (_switch_transport_enabled) {
        _settings.transport_enabled = lv_obj_has_state(_switch_transport_enabled, LV_STATE_CHECKED);
    }

    // Interface settings
    if (_switch_tcp_enabled) {
        _settings.tcp_enabled = lv_obj_has_state(_switch_tcp_enabled, LV_STATE_CHECKED);
    }
    if (_switch_lora_enabled) {
        _settings.lora_enabled = lv_obj_has_state(_switch_lora_enabled, LV_STATE_CHECKED);
    }
    if (_ta_lora_frequency) {
        _settings.lora_frequency = String(lv_textarea_get_text(_ta_lora_frequency)).toFloat();
    }
    if (_dropdown_lora_bandwidth) {
        // Map index to bandwidth: 0=62.5, 1=125, 2=250, 3=500
        static const float bw_values[] = {62.5f, 125.0f, 250.0f, 500.0f};
        int idx = lv_dropdown_get_selected(_dropdown_lora_bandwidth);
        if (idx >= 0 && idx < 4) {
            _settings.lora_bandwidth = bw_values[idx];
        }
    }
    if (_dropdown_lora_sf) {
        // Index 0-7 maps to SF 5-12
        _settings.lora_sf = lv_dropdown_get_selected(_dropdown_lora_sf) + 5;
    }
    if (_dropdown_lora_cr) {
        // Index 0-3 maps to CR 5-8
        _settings.lora_cr = lv_dropdown_get_selected(_dropdown_lora_cr) + 5;
    }
    if (_slider_lora_power) {
        _settings.lora_power = lv_slider_get_value(_slider_lora_power);
    }
    if (_switch_auto_enabled) {
        _settings.auto_enabled = lv_obj_has_state(_switch_auto_enabled, LV_STATE_CHECKED);
    }
    if (_switch_ble_enabled) {
        _settings.ble_enabled = lv_obj_has_state(_switch_ble_enabled, LV_STATE_CHECKED);
    }

    // Propagation settings
    if (_switch_prop_fallback) {
        _settings.prop_fallback_enabled = lv_obj_has_state(_switch_prop_fallback, LV_STATE_CHECKED);
    }
    if (_switch_prop_only) {
        _settings.prop_only = lv_obj_has_state(_switch_prop_only, LV_STATE_CHECKED);
    }
}

void SettingsScreen::set_back_callback(BackCallback callback) {
    _back_callback = callback;
}

void SettingsScreen::set_save_callback(SaveCallback callback) {
    _save_callback = callback;
}

void SettingsScreen::set_wifi_reconnect_callback(WifiReconnectCallback callback) {
    _wifi_reconnect_callback = callback;
}

void SettingsScreen::set_brightness_change_callback(BrightnessChangeCallback callback) {
    _brightness_change_callback = callback;
}

void SettingsScreen::set_propagation_nodes_callback(PropagationNodesCallback callback) {
    _propagation_nodes_callback = callback;
}

void SettingsScreen::set_status_callback(StatusScreenCallback callback) {
    _status_callback = callback;
}

void SettingsScreen::show() {
    LVGL_LOCK();
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_screen);

    // Add back/save buttons to focus group for trackball navigation
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        if (_btn_back) lv_group_add_obj(group, _btn_back);
        if (_btn_save) lv_group_add_obj(group, _btn_save);

        // Focus on back button
        if (_btn_back) {
            lv_group_focus_obj(_btn_back);
        }
    }
}

void SettingsScreen::hide() {
    LVGL_LOCK();
    close_transport_warning();
    // Remove from focus group when hiding
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        if (_btn_back) lv_group_remove_obj(_btn_back);
        if (_btn_save) lv_group_remove_obj(_btn_save);
    }

    lv_obj_add_flag(_screen, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* SettingsScreen::get_object() {
    return _screen;
}

void SettingsScreen::on_back_clicked(lv_event_t* event) {
    SettingsScreen* screen = (SettingsScreen*)lv_event_get_user_data(event);
    if (screen->_back_callback) {
        screen->_back_callback();
    }
}

void SettingsScreen::on_save_clicked(lv_event_t* event) {
    SettingsScreen* screen = (SettingsScreen*)lv_event_get_user_data(event);
    screen->save_settings();
}

void SettingsScreen::on_reconnect_clicked(lv_event_t* event) {
    SettingsScreen* screen = (SettingsScreen*)lv_event_get_user_data(event);

    const char* ssid = lv_textarea_get_text(screen->_ta_wifi_ssid);
    const char* pass = lv_textarea_get_text(screen->_ta_wifi_password);

    if (screen->_wifi_reconnect_callback) {
        screen->_wifi_reconnect_callback(String(ssid), String(pass));
    }
}

void SettingsScreen::on_brightness_changed(lv_event_t* event) {
    SettingsScreen* screen = (SettingsScreen*)lv_event_get_user_data(event);
    uint8_t brightness = lv_slider_get_value(screen->_slider_brightness);

    // Update label
    lv_label_set_text(screen->_label_brightness_value, String(brightness).c_str());

    // Apply immediately
    if (screen->_brightness_change_callback) {
        screen->_brightness_change_callback(brightness);
    }
}

void SettingsScreen::on_lora_enabled_changed(lv_event_t* event) {
    SettingsScreen* screen = (SettingsScreen*)lv_event_get_user_data(event);
    bool enabled = lv_obj_has_state(screen->_switch_lora_enabled, LV_STATE_CHECKED);

    // Show/hide LoRa parameters
    if (screen->_lora_params_container) {
        if (enabled) {
            lv_obj_clear_flag(screen->_lora_params_container, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(screen->_lora_params_container, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void SettingsScreen::on_lora_power_changed(lv_event_t* event) {
    SettingsScreen* screen = (SettingsScreen*)lv_event_get_user_data(event);
    int8_t power = lv_slider_get_value(screen->_slider_lora_power);

    // Update label
    char pwr_str[16];
    snprintf(pwr_str, sizeof(pwr_str), "%d dBm", power);
    lv_label_set_text(screen->_label_lora_power_value, pwr_str);
}

void SettingsScreen::on_notification_volume_changed(lv_event_t* event) {
    SettingsScreen* screen = (SettingsScreen*)lv_event_get_user_data(event);
    uint8_t volume = lv_slider_get_value(screen->_slider_notification_volume);

    // Update label
    lv_label_set_text(screen->_label_notification_volume_value, String(volume).c_str());
}

void SettingsScreen::on_transport_enabled_changed(lv_event_t* event) {
    SettingsScreen* screen = (SettingsScreen*)lv_event_get_user_data(event);
    bool enabled = lv_obj_has_state(screen->_switch_transport_enabled, LV_STATE_CHECKED);

    if (!enabled) {
        screen->_transport_enable_confirmed = false;
        return;
    }

    if (screen->_transport_enable_confirmed) {
        return;
    }

    // Never leave the switch enabled merely because it was toggled. The user
    // must complete the explicit second confirmation in the danger dialog.
    lv_obj_clear_state(screen->_switch_transport_enabled, LV_STATE_CHECKED);
    screen->show_transport_warning();
}

void SettingsScreen::on_transport_confirm_enable(lv_event_t* event) {
    SettingsScreen* screen = (SettingsScreen*)lv_event_get_user_data(event);
    screen->_transport_enable_confirmed = true;
    lv_obj_add_state(screen->_switch_transport_enabled, LV_STATE_CHECKED);
    screen->_transport_enable_confirmed = false;
    screen->close_transport_warning();
    INFO("Transport mode opt-in confirmed; save settings and reboot to activate");
}

void SettingsScreen::on_transport_cancel_enable(lv_event_t* event) {
    SettingsScreen* screen = (SettingsScreen*)lv_event_get_user_data(event);
    screen->_transport_enable_confirmed = false;
    lv_obj_clear_state(screen->_switch_transport_enabled, LV_STATE_CHECKED);
    screen->close_transport_warning();
}

void SettingsScreen::on_status_clicked(lv_event_t* event) {
    SettingsScreen* screen = (SettingsScreen*)lv_event_get_user_data(event);
    if (screen->_status_callback) {
        screen->_status_callback();
    }
}

void SettingsScreen::on_propagation_nodes_clicked(lv_event_t* event) {
    SettingsScreen* screen = (SettingsScreen*)lv_event_get_user_data(event);
    if (screen->_propagation_nodes_callback) {
        screen->_propagation_nodes_callback();
    }
}

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
