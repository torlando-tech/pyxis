// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "ConversationListScreen.h"

#ifdef ARDUINO

#include "Theme.h"
#include "Log.h"
#include "Identity.h"
#include "Utilities/OS.h"
#include "../../Hardware/TDeck/Config.h"
#include "../LVGL/LVGLInit.h"
#include "../LVGL/LVGLLock.h"
#include <WiFi.h>
#include <MsgPack.h>
#include <TinyGPSPlus.h>

using namespace RNS;
using namespace Hardware::TDeck;

namespace UI {
namespace LXMF {

ConversationListScreen::ConversationListScreen(lv_obj_t* parent)
    : _screen(nullptr), _header(nullptr), _list(nullptr), _bottom_nav(nullptr),
      _btn_new(nullptr), _btn_settings(nullptr), _label_wifi(nullptr), _label_lora(nullptr),
      _label_gps(nullptr), _label_ble(nullptr), _battery_container(nullptr),
      _label_battery_icon(nullptr), _label_battery_pct(nullptr),
      _lora_interface(nullptr), _ble_interface(nullptr), _gps(nullptr),
      _message_store(nullptr) {
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

    // Create UI components
    create_header();
    create_list();
    create_bottom_nav();

    TRACE("ConversationListScreen created");
}

ConversationListScreen::~ConversationListScreen() {
    LVGL_LOCK();
    // Pool handles cleanup automatically when vector destructs
    if (_screen) {
        lv_obj_del(_screen);
    }
}

void ConversationListScreen::create_header() {
    _header = lv_obj_create(_screen);
    lv_obj_set_size(_header, LV_PCT(100), 36);
    lv_obj_align(_header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(_header, Theme::surfaceHeader(), 0);
    lv_obj_set_style_border_width(_header, 0, 0);
    lv_obj_set_style_radius(_header, 0, 0);
    lv_obj_set_style_pad_all(_header, 0, 0);

    // Title
    lv_obj_t* title = lv_label_create(_header);
    lv_label_set_text(title, "LXMF");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(title, Theme::textPrimary(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    // Status indicators - compact layout: WiFi, LoRa, GPS, BLE, Battery(vertical)
    _label_wifi = lv_label_create(_header);
    lv_label_set_text(_label_wifi, LV_SYMBOL_WIFI " --");
    lv_obj_align(_label_wifi, LV_ALIGN_LEFT_MID, 54, 0);
    lv_obj_set_style_text_color(_label_wifi, Theme::textMuted(), 0);

    _label_lora = lv_label_create(_header);
    lv_label_set_text(_label_lora, LV_SYMBOL_CALL"--");  // Antenna-like symbol
    lv_obj_align(_label_lora, LV_ALIGN_LEFT_MID, 101, 0);
    lv_obj_set_style_text_color(_label_lora, Theme::textMuted(), 0);

    _label_gps = lv_label_create(_header);
    lv_label_set_text(_label_gps, LV_SYMBOL_GPS " --");
    lv_obj_align(_label_gps, LV_ALIGN_LEFT_MID, 142, 0);
    lv_obj_set_style_text_color(_label_gps, Theme::textMuted(), 0);

    // BLE status: Bluetooth icon with central|peripheral counts
    _label_ble = lv_label_create(_header);
    lv_label_set_text(_label_ble, LV_SYMBOL_BLUETOOTH " -|-");
    lv_obj_align(_label_ble, LV_ALIGN_LEFT_MID, 179, 0);
    lv_obj_set_style_text_color(_label_ble, Theme::textMuted(), 0);

    // Battery: vertical layout (icon on top, percentage below) to save horizontal space
    _battery_container = lv_obj_create(_header);
    lv_obj_set_size(_battery_container, 30, 34);
    lv_obj_align(_battery_container, LV_ALIGN_LEFT_MID, 219, 0);
    lv_obj_set_style_bg_opa(_battery_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_battery_container, 0, 0);
    lv_obj_set_style_pad_all(_battery_container, 0, 0);
    lv_obj_clear_flag(_battery_container, LV_OBJ_FLAG_SCROLLABLE);

    _label_battery_icon = lv_label_create(_battery_container);
    lv_label_set_text(_label_battery_icon, LV_SYMBOL_BATTERY_FULL);
    lv_obj_align(_label_battery_icon, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_color(_label_battery_icon, Theme::textMuted(), 0);

    _label_battery_pct = lv_label_create(_battery_container);
    lv_label_set_text(_label_battery_pct, "--%");
    lv_obj_align(_label_battery_pct, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_color(_label_battery_pct, Theme::textMuted(), 0);
    lv_obj_set_style_text_font(_label_battery_pct, &lv_font_montserrat_12, 0);

    // Sync button (right corner) - syncs messages from propagation node
    _btn_new = lv_btn_create(_header);
    lv_obj_set_size(_btn_new, 55, 28);
    lv_obj_align(_btn_new, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(_btn_new, Theme::primary(), 0);
    lv_obj_set_style_bg_color(_btn_new, Theme::primaryPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btn_new, on_sync_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* label_sync = lv_label_create(_btn_new);
    lv_label_set_text(label_sync, LV_SYMBOL_REFRESH);
    lv_obj_center(label_sync);
    lv_obj_set_style_text_color(label_sync, Theme::textPrimary(), 0);
}

void ConversationListScreen::create_list() {
    _list = lv_obj_create(_screen);
    lv_obj_set_size(_list, LV_PCT(100), 168);  // 240 - 36 (header) - 36 (bottom nav)
    lv_obj_align(_list, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_pad_all(_list, 2, 0);
    lv_obj_set_style_pad_gap(_list, 2, 0);
    lv_obj_set_style_bg_color(_list, Theme::surface(), 0);
    lv_obj_set_style_border_width(_list, 0, 0);
    lv_obj_set_style_radius(_list, 0, 0);
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

void ConversationListScreen::create_bottom_nav() {
    _bottom_nav = lv_obj_create(_screen);
    lv_obj_set_size(_bottom_nav, LV_PCT(100), 36);
    lv_obj_align(_bottom_nav, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(_bottom_nav, Theme::surfaceHeader(), 0);
    lv_obj_set_style_border_width(_bottom_nav, 0, 0);
    lv_obj_set_style_radius(_bottom_nav, 0, 0);
    lv_obj_set_style_pad_all(_bottom_nav, 0, 0);
    lv_obj_set_flex_flow(_bottom_nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_bottom_nav, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Bottom navigation buttons: Messages, Announces, Status, Settings
    const char* icons[] = {LV_SYMBOL_ENVELOPE, LV_SYMBOL_BELL, LV_SYMBOL_BARS, LV_SYMBOL_SETTINGS};

    for (int i = 0; i < 4; i++) {
        lv_obj_t* btn = lv_btn_create(_bottom_nav);
        lv_obj_set_size(btn, 65, 28);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_set_style_bg_color(btn, Theme::surfaceInput(), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x3a3a3a), LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, on_bottom_nav_clicked, LV_EVENT_CLICKED, this);

        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, icons[i]);
        lv_obj_center(label);
        lv_obj_set_style_text_color(label, Theme::textTertiary(), 0);
    }
}

void ConversationListScreen::load_conversations(::LXMF::MessageStore& store) {
    LVGL_LOCK();
    _message_store = &store;
    refresh();
}

void ConversationListScreen::refresh() {
    LVGL_LOCK();
    if (!_message_store) {
        return;
    }

    INFO("Refreshing conversation list");

    // Clear existing items (also removes from focus group when deleted)
    lv_obj_clean(_list);
    _conversations.clear();
    _conversation_containers.clear();
    _peer_hash_pool.clear();

    // Load conversations from store
    std::vector<Bytes> peer_hashes = _message_store->get_conversations();

    // Reserve capacity to avoid reallocations during population
    _peer_hash_pool.reserve(peer_hashes.size());
    _conversations.reserve(peer_hashes.size());
    _conversation_containers.reserve(peer_hashes.size());

    {
        char log_buf[48];
        snprintf(log_buf, sizeof(log_buf), "  Found %zu conversations", peer_hashes.size());
        INFO(log_buf);
    }

    for (const auto& peer_hash : peer_hashes) {
        std::vector<Bytes> messages = _message_store->get_messages_for_conversation(peer_hash);

        if (messages.empty()) {
            continue;
        }

        // Load last message for preview
        Bytes last_msg_hash = messages.back();
        ::LXMF::LXMessage last_msg = _message_store->load_message(last_msg_hash);

        // Create conversation item
        ConversationItem item;
        item.peer_hash = peer_hash;

        // Try to get display name from app_data, fall back to hash
        Bytes app_data = Identity::recall_app_data(peer_hash);
        if (app_data && app_data.size() > 0) {
            String display_name = parse_display_name(app_data);
            if (display_name.length() > 0) {
                item.peer_name = display_name;
            } else {
                item.peer_name = truncate_hash(peer_hash);
            }
        } else {
            item.peer_name = truncate_hash(peer_hash);
        }

        // Get message content for preview
        String content((const char*)last_msg.content().data(), last_msg.content().size());
        item.last_message = content.substring(0, 30);  // Truncate to 30 chars
        if (content.length() > 30) {
            item.last_message += "...";
        }

        item.timestamp = (uint32_t)last_msg.timestamp();
        item.timestamp_str = format_timestamp(item.timestamp);
        item.unread_count = 0;  // TODO: Track unread count

        _conversations.push_back(item);
        create_conversation_item(item);
    }
}

void ConversationListScreen::create_conversation_item(const ConversationItem& item) {
    // Create container for conversation item - compact 2-row layout
    lv_obj_t* container = lv_obj_create(_list);
    lv_obj_set_size(container, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(container, Theme::surfaceContainer(), 0);
    lv_obj_set_style_bg_color(container, Theme::surfaceElevated(), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(container, 1, 0);
    lv_obj_set_style_border_color(container, Theme::border(), 0);
    lv_obj_set_style_radius(container, 6, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_add_flag(container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    // Focus style for trackball navigation
    lv_obj_set_style_border_color(container, Theme::info(), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(container, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(container, Theme::surfaceElevated(), LV_STATE_FOCUSED);

    // Store peer hash in user data using pool (avoids per-item heap allocations)
    _peer_hash_pool.push_back(item.peer_hash);
    lv_obj_set_user_data(container, &_peer_hash_pool.back());
    lv_obj_add_event_cb(container, on_conversation_clicked, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(container, on_conversation_long_pressed, LV_EVENT_LONG_PRESSED, this);

    // Track container for focus group management
    _conversation_containers.push_back(container);

    // Row 1: Peer hash
    lv_obj_t* label_peer = lv_label_create(container);
    lv_label_set_text(label_peer, item.peer_name.c_str());
    lv_obj_align(label_peer, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_obj_set_style_text_color(label_peer, Theme::info(), 0);
    lv_obj_set_style_text_font(label_peer, &lv_font_montserrat_14, 0);

    // Row 2: Message preview (left) + Timestamp (right)
    lv_obj_t* label_preview = lv_label_create(container);
    lv_label_set_text(label_preview, item.last_message.c_str());
    lv_obj_align(label_preview, LV_ALIGN_BOTTOM_LEFT, 6, -4);
    lv_obj_set_style_text_color(label_preview, Theme::textTertiary(), 0);
    lv_obj_set_width(label_preview, 220);  // Limit width to leave room for timestamp
    lv_label_set_long_mode(label_preview, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_height(label_preview, 16, 0);  // Force single line

    lv_obj_t* label_time = lv_label_create(container);
    lv_label_set_text(label_time, item.timestamp_str.c_str());
    lv_obj_align(label_time, LV_ALIGN_BOTTOM_RIGHT, -6, -4);
    lv_obj_set_style_text_color(label_time, Theme::textMuted(), 0);

    // Unread count badge
    if (item.unread_count > 0) {
        lv_obj_t* badge = lv_obj_create(container);
        lv_obj_set_size(badge, 20, 20);
        lv_obj_align(badge, LV_ALIGN_BOTTOM_RIGHT, -6, -4);
        lv_obj_set_style_bg_color(badge, Theme::error(), 0);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(badge, 0, 0);
        lv_obj_set_style_pad_all(badge, 0, 0);

        lv_obj_t* label_count = lv_label_create(badge);
        lv_label_set_text_fmt(label_count, "%d", item.unread_count);
        lv_obj_center(label_count);
        lv_obj_set_style_text_color(label_count, lv_color_white(), 0);
    }
}

void ConversationListScreen::update_unread_count(const Bytes& peer_hash, uint16_t unread_count) {
    LVGL_LOCK();
    // Find conversation and update
    for (auto& conv : _conversations) {
        if (conv.peer_hash == peer_hash) {
            conv.unread_count = unread_count;
            refresh();  // Redraw list
            break;
        }
    }
}

void ConversationListScreen::set_conversation_selected_callback(ConversationSelectedCallback callback) {
    _conversation_selected_callback = callback;
}

void ConversationListScreen::set_compose_callback(ComposeCallback callback) {
    _compose_callback = callback;
}

void ConversationListScreen::set_sync_callback(SyncCallback callback) {
    _sync_callback = callback;
}

void ConversationListScreen::set_settings_callback(SettingsCallback callback) {
    _settings_callback = callback;
}

void ConversationListScreen::set_announces_callback(AnnouncesCallback callback) {
    _announces_callback = callback;
}

void ConversationListScreen::set_status_callback(StatusCallback callback) {
    _status_callback = callback;
}

void ConversationListScreen::show() {
    LVGL_LOCK();
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_screen);  // Bring to front for touch events

    // Add widgets to focus group for trackball navigation
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        // Add conversation containers first (so they come before New button in nav order)
        for (lv_obj_t* container : _conversation_containers) {
            lv_group_add_obj(group, container);
        }

        // Add New button last
        if (_btn_new) {
            lv_group_add_obj(group, _btn_new);
        }

        // Focus first conversation if available, otherwise New button
        if (!_conversation_containers.empty()) {
            lv_group_focus_obj(_conversation_containers[0]);
        } else if (_btn_new) {
            lv_group_focus_obj(_btn_new);
        }
    }
}

void ConversationListScreen::hide() {
    LVGL_LOCK();
    // Remove from focus group when hiding
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        // Remove conversation containers
        for (lv_obj_t* container : _conversation_containers) {
            lv_group_remove_obj(container);
        }

        if (_btn_new) {
            lv_group_remove_obj(_btn_new);
        }
    }

    lv_obj_add_flag(_screen, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* ConversationListScreen::get_object() {
    return _screen;
}

void ConversationListScreen::update_status() {
    LVGL_LOCK();
    // Update WiFi RSSI
    if (WiFi.status() == WL_CONNECTED) {
        int rssi = WiFi.RSSI();
        char wifi_text[32];
        snprintf(wifi_text, sizeof(wifi_text), "%s %d", LV_SYMBOL_WIFI, rssi);
        lv_label_set_text(_label_wifi, wifi_text);

        // Color based on signal strength
        if (rssi > -50) {
            lv_obj_set_style_text_color(_label_wifi, Theme::success(), 0);  // Green
        } else if (rssi > -70) {
            lv_obj_set_style_text_color(_label_wifi, Theme::warning(), 0);  // Yellow
        } else {
            lv_obj_set_style_text_color(_label_wifi, Theme::error(), 0);  // Red
        }
    } else {
        lv_label_set_text(_label_wifi, LV_SYMBOL_WIFI " --");
        lv_obj_set_style_text_color(_label_wifi, Theme::textMuted(), 0);
    }

    // Update LoRa RSSI
    if (_lora_interface) {
        float rssi_f = _lora_interface->get_rssi();
        int rssi = (int)rssi_f;

        // Only show RSSI if we've received at least one packet (RSSI != 0)
        if (rssi_f != 0.0f) {
            char lora_text[32];
            snprintf(lora_text, sizeof(lora_text), "%s%d", LV_SYMBOL_CALL, rssi);
            lv_label_set_text(_label_lora, lora_text);

            // Color based on signal strength (LoRa typically has weaker signals)
            if (rssi > -80) {
                lv_obj_set_style_text_color(_label_lora, Theme::success(), 0);  // Green
            } else if (rssi > -100) {
                lv_obj_set_style_text_color(_label_lora, Theme::warning(), 0);  // Yellow
            } else {
                lv_obj_set_style_text_color(_label_lora, Theme::error(), 0);  // Red
            }
        } else {
            // RSSI of 0 means no recent packet
            lv_label_set_text(_label_lora, LV_SYMBOL_CALL"--");
            lv_obj_set_style_text_color(_label_lora, Theme::textMuted(), 0);
        }
    } else {
        lv_label_set_text(_label_lora, LV_SYMBOL_CALL"--");
        lv_obj_set_style_text_color(_label_lora, Theme::textMuted(), 0);
    }

    // Update GPS satellite count
    if (_gps && _gps->satellites.isValid()) {
        int sats = _gps->satellites.value();
        char gps_text[32];
        snprintf(gps_text, sizeof(gps_text), "%s %d", LV_SYMBOL_GPS, sats);
        lv_label_set_text(_label_gps, gps_text);

        // Color based on satellite count
        if (sats >= 6) {
            lv_obj_set_style_text_color(_label_gps, Theme::success(), 0);  // Green
        } else if (sats >= 3) {
            lv_obj_set_style_text_color(_label_gps, Theme::warning(), 0);  // Yellow
        } else {
            lv_obj_set_style_text_color(_label_gps, Theme::error(), 0);  // Red
        }
    } else {
        lv_label_set_text(_label_gps, LV_SYMBOL_GPS " --");
        lv_obj_set_style_text_color(_label_gps, Theme::textMuted(), 0);
    }

    // Update BLE connection counts (central|peripheral)
    if (_ble_interface) {
        int central_count = 0;
        int peripheral_count = 0;

        // Get connection counts from BLE interface
        // The interface stores stats about connections
        // Use get_stats() map if available, otherwise show "--"
        auto stats = _ble_interface->get_stats();
        auto it_c = stats.find("central_connections");
        auto it_p = stats.find("peripheral_connections");
        if (it_c != stats.end()) central_count = (int)it_c->second;
        if (it_p != stats.end()) peripheral_count = (int)it_p->second;

        char ble_text[32];
        snprintf(ble_text, sizeof(ble_text), "%s %d|%d", LV_SYMBOL_BLUETOOTH, central_count, peripheral_count);
        lv_label_set_text(_label_ble, ble_text);

        // Color based on connection status
        int total = central_count + peripheral_count;
        if (total > 0) {
            lv_obj_set_style_text_color(_label_ble, Theme::bluetooth(), 0);  // Blue - connected
        } else {
            lv_obj_set_style_text_color(_label_ble, Theme::textMuted(), 0);  // Gray - no connections
        }
    } else {
        lv_label_set_text(_label_ble, LV_SYMBOL_BLUETOOTH " -|-");
        lv_obj_set_style_text_color(_label_ble, Theme::textMuted(), 0);
    }

    // Update battery level (read from ADC) - vertical layout
    // ESP32 ADC has linearity/offset issues - add 0.32V calibration per LilyGo community
    int raw_adc = analogRead(Pin::BATTERY_ADC);
    float voltage = (raw_adc / 4095.0) * 3.3 * Power::BATTERY_VOLTAGE_DIVIDER + 0.32;
    int percent = (int)((voltage - Power::BATTERY_EMPTY) / (Power::BATTERY_FULL - Power::BATTERY_EMPTY) * 100);
    percent = constrain(percent, 0, 100);

    // Detect charging: voltage > 4.4V indicates USB power connected
    // (calibrated voltage reads ~5V+ when charging, ~4.2V max on battery)
    bool charging = (voltage > 4.4);

    // Update icon and percentage display
    if (charging) {
        // When charging: show charge icon centered, hide percentage (voltage doesn't reflect battery state)
        lv_label_set_text(_label_battery_icon, LV_SYMBOL_CHARGE);
        lv_obj_align(_label_battery_icon, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(_label_battery_pct, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(_label_battery_icon, Theme::charging(), 0);  // Cyan
    } else {
        // When on battery: show icon at top with percentage below
        lv_label_set_text(_label_battery_icon, LV_SYMBOL_BATTERY_FULL);
        lv_obj_align(_label_battery_icon, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_clear_flag(_label_battery_pct, LV_OBJ_FLAG_HIDDEN);
        char pct_text[16];
        snprintf(pct_text, sizeof(pct_text), "%d%%", percent);
        lv_label_set_text(_label_battery_pct, pct_text);

        // Color based on battery level
        lv_color_t battery_color;
        if (percent > 50) {
            battery_color = Theme::success();  // Green
        } else if (percent > 20) {
            battery_color = Theme::warning();  // Yellow
        } else {
            battery_color = Theme::error();  // Red
        }
        lv_obj_set_style_text_color(_label_battery_icon, battery_color, 0);
        lv_obj_set_style_text_color(_label_battery_pct, battery_color, 0);
    }
}

void ConversationListScreen::on_conversation_clicked(lv_event_t* event) {
    ConversationListScreen* screen = (ConversationListScreen*)lv_event_get_user_data(event);
    lv_obj_t* target = lv_event_get_target(event);

    Bytes* peer_hash = (Bytes*)lv_obj_get_user_data(target);

    if (peer_hash && screen->_conversation_selected_callback) {
        screen->_conversation_selected_callback(*peer_hash);
    }
}

void ConversationListScreen::on_sync_clicked(lv_event_t* event) {
    ConversationListScreen* screen = (ConversationListScreen*)lv_event_get_user_data(event);

    if (screen->_sync_callback) {
        screen->_sync_callback();
    }
}

void ConversationListScreen::on_settings_clicked(lv_event_t* event) {
    ConversationListScreen* screen = (ConversationListScreen*)lv_event_get_user_data(event);

    if (screen->_settings_callback) {
        screen->_settings_callback();
    }
}

void ConversationListScreen::on_bottom_nav_clicked(lv_event_t* event) {
    ConversationListScreen* screen = (ConversationListScreen*)lv_event_get_user_data(event);
    lv_obj_t* target = lv_event_get_target(event);
    int btn_index = (int)(intptr_t)lv_obj_get_user_data(target);

    switch (btn_index) {
        case 0: // Compose new message
            if (screen->_compose_callback) {
                screen->_compose_callback();
            }
            break;
        case 1: // Announces
            if (screen->_announces_callback) {
                screen->_announces_callback();
            }
            break;
        case 2: // Status
            if (screen->_status_callback) {
                screen->_status_callback();
            }
            break;
        case 3: // Settings
            if (screen->_settings_callback) {
                screen->_settings_callback();
            }
            break;
        default:
            break;
    }
}

void ConversationListScreen::msgbox_close_cb(lv_event_t* event) {
    lv_obj_t* mbox = lv_event_get_current_target(event);
    lv_msgbox_close(mbox);
}

void ConversationListScreen::on_conversation_long_pressed(lv_event_t* event) {
    ConversationListScreen* screen = (ConversationListScreen*)lv_event_get_user_data(event);
    lv_obj_t* target = lv_event_get_target(event);

    Bytes* peer_hash = (Bytes*)lv_obj_get_user_data(target);
    if (!peer_hash) {
        return;
    }

    // Store the hash we want to delete
    screen->_pending_delete_hash = *peer_hash;

    // Show confirmation dialog
    static const char* btns[] = {"Delete", "Cancel", ""};
    lv_obj_t* mbox = lv_msgbox_create(NULL, "Delete Conversation",
        "Delete this conversation and all messages?", btns, false);
    lv_obj_center(mbox);
    lv_obj_add_event_cb(mbox, on_delete_confirmed, LV_EVENT_VALUE_CHANGED, screen);
}

void ConversationListScreen::on_delete_confirmed(lv_event_t* event) {
    lv_obj_t* mbox = lv_event_get_current_target(event);
    ConversationListScreen* screen = (ConversationListScreen*)lv_event_get_user_data(event);
    uint16_t btn_id = lv_msgbox_get_active_btn(mbox);

    if (btn_id == 0 && screen->_message_store) {  // "Delete" button
        // Delete the conversation
        screen->_message_store->delete_conversation(screen->_pending_delete_hash);
        INFO("Deleted conversation");

        // Refresh the list
        screen->refresh();
    }

    lv_msgbox_close(mbox);
}

String ConversationListScreen::format_timestamp(uint32_t timestamp) {
    double now = Utilities::OS::time();
    double diff = now - (double)timestamp;

    if (diff < 0) {
        return "Future";  // Clock not synced or future timestamp
    } else if (diff < 60) {
        return "Just now";
    } else if (diff < 3600) {
        int mins = (int)(diff / 60);
        return String(mins) + "m ago";
    } else if (diff < 86400) {
        int hours = (int)(diff / 3600);
        return String(hours) + "h ago";
    } else if (diff < 604800) {
        int days = (int)(diff / 86400);
        return String(days) + "d ago";
    } else {
        int weeks = (int)(diff / 604800);
        return String(weeks) + "w ago";
    }
}

String ConversationListScreen::truncate_hash(const Bytes& hash) {
    return String(hash.toHex().c_str());
}

String ConversationListScreen::parse_display_name(const Bytes& app_data) {
    if (app_data.size() == 0) {
        return String();
    }

    uint8_t first_byte = app_data.data()[0];

    // Check for msgpack array format (LXMF 0.5.0+)
    // fixarray: 0x90-0x9f (array with 0-15 elements)
    // array16: 0xdc
    if ((first_byte >= 0x90 && first_byte <= 0x9f) || first_byte == 0xdc) {
        // Msgpack encoded: [display_name, stamp_cost, ...]
        MsgPack::Unpacker unpacker;
        unpacker.feed(app_data.data(), app_data.size());

        // Get array size
        MsgPack::arr_size_t arr_size;
        if (!unpacker.deserialize(arr_size)) {
            return String();
        }

        if (arr_size.size() < 1) {
            return String();
        }

        // First element is display_name (can be nil or bytes)
        if (unpacker.isNil()) {
            unpacker.unpackNil();
            return String();
        }

        MsgPack::bin_t<uint8_t> name_bin;
        if (unpacker.deserialize(name_bin)) {
            // Convert bytes to string
            return String((const char*)name_bin.data(), name_bin.size());
        }

        return String();
    } else {
        // Original format: raw UTF-8 string
        return String(app_data.toString().c_str());
    }
}

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
