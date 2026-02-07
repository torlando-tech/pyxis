// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "PropagationNodesScreen.h"
#include "Theme.h"

#ifdef ARDUINO

#include "Log.h"
#include "LXMF/PropagationNodeManager.h"
#include "Utilities/OS.h"
#include "../LVGL/LVGLInit.h"
#include "../LVGL/LVGLLock.h"

using namespace RNS;

namespace UI {
namespace LXMF {

PropagationNodesScreen::PropagationNodesScreen(lv_obj_t* parent)
    : _screen(nullptr), _header(nullptr), _list(nullptr),
      _btn_back(nullptr), _btn_sync(nullptr), _auto_select_row(nullptr),
      _auto_select_checkbox(nullptr), _empty_label(nullptr),
      _auto_select_enabled(true) {

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
    create_auto_select_row();
    create_list();

    // Hide by default
    hide();

    TRACE("PropagationNodesScreen created");
}

PropagationNodesScreen::~PropagationNodesScreen() {
    LVGL_LOCK();
    if (_screen) {
        lv_obj_del(_screen);
    }
}

void PropagationNodesScreen::create_header() {
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
    lv_label_set_text(title, "Prop Nodes");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 60, 0);
    lv_obj_set_style_text_color(title, Theme::textPrimary(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    // Sync button
    _btn_sync = lv_btn_create(_header);
    lv_obj_set_size(_btn_sync, 65, 28);
    lv_obj_align(_btn_sync, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(_btn_sync, Theme::primary(), 0);
    lv_obj_set_style_bg_color(_btn_sync, Theme::primaryPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btn_sync, on_sync_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* label_sync = lv_label_create(_btn_sync);
    lv_label_set_text(label_sync, "Sync");
    lv_obj_center(label_sync);
    lv_obj_set_style_text_color(label_sync, Theme::textPrimary(), 0);
    lv_obj_set_style_text_font(label_sync, &lv_font_montserrat_12, 0);
}

void PropagationNodesScreen::create_auto_select_row() {
    _auto_select_row = lv_obj_create(_screen);
    lv_obj_set_size(_auto_select_row, LV_PCT(100), 36);
    lv_obj_align(_auto_select_row, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(_auto_select_row, lv_color_hex(0x1e1e1e), 0);
    lv_obj_set_style_border_width(_auto_select_row, 0, 0);
    lv_obj_set_style_border_side(_auto_select_row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(_auto_select_row, Theme::btnSecondary(), 0);
    lv_obj_set_style_radius(_auto_select_row, 0, 0);
    lv_obj_set_style_pad_left(_auto_select_row, 8, 0);
    lv_obj_add_flag(_auto_select_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_auto_select_row, on_auto_select_changed, LV_EVENT_CLICKED, this);

    // Checkbox
    _auto_select_checkbox = lv_checkbox_create(_auto_select_row);
    lv_checkbox_set_text(_auto_select_checkbox, "Auto-select best node");
    lv_obj_align(_auto_select_checkbox, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(_auto_select_checkbox, Theme::textSecondary(), 0);
    lv_obj_set_style_text_font(_auto_select_checkbox, &lv_font_montserrat_14, 0);

    // Style the checkbox indicator
    lv_obj_set_style_bg_color(_auto_select_checkbox, Theme::info(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(_auto_select_checkbox, Theme::textMuted(), LV_PART_INDICATOR);
    lv_obj_set_style_border_width(_auto_select_checkbox, 2, LV_PART_INDICATOR);

    // Set initial state
    if (_auto_select_enabled) {
        lv_obj_add_state(_auto_select_checkbox, LV_STATE_CHECKED);
    }
}

void PropagationNodesScreen::create_list() {
    _list = lv_obj_create(_screen);
    lv_obj_set_size(_list, LV_PCT(100), 168);  // 240 - 36 (header) - 36 (auto-select row)
    lv_obj_align(_list, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_style_pad_all(_list, 4, 0);
    lv_obj_set_style_pad_gap(_list, 4, 0);
    lv_obj_set_style_bg_color(_list, Theme::surface(), 0);
    lv_obj_set_style_border_width(_list, 0, 0);
    lv_obj_set_style_radius(_list, 0, 0);
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

void PropagationNodesScreen::load_nodes(::LXMF::PropagationNodeManager& manager,
                                        const RNS::Bytes& selected_hash,
                                        bool auto_select_enabled) {
    LVGL_LOCK();
    INFO("Loading propagation nodes");

    _selected_hash = selected_hash;
    _auto_select_enabled = auto_select_enabled;

    // Update checkbox state
    if (_auto_select_enabled) {
        lv_obj_add_state(_auto_select_checkbox, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(_auto_select_checkbox, LV_STATE_CHECKED);
    }

    // Clear existing items
    lv_obj_clean(_list);
    _nodes.clear();
    _empty_label = nullptr;

    // Get nodes from manager
    auto nodes = manager.get_nodes();

    for (const auto& node_info : nodes) {
        NodeItem item;
        item.node_hash = node_info.node_hash;
        item.name = node_info.name.c_str();
        item.hash_display = truncate_hash(node_info.node_hash);
        item.hops = node_info.hops;
        item.enabled = node_info.enabled;
        item.is_selected = (!_auto_select_enabled && node_info.node_hash == _selected_hash);

        _nodes.push_back(item);
    }

    std::string count_msg = "  Found " + std::to_string(_nodes.size()) + " propagation nodes";
    INFO(count_msg.c_str());

    if (_nodes.empty()) {
        show_empty_state();
    } else {
        for (size_t i = 0; i < _nodes.size(); i++) {
            create_node_item(_nodes[i], i);
        }
    }
}

void PropagationNodesScreen::refresh() {
    // Refresh requires manager reference - caller should use load_nodes()
    DEBUG("PropagationNodesScreen::refresh() - use load_nodes() with manager reference");
}

void PropagationNodesScreen::show_empty_state() {
    _empty_label = lv_label_create(_list);
    lv_label_set_text(_empty_label, "No propagation nodes\n\nWaiting for nodes\nto announce...");
    lv_obj_set_style_text_color(_empty_label, Theme::textMuted(), 0);
    lv_obj_set_style_text_align(_empty_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_empty_label, LV_ALIGN_CENTER, 0, 0);
}

void PropagationNodesScreen::create_node_item(const NodeItem& item, size_t index) {
    // Container for node item - 2-row layout
    lv_obj_t* container = lv_obj_create(_list);
    lv_obj_set_size(container, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x1e1e1e), 0);
    lv_obj_set_style_bg_color(container, Theme::surfaceInput(), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(container, 1, 0);
    lv_obj_set_style_border_color(container, Theme::btnSecondary(), 0);
    lv_obj_set_style_radius(container, 6, 0);
    lv_obj_set_style_pad_all(container, 4, 0);
    lv_obj_add_flag(container, LV_OBJ_FLAG_CLICKABLE);

    // Store index in user_data
    lv_obj_set_user_data(container, (void*)(uintptr_t)index);
    lv_obj_add_event_cb(container, on_node_clicked, LV_EVENT_CLICKED, this);

    // Selection indicator (radio button style)
    lv_obj_t* radio = lv_obj_create(container);
    lv_obj_set_size(radio, 16, 16);
    lv_obj_align(radio, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_radius(radio, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(radio, 2, 0);
    lv_obj_clear_flag(radio, LV_OBJ_FLAG_CLICKABLE);

    if (item.is_selected && !_auto_select_enabled) {
        lv_obj_set_style_bg_color(radio, Theme::info(), 0);
        lv_obj_set_style_border_color(radio, Theme::info(), 0);
    } else {
        lv_obj_set_style_bg_color(radio, lv_color_hex(0x1e1e1e), 0);
        lv_obj_set_style_border_color(radio, Theme::textMuted(), 0);
    }

    // Row 1: Name and hops
    lv_obj_t* label_name = lv_label_create(container);
    lv_label_set_text(label_name, item.name.c_str());
    lv_obj_align(label_name, LV_ALIGN_TOP_LEFT, 24, 2);
    lv_obj_set_style_text_color(label_name, item.enabled ? Theme::info() : Theme::textMuted(), 0);
    lv_obj_set_style_text_font(label_name, &lv_font_montserrat_14, 0);

    lv_obj_t* label_hops = lv_label_create(container);
    lv_label_set_text(label_hops, format_hops(item.hops).c_str());
    lv_obj_align(label_hops, LV_ALIGN_TOP_RIGHT, -4, 2);
    lv_obj_set_style_text_color(label_hops, Theme::textTertiary(), 0);
    lv_obj_set_style_text_font(label_hops, &lv_font_montserrat_12, 0);

    // Row 2: Hash and status
    lv_obj_t* label_hash = lv_label_create(container);
    lv_label_set_text(label_hash, item.hash_display.c_str());
    lv_obj_align(label_hash, LV_ALIGN_BOTTOM_LEFT, 24, -2);
    lv_obj_set_style_text_color(label_hash, Theme::textMuted(), 0);
    lv_obj_set_style_text_font(label_hash, &lv_font_montserrat_12, 0);

    if (!item.enabled) {
        lv_obj_t* label_status = lv_label_create(container);
        lv_label_set_text(label_status, "disabled");
        lv_obj_align(label_status, LV_ALIGN_BOTTOM_RIGHT, -4, -2);
        lv_obj_set_style_text_color(label_status, Theme::error(), 0);
        lv_obj_set_style_text_font(label_status, &lv_font_montserrat_12, 0);
    }
}

void PropagationNodesScreen::update_selection_ui() {
    // Clear and recreate list to update selection state
    lv_obj_clean(_list);
    _empty_label = nullptr;

    if (_nodes.empty()) {
        show_empty_state();
    } else {
        for (size_t i = 0; i < _nodes.size(); i++) {
            _nodes[i].is_selected = (!_auto_select_enabled && _nodes[i].node_hash == _selected_hash);
            create_node_item(_nodes[i], i);
        }
    }
}

// Event handlers
void PropagationNodesScreen::on_node_clicked(lv_event_t* event) {
    PropagationNodesScreen* screen = static_cast<PropagationNodesScreen*>(lv_event_get_user_data(event));
    lv_obj_t* target = lv_event_get_target(event);
    size_t index = (size_t)(uintptr_t)lv_obj_get_user_data(target);

    if (index < screen->_nodes.size()) {
        const NodeItem& item = screen->_nodes[index];
        std::string msg = "Selected propagation node: " + std::string(item.name.c_str());
        INFO(msg.c_str());

        // Disable auto-select when user manually selects
        screen->_auto_select_enabled = false;
        lv_obj_clear_state(screen->_auto_select_checkbox, LV_STATE_CHECKED);

        screen->_selected_hash = item.node_hash;
        screen->update_selection_ui();

        if (screen->_auto_select_changed_callback) {
            screen->_auto_select_changed_callback(false);
        }

        if (screen->_node_selected_callback) {
            screen->_node_selected_callback(item.node_hash);
        }
    }
}

void PropagationNodesScreen::on_back_clicked(lv_event_t* event) {
    PropagationNodesScreen* screen = static_cast<PropagationNodesScreen*>(lv_event_get_user_data(event));
    if (screen->_back_callback) {
        screen->_back_callback();
    }
}

void PropagationNodesScreen::on_sync_clicked(lv_event_t* event) {
    PropagationNodesScreen* screen = static_cast<PropagationNodesScreen*>(lv_event_get_user_data(event));
    if (screen->_sync_callback) {
        screen->_sync_callback();
    }
}

void PropagationNodesScreen::on_auto_select_changed(lv_event_t* event) {
    PropagationNodesScreen* screen = static_cast<PropagationNodesScreen*>(lv_event_get_user_data(event));

    screen->_auto_select_enabled = !screen->_auto_select_enabled;

    if (screen->_auto_select_enabled) {
        lv_obj_add_state(screen->_auto_select_checkbox, LV_STATE_CHECKED);
        screen->_selected_hash = {};  // Clear manual selection
    } else {
        lv_obj_clear_state(screen->_auto_select_checkbox, LV_STATE_CHECKED);
    }

    screen->update_selection_ui();

    if (screen->_auto_select_changed_callback) {
        screen->_auto_select_changed_callback(screen->_auto_select_enabled);
    }

    if (screen->_auto_select_enabled && screen->_node_selected_callback) {
        // Signal that auto-select is now active (empty hash)
        screen->_node_selected_callback({});
    }
}

// Callback setters
void PropagationNodesScreen::set_node_selected_callback(NodeSelectedCallback callback) {
    _node_selected_callback = callback;
}

void PropagationNodesScreen::set_back_callback(BackCallback callback) {
    _back_callback = callback;
}

void PropagationNodesScreen::set_sync_callback(SyncCallback callback) {
    _sync_callback = callback;
}

void PropagationNodesScreen::set_auto_select_changed_callback(AutoSelectChangedCallback callback) {
    _auto_select_changed_callback = callback;
}

// Visibility
void PropagationNodesScreen::show() {
    LVGL_LOCK();
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_screen);

    // Add buttons to focus group for trackball navigation
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        if (_btn_back) lv_group_add_obj(group, _btn_back);
        if (_btn_sync) lv_group_add_obj(group, _btn_sync);

        // Focus on back button
        if (_btn_back) {
            lv_group_focus_obj(_btn_back);
        }
    }
}

void PropagationNodesScreen::hide() {
    LVGL_LOCK();
    // Remove from focus group when hiding
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        if (_btn_back) lv_group_remove_obj(_btn_back);
        if (_btn_sync) lv_group_remove_obj(_btn_sync);
    }

    lv_obj_add_flag(_screen, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* PropagationNodesScreen::get_object() {
    return _screen;
}

// Utility functions
String PropagationNodesScreen::truncate_hash(const Bytes& hash) {
    if (hash.size() < 8) return "???";
    String hex = hash.toHex().c_str();
    return hex.substring(0, 8) + "...";
}

String PropagationNodesScreen::format_hops(uint8_t hops) {
    if (hops == 0) return "direct";
    if (hops == 0xFF) return "? hops";
    return String(hops) + " hop" + (hops == 1 ? "" : "s");
}

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
