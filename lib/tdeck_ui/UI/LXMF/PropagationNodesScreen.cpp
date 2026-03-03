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
#include "../TextAreaHelper.h"

using namespace RNS;

namespace UI {
namespace LXMF {

PropagationNodesScreen::PropagationNodesScreen(lv_obj_t* parent)
    : _screen(nullptr), _header(nullptr), _list(nullptr),
      _btn_back(nullptr), _btn_sync(nullptr), _auto_select_row(nullptr),
      _auto_select_checkbox(nullptr), _empty_label(nullptr),
      _search_input(nullptr), _auto_select_enabled(true) {

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
    create_search_row();
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

void PropagationNodesScreen::create_search_row() {
    lv_obj_t* row = lv_obj_create(_screen);
    lv_obj_set_size(row, LV_PCT(100), 32);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(row, Theme::surface(), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 2, 0);
    lv_obj_set_style_pad_left(row, 8, 0);
    lv_obj_set_style_pad_right(row, 8, 0);

    _search_input = lv_textarea_create(row);
    lv_obj_set_size(_search_input, LV_PCT(100), 28);
    lv_obj_align(_search_input, LV_ALIGN_CENTER, 0, 0);
    lv_textarea_set_placeholder_text(_search_input, "Filter nodes...");
    lv_textarea_set_one_line(_search_input, true);
    lv_textarea_set_max_length(_search_input, 32);
    lv_obj_set_style_bg_color(_search_input, Theme::surfaceInput(), 0);
    lv_obj_set_style_text_color(_search_input, Theme::textPrimary(), 0);
    lv_obj_set_style_border_color(_search_input, Theme::border(), 0);
    lv_obj_set_style_border_width(_search_input, 1, 0);
    lv_obj_set_style_radius(_search_input, 4, 0);
    lv_obj_set_style_text_font(_search_input, &lv_font_montserrat_12, 0);
    lv_obj_add_event_cb(_search_input, on_search_changed, LV_EVENT_VALUE_CHANGED, this);

    // Enable paste on long-press
    TextAreaHelper::enable_paste(_search_input);
}

void PropagationNodesScreen::create_auto_select_row() {
    _auto_select_row = lv_obj_create(_screen);
    lv_obj_set_size(_auto_select_row, LV_PCT(100), 36);
    lv_obj_align(_auto_select_row, LV_ALIGN_TOP_MID, 0, 68);
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
    lv_obj_set_size(_list, LV_PCT(100), 136);  // 240 - 36 (header) - 32 (search) - 36 (auto-select)
    lv_obj_align(_list, LV_ALIGN_TOP_MID, 0, 104);
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
    _nodes.clear();
    _empty_label = nullptr;

    // Get nodes from manager
    auto nodes = manager.get_nodes();

    for (const auto& node_info : nodes) {
        NodeItem item;
        item.node_hash = node_info.node_hash;
        item.name = node_info.name.c_str();
        item.hash_display = format_hash(node_info.node_hash);
        item.hops = node_info.hops;
        item.stamp_cost = node_info.stamp_cost;
        item.enabled = node_info.enabled;
        item.is_selected = (!_auto_select_enabled && node_info.node_hash == _selected_hash);

        _nodes.push_back(item);
    }

    std::string count_msg = "  Found " + std::to_string(_nodes.size()) + " propagation nodes";
    INFO(count_msg.c_str());

    apply_filter();
}

void PropagationNodesScreen::refresh() {
    // Refresh requires manager reference - caller should use load_nodes()
    DEBUG("PropagationNodesScreen::refresh() - use load_nodes() with manager reference");
}

void PropagationNodesScreen::show_empty_state() {
    _empty_label = lv_label_create(_list);
    lv_label_set_text(_empty_label, "No propagation nodes\n\nWaiting for announces\nor enter 32-char hash");
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
    } else if (item.stamp_cost > 0) {
        lv_obj_t* label_cost = lv_label_create(container);
        lv_label_set_text_fmt(label_cost, "stamp:%d", item.stamp_cost);
        lv_obj_align(label_cost, LV_ALIGN_BOTTOM_RIGHT, -4, -2);
        lv_obj_set_style_text_color(label_cost, Theme::textTertiary(), 0);
        lv_obj_set_style_text_font(label_cost, &lv_font_montserrat_12, 0);
    }
}

void PropagationNodesScreen::create_manual_entry_item(const Bytes& hash, const String& hex_display) {
    lv_obj_t* container = lv_obj_create(_list);
    lv_obj_set_size(container, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(container, lv_color_hex(0x1e1e1e), 0);
    lv_obj_set_style_bg_color(container, Theme::surfaceInput(), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(container, 1, 0);
    lv_obj_set_style_border_color(container, Theme::info(), 0);
    lv_obj_set_style_radius(container, 6, 0);
    lv_obj_set_style_pad_all(container, 4, 0);
    lv_obj_add_flag(container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(container, on_manual_node_clicked, LV_EVENT_CLICKED, this);

    // Radio indicator
    bool is_selected = (!_auto_select_enabled && hash == _selected_hash);
    lv_obj_t* radio = lv_obj_create(container);
    lv_obj_set_size(radio, 16, 16);
    lv_obj_align(radio, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_radius(radio, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(radio, 2, 0);
    lv_obj_clear_flag(radio, LV_OBJ_FLAG_CLICKABLE);
    if (is_selected) {
        lv_obj_set_style_bg_color(radio, Theme::info(), 0);
        lv_obj_set_style_border_color(radio, Theme::info(), 0);
    } else {
        lv_obj_set_style_bg_color(radio, lv_color_hex(0x1e1e1e), 0);
        lv_obj_set_style_border_color(radio, Theme::textMuted(), 0);
    }

    // Row 1: "Use manual node"
    lv_obj_t* label_name = lv_label_create(container);
    lv_label_set_text(label_name, "Use manual node");
    lv_obj_align(label_name, LV_ALIGN_TOP_LEFT, 24, 2);
    lv_obj_set_style_text_color(label_name, Theme::info(), 0);
    lv_obj_set_style_text_font(label_name, &lv_font_montserrat_14, 0);

    // Row 2: Full hex hash
    lv_obj_t* label_hash = lv_label_create(container);
    lv_label_set_text(label_hash, hex_display.c_str());
    lv_obj_align(label_hash, LV_ALIGN_BOTTOM_LEFT, 24, -2);
    lv_obj_set_style_text_color(label_hash, Theme::textMuted(), 0);
    lv_obj_set_style_text_font(label_hash, &lv_font_montserrat_12, 0);
}

void PropagationNodesScreen::on_manual_node_clicked(lv_event_t* event) {
    PropagationNodesScreen* screen = static_cast<PropagationNodesScreen*>(lv_event_get_user_data(event));

    if (screen->_manual_hash.size() == 0) return;

    INFO("Manual propagation node selected");

    // Disable auto-select
    screen->_auto_select_enabled = false;
    lv_obj_clear_state(screen->_auto_select_checkbox, LV_STATE_CHECKED);

    screen->_selected_hash = screen->_manual_hash;
    screen->update_selection_ui();

    if (screen->_auto_select_changed_callback) {
        screen->_auto_select_changed_callback(false);
    }

    if (screen->_node_selected_callback) {
        screen->_node_selected_callback(screen->_manual_hash);
    }
}

void PropagationNodesScreen::update_selection_ui() {
    apply_filter();
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

void PropagationNodesScreen::on_search_changed(lv_event_t* event) {
    PropagationNodesScreen* screen = static_cast<PropagationNodesScreen*>(lv_event_get_user_data(event));
    const char* text = lv_textarea_get_text(screen->_search_input);
    screen->_search_filter = text;
    screen->_search_filter.toLowerCase();
    screen->apply_filter();
}

void PropagationNodesScreen::apply_filter() {
    lv_obj_clean(_list);
    _empty_label = nullptr;
    _manual_hash = {};

    // Check if search is a valid 32-char hex hash with no matching discovered node
    bool show_manual = false;
    if (is_valid_hex(_search_filter)) {
        Bytes parsed_hash;
        parsed_hash.assignHex(_search_filter.c_str());
        bool found_in_nodes = false;
        for (size_t i = 0; i < _nodes.size(); i++) {
            if (_nodes[i].node_hash == parsed_hash) {
                found_in_nodes = true;
                break;
            }
        }
        if (!found_in_nodes) {
            show_manual = true;
            _manual_hash = parsed_hash;
            create_manual_entry_item(parsed_hash, _search_filter);
        }
    }

    size_t shown = 0;
    for (size_t i = 0; i < _nodes.size(); i++) {
        if (_search_filter.length() > 0) {
            String name_lower = _nodes[i].name;
            name_lower.toLowerCase();
            String hash_lower = _nodes[i].hash_display;
            hash_lower.toLowerCase();
            if (name_lower.indexOf(_search_filter) < 0 &&
                hash_lower.indexOf(_search_filter) < 0) {
                continue;
            }
        }
        _nodes[i].is_selected = (!_auto_select_enabled && _nodes[i].node_hash == _selected_hash);
        create_node_item(_nodes[i], i);
        shown++;
    }

    if (shown == 0 && !show_manual) {
        show_empty_state();
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
        if (_search_input) lv_group_add_obj(group, _search_input);
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
        if (_search_input) lv_group_remove_obj(_search_input);
        if (_btn_sync) lv_group_remove_obj(_btn_sync);
    }

    lv_obj_add_flag(_screen, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* PropagationNodesScreen::get_object() {
    return _screen;
}

// Utility functions
String PropagationNodesScreen::format_hash(const Bytes& hash) {
    if (hash.size() == 0) return "???";
    return String(hash.toHex().c_str());
}

String PropagationNodesScreen::format_hops(uint8_t hops) {
    if (hops == 0) return "direct";
    if (hops == 0xFF) return "? hops";
    return String(hops) + " hop" + (hops == 1 ? "" : "s");
}

bool PropagationNodesScreen::is_valid_hex(const String& str) {
    if (str.length() != 32) return false;
    for (size_t i = 0; i < 32; i++) {
        char c = str.charAt(i);
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
