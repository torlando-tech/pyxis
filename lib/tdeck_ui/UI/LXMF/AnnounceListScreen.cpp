// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "AnnounceListScreen.h"
#include "Theme.h"

#ifdef ARDUINO

#include "Log.h"
#include "../LVGL/LVGLLock.h"
#include "Transport.h"
#include "Identity.h"
#include "Destination.h"
#include "Utilities/OS.h"
#include "../LVGL/LVGLInit.h"
#include <MsgPack.h>

using namespace RNS;

namespace UI {
namespace LXMF {

AnnounceListScreen::AnnounceListScreen(lv_obj_t* parent)
    : _screen(nullptr), _header(nullptr), _list(nullptr),
      _btn_back(nullptr), _btn_refresh(nullptr), _btn_announce(nullptr), _empty_label(nullptr) {
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

    // Hide by default
    hide();

    TRACE("AnnounceListScreen created");
}

AnnounceListScreen::~AnnounceListScreen() {
    LVGL_LOCK();
    if (_screen) {
        lv_obj_del(_screen);
    }
}

void AnnounceListScreen::create_header() {
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
    lv_label_set_text(title, "Announces");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 60, 0);
    lv_obj_set_style_text_color(title, Theme::textPrimary(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    // Send Announce button (green)
    _btn_announce = lv_btn_create(_header);
    lv_obj_set_size(_btn_announce, 65, 28);
    lv_obj_align(_btn_announce, LV_ALIGN_RIGHT_MID, -70, 0);
    lv_obj_set_style_bg_color(_btn_announce, Theme::primary(), 0);
    lv_obj_set_style_bg_color(_btn_announce, Theme::primaryPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btn_announce, on_send_announce_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* label_announce = lv_label_create(_btn_announce);
    lv_label_set_text(label_announce, LV_SYMBOL_BELL);  // Bell icon for announce
    lv_obj_center(label_announce);
    lv_obj_set_style_text_color(label_announce, Theme::textPrimary(), 0);

    // Refresh button
    _btn_refresh = lv_btn_create(_header);
    lv_obj_set_size(_btn_refresh, 65, 28);
    lv_obj_align(_btn_refresh, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(_btn_refresh, Theme::btnSecondary(), 0);
    lv_obj_set_style_bg_color(_btn_refresh, Theme::btnSecondaryPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btn_refresh, on_refresh_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* label_refresh = lv_label_create(_btn_refresh);
    lv_label_set_text(label_refresh, LV_SYMBOL_REFRESH);
    lv_obj_center(label_refresh);
    lv_obj_set_style_text_color(label_refresh, Theme::textPrimary(), 0);
}

void AnnounceListScreen::create_list() {
    _list = lv_obj_create(_screen);
    lv_obj_set_size(_list, LV_PCT(100), 204);  // 240 - 36 (header)
    lv_obj_align(_list, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_pad_all(_list, 4, 0);
    lv_obj_set_style_pad_gap(_list, 4, 0);
    lv_obj_set_style_bg_color(_list, Theme::surface(), 0);
    lv_obj_set_style_border_width(_list, 0, 0);
    lv_obj_set_style_radius(_list, 0, 0);
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

void AnnounceListScreen::refresh() {
    LVGL_LOCK();
    INFO("Refreshing announce list");

    // Clear existing items (also removes from focus group when deleted)
    lv_obj_clean(_list);
    _announces.clear();
    _announce_containers.clear();
    _dest_hash_pool.clear();
    _empty_label = nullptr;

    // Get destination table from Transport
    const auto& dest_table = Transport::get_destination_table();

    // Compute name_hash for lxmf.delivery to filter announces
    Bytes lxmf_delivery_name_hash = Destination::name_hash("lxmf", "delivery");

    for (auto it = dest_table.begin(); it != dest_table.end(); ++it) {
        const Bytes& dest_hash = it->first;
        const Transport::DestinationEntry& dest_entry = it->second;

        // Check if this destination has a known identity (was announced properly)
        Identity identity = Identity::recall(dest_hash);
        if (!identity) {
            continue;  // Skip destinations without known identity
        }

        // Verify this is an lxmf.delivery destination by computing expected hash
        Bytes expected_hash = Destination::hash(identity, "lxmf", "delivery");
        if (dest_hash != expected_hash) {
            continue;  // Not an lxmf.delivery destination
        }

        AnnounceItem item;
        item.destination_hash = dest_hash;
        item.hash_display = truncate_hash(dest_hash);
        item.hops = dest_entry._hops;
        item.timestamp = dest_entry._timestamp;
        item.timestamp_str = format_timestamp(dest_entry._timestamp);
        item.has_path = Transport::has_path(dest_hash);

        // Try to get display name from app_data
        Bytes app_data = Identity::recall_app_data(dest_hash);
        if (app_data && app_data.size() > 0) {
            item.display_name = parse_display_name(app_data);
        }

        _announces.push_back(item);
    }

    // Sort by timestamp (newest first)
    std::sort(_announces.begin(), _announces.end(),
        [](const AnnounceItem& a, const AnnounceItem& b) {
            return a.timestamp > b.timestamp;
        });

    {
        char log_buf[64];
        snprintf(log_buf, sizeof(log_buf), "  Found %zu announced destinations", _announces.size());
        INFO(log_buf);
    }

    if (_announces.empty()) {
        show_empty_state();
    } else {
        // Limit to 20 most recent to prevent memory exhaustion
        const size_t MAX_DISPLAY = 20;
        size_t display_count = std::min(_announces.size(), MAX_DISPLAY);

        // Reserve capacity to avoid reallocations during population
        _dest_hash_pool.reserve(display_count);
        _announce_containers.reserve(display_count);

        size_t count = 0;
        for (const auto& item : _announces) {
            if (count >= MAX_DISPLAY) break;
            create_announce_item(item);
            count++;
        }
    }
}

void AnnounceListScreen::show_empty_state() {
    _empty_label = lv_label_create(_list);
    lv_label_set_text(_empty_label, "No announces yet\n\nWaiting for LXMF\ndestinations to announce...");
    lv_obj_set_style_text_color(_empty_label, Theme::textMuted(), 0);
    lv_obj_set_style_text_align(_empty_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_empty_label, LV_ALIGN_CENTER, 0, 0);
}

void AnnounceListScreen::create_announce_item(const AnnounceItem& item) {
    // Create container for announce item - compact 2-row layout matching ConversationListScreen
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

    // Store destination hash in user data using pool (avoids per-item heap allocations)
    _dest_hash_pool.push_back(item.destination_hash);
    lv_obj_set_user_data(container, &_dest_hash_pool.back());
    lv_obj_add_event_cb(container, on_announce_clicked, LV_EVENT_CLICKED, this);

    // Track container for focus group management
    _announce_containers.push_back(container);

    // Row 1: Display name (if available) or destination hash
    lv_obj_t* label_name = lv_label_create(container);
    if (item.display_name.length() > 0) {
        lv_label_set_text(label_name, item.display_name.c_str());
    } else {
        lv_label_set_text(label_name, item.hash_display.c_str());
    }
    lv_obj_align(label_name, LV_ALIGN_TOP_LEFT, 6, 4);
    lv_obj_set_style_text_color(label_name, Theme::info(), 0);
    lv_obj_set_style_text_font(label_name, &lv_font_montserrat_14, 0);

    // Row 2: Hops info (left) + Timestamp (right)
    lv_obj_t* label_hops = lv_label_create(container);
    lv_label_set_text(label_hops, format_hops(item.hops).c_str());
    lv_obj_align(label_hops, LV_ALIGN_BOTTOM_LEFT, 6, -4);
    lv_obj_set_style_text_color(label_hops, Theme::textTertiary(), 0);

    lv_obj_t* label_time = lv_label_create(container);
    lv_label_set_text(label_time, item.timestamp_str.c_str());
    lv_obj_align(label_time, LV_ALIGN_BOTTOM_RIGHT, -6, -4);
    lv_obj_set_style_text_color(label_time, Theme::textMuted(), 0);

    // Status indicator (green dot if has path) - on row 1, right side
    if (item.has_path) {
        lv_obj_t* status_dot = lv_obj_create(container);
        lv_obj_set_size(status_dot, 8, 8);
        lv_obj_align(status_dot, LV_ALIGN_TOP_RIGHT, -6, 8);
        lv_obj_set_style_bg_color(status_dot, Theme::success(), 0);
        lv_obj_set_style_radius(status_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(status_dot, 0, 0);
        lv_obj_set_style_pad_all(status_dot, 0, 0);
    }
}

void AnnounceListScreen::set_announce_selected_callback(AnnounceSelectedCallback callback) {
    _announce_selected_callback = callback;
}

void AnnounceListScreen::set_back_callback(BackCallback callback) {
    _back_callback = callback;
}

void AnnounceListScreen::set_send_announce_callback(SendAnnounceCallback callback) {
    _send_announce_callback = callback;
}

void AnnounceListScreen::show() {
    LVGL_LOCK();
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_screen);

    // Add widgets to focus group for trackball navigation
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        // Add header buttons first
        if (_btn_back) lv_group_add_obj(group, _btn_back);
        if (_btn_announce) lv_group_add_obj(group, _btn_announce);
        if (_btn_refresh) lv_group_add_obj(group, _btn_refresh);

        // Add announce containers
        for (lv_obj_t* container : _announce_containers) {
            lv_group_add_obj(group, container);
        }

        // Focus on first announce if available, otherwise back button
        if (!_announce_containers.empty()) {
            lv_group_focus_obj(_announce_containers[0]);
        } else if (_btn_back) {
            lv_group_focus_obj(_btn_back);
        }
    }
}

void AnnounceListScreen::hide() {
    LVGL_LOCK();
    // Remove from focus group when hiding
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        if (_btn_back) lv_group_remove_obj(_btn_back);
        if (_btn_announce) lv_group_remove_obj(_btn_announce);
        if (_btn_refresh) lv_group_remove_obj(_btn_refresh);

        // Remove announce containers
        for (lv_obj_t* container : _announce_containers) {
            lv_group_remove_obj(container);
        }
    }

    lv_obj_add_flag(_screen, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* AnnounceListScreen::get_object() {
    return _screen;
}

void AnnounceListScreen::on_announce_clicked(lv_event_t* event) {
    AnnounceListScreen* screen = (AnnounceListScreen*)lv_event_get_user_data(event);
    lv_obj_t* target = lv_event_get_target(event);

    Bytes* dest_hash = (Bytes*)lv_obj_get_user_data(target);

    if (dest_hash && screen->_announce_selected_callback) {
        screen->_announce_selected_callback(*dest_hash);
    }
}

void AnnounceListScreen::on_back_clicked(lv_event_t* event) {
    AnnounceListScreen* screen = (AnnounceListScreen*)lv_event_get_user_data(event);

    if (screen->_back_callback) {
        screen->_back_callback();
    }
}

void AnnounceListScreen::on_refresh_clicked(lv_event_t* event) {
    AnnounceListScreen* screen = (AnnounceListScreen*)lv_event_get_user_data(event);
    screen->refresh();
}

void AnnounceListScreen::on_send_announce_clicked(lv_event_t* event) {
    AnnounceListScreen* screen = (AnnounceListScreen*)lv_event_get_user_data(event);
    if (screen->_send_announce_callback) {
        screen->_send_announce_callback();
    }
}

std::string AnnounceListScreen::format_timestamp(double timestamp) {
    double now = Utilities::OS::time();
    double diff = now - timestamp;
    char buf[16];

    if (diff < 60) {
        return "Just now";
    } else if (diff < 3600) {
        int mins = (int)(diff / 60);
        snprintf(buf, sizeof(buf), "%dm ago", mins);
        return buf;
    } else if (diff < 86400) {
        int hours = (int)(diff / 3600);
        snprintf(buf, sizeof(buf), "%dh ago", hours);
        return buf;
    } else {
        int days = (int)(diff / 86400);
        snprintf(buf, sizeof(buf), "%dd ago", days);
        return buf;
    }
}

std::string AnnounceListScreen::format_hops(uint8_t hops) {
    if (hops == 0) {
        return "Direct";
    } else if (hops == 1) {
        return "1 hop";
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u hops", hops);
        return buf;
    }
}

std::string AnnounceListScreen::truncate_hash(const Bytes& hash) {
    return hash.toHex();
}

std::string AnnounceListScreen::parse_display_name(const Bytes& app_data) {
    if (app_data.size() == 0) {
        return std::string();
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
            return std::string();
        }

        if (arr_size.size() < 1) {
            return std::string();
        }

        // First element is display_name (can be nil or bytes)
        if (unpacker.isNil()) {
            unpacker.unpackNil();
            return std::string();
        }

        MsgPack::bin_t<uint8_t> name_bin;
        if (unpacker.deserialize(name_bin)) {
            // Convert bytes to string
            return std::string((const char*)name_bin.data(), name_bin.size());
        }

        return std::string();
    } else {
        // Original format: raw UTF-8 string
        return app_data.toString();
    }
}

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
