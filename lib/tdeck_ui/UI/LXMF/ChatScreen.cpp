// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "ChatScreen.h"
#include "Theme.h"

#ifdef ARDUINO

#include "Log.h"
#include "Identity.h"
#include "../LVGL/LVGLInit.h"
#include "../LVGL/LVGLLock.h"
#include "../Clipboard.h"
#include <MsgPack.h>

using namespace RNS;

namespace UI {
namespace LXMF {

ChatScreen::ChatScreen(lv_obj_t* parent)
    : _screen(nullptr), _header(nullptr), _message_list(nullptr), _input_area(nullptr),
      _text_area(nullptr), _btn_send(nullptr), _btn_back(nullptr),
      _message_store(nullptr), _display_start_idx(0), _loading_more(false) {
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
    create_message_list();
    create_input_area();

    // Hide by default
    hide();

    TRACE("ChatScreen created");
}

ChatScreen::~ChatScreen() {
    LVGL_LOCK();
    if (_screen) {
        lv_obj_del(_screen);
    }
}

void ChatScreen::create_header() {
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
    lv_obj_align(_btn_back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(_btn_back, Theme::btnSecondary(), 0);
    lv_obj_set_style_bg_color(_btn_back, Theme::btnSecondaryPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btn_back, on_back_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* label_back = lv_label_create(_btn_back);
    lv_label_set_text(label_back, LV_SYMBOL_LEFT);
    lv_obj_center(label_back);
    lv_obj_set_style_text_color(label_back, Theme::textSecondary(), 0);

    // Peer name/hash (will be set when conversation is loaded)
    lv_obj_t* label_peer = lv_label_create(_header);
    lv_label_set_text(label_peer, "Chat");
    lv_obj_align(label_peer, LV_ALIGN_LEFT_MID, 60, 0);
    lv_obj_set_style_text_color(label_peer, Theme::textPrimary(), 0);
    lv_obj_set_style_text_font(label_peer, &lv_font_montserrat_16, 0);

}

void ChatScreen::create_message_list() {
    _message_list = lv_obj_create(_screen);
    lv_obj_set_size(_message_list, LV_PCT(100), 152);  // 240 - 36 (header) - 52 (input)
    lv_obj_align(_message_list, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_pad_all(_message_list, 4, 0);
    lv_obj_set_style_pad_gap(_message_list, 4, 0);
    lv_obj_set_style_bg_color(_message_list, lv_color_hex(0x0d0d0d), 0);  // Slightly darker
    lv_obj_set_style_border_width(_message_list, 0, 0);
    lv_obj_set_style_radius(_message_list, 0, 0);
    lv_obj_set_flex_flow(_message_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_message_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Add scroll event for infinite scroll (load more when at top)
    lv_obj_add_event_cb(_message_list, on_scroll, LV_EVENT_SCROLL_END, this);
}

void ChatScreen::create_input_area() {
    _input_area = lv_obj_create(_screen);
    lv_obj_set_size(_input_area, LV_PCT(100), 52);
    lv_obj_align(_input_area, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(_input_area, Theme::surfaceHeader(), 0);
    lv_obj_set_style_border_width(_input_area, 0, 0);
    lv_obj_set_style_radius(_input_area, 0, 0);
    lv_obj_set_style_pad_all(_input_area, 0, 0);
    lv_obj_clear_flag(_input_area, LV_OBJ_FLAG_SCROLLABLE);

    // Text area for message input
    _text_area = lv_textarea_create(_input_area);
    lv_obj_set_size(_text_area, 241, 40);
    lv_obj_align(_text_area, LV_ALIGN_LEFT_MID, 4, 0);
    lv_textarea_set_placeholder_text(_text_area, "Type message...");
    lv_textarea_set_one_line(_text_area, false);
    lv_textarea_set_max_length(_text_area, 500);
    lv_obj_set_style_bg_color(_text_area, Theme::surfaceInput(), 0);
    lv_obj_set_style_text_color(_text_area, Theme::textPrimary(), 0);
    lv_obj_set_style_border_color(_text_area, Theme::border(), 0);

    // Add long-press for paste
    lv_obj_add_event_cb(_text_area, on_textarea_long_pressed, LV_EVENT_LONG_PRESSED, this);

    // Send button
    _btn_send = lv_btn_create(_input_area);
    lv_obj_set_size(_btn_send, 67, 40);
    lv_obj_align(_btn_send, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(_btn_send, Theme::successDark(), 0);
    lv_obj_set_style_bg_color(_btn_send, Theme::successPressed(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(_btn_send, on_send_clicked, LV_EVENT_CLICKED, this);

    lv_obj_t* label_send = lv_label_create(_btn_send);
    lv_label_set_text(label_send, "Send");
    lv_obj_center(label_send);
    lv_obj_set_style_text_color(label_send, Theme::textPrimary(), 0);
}

void ChatScreen::load_conversation(const Bytes& peer_hash, ::LXMF::MessageStore& store) {
    LVGL_LOCK();
    _peer_hash = peer_hash;
    _message_store = &store;

    {
        char log_buf[64];
        snprintf(log_buf, sizeof(log_buf), "Loading conversation with peer %.8s...",
                 peer_hash.toHex().c_str());
        INFO(log_buf);
    }

    // Try to get display name from app_data
    String peer_name;
    Bytes app_data = Identity::recall_app_data(peer_hash);
    if (app_data && app_data.size() > 0) {
        peer_name = parse_display_name(app_data);
    }

    // Fall back to truncated hash if no display name
    if (peer_name.length() == 0) {
        char hash_buf[20];
        snprintf(hash_buf, sizeof(hash_buf), "%.12s...", peer_hash.toHex().c_str());
        peer_name = hash_buf;
    }

    // Update header with peer info
    lv_obj_t* label_peer = lv_obj_get_child(_header, 1);  // Second child is peer label
    lv_label_set_text(label_peer, peer_name.c_str());

    refresh();
}

void ChatScreen::refresh() {
    LVGL_LOCK();
    if (!_message_store) {
        return;
    }

    INFO("Refreshing chat messages");

    // Clear existing messages and row tracking
    lv_obj_clean(_message_list);
    _messages.clear();
    _message_rows.clear();

    // Reserve capacity for message hashes to reduce fragmentation
    _all_message_hashes.reserve(200);

    // Load all message hashes from store (sorted by timestamp)
    _all_message_hashes = _message_store->get_messages_for_conversation(_peer_hash);

    // Start displaying from the most recent messages
    if (_all_message_hashes.size() > MESSAGES_PER_PAGE) {
        _display_start_idx = _all_message_hashes.size() - MESSAGES_PER_PAGE;
    } else {
        _display_start_idx = 0;
    }

    {
        char log_buf[80];
        snprintf(log_buf, sizeof(log_buf), "  Found %zu messages, displaying last %zu",
                 _all_message_hashes.size(), _all_message_hashes.size() - _display_start_idx);
        INFO(log_buf);
    }

    for (size_t i = _display_start_idx; i < _all_message_hashes.size(); i++) {
        const auto& msg_hash = _all_message_hashes[i];

        // Use fast metadata loader (no msgpack unpacking)
        ::LXMF::MessageStore::MessageMetadata meta = _message_store->load_message_metadata(msg_hash);

        if (!meta.valid) {
            continue;
        }

        MessageItem item;
        item.message_hash = msg_hash;
        item.content = String(meta.content.c_str());
        format_timestamp(meta.timestamp, item.timestamp_str, sizeof(item.timestamp_str));
        item.outgoing = !meta.incoming;
        item.delivered = (meta.state == static_cast<int>(::LXMF::Type::Message::DELIVERED));
        item.failed = (meta.state == static_cast<int>(::LXMF::Type::Message::FAILED));

        _messages.push_back(item);
        create_message_bubble(item);
    }

    // Scroll to bottom
    lv_obj_scroll_to_y(_message_list, LV_COORD_MAX, LV_ANIM_OFF);
}

void ChatScreen::load_more_messages() {
    LVGL_LOCK();
    if (_loading_more || _display_start_idx == 0 || !_message_store) {
        return;  // Already at the beginning or already loading
    }

    _loading_more = true;
    INFO("Loading more messages...");

    // Calculate how many more to load
    size_t load_count = MESSAGES_PER_PAGE;
    if (_display_start_idx < load_count) {
        load_count = _display_start_idx;
    }
    size_t new_start_idx = _display_start_idx - load_count;

    {
        char log_buf[64];
        snprintf(log_buf, sizeof(log_buf), "  Loading messages %zu to %zu",
                 new_start_idx, _display_start_idx - 1);
        INFO(log_buf);
    }

    // Load and prepend messages directly (no temporary vector allocation)
    // Process in reverse order so push_front maintains correct sequence
    size_t items_added = 0;
    for (size_t i = _display_start_idx; i > new_start_idx; ) {
        --i;  // Decrement first since we're iterating backwards
        const auto& msg_hash = _all_message_hashes[i];

        ::LXMF::MessageStore::MessageMetadata meta = _message_store->load_message_metadata(msg_hash);
        if (!meta.valid) {
            continue;
        }

        MessageItem item;
        item.message_hash = msg_hash;
        item.content = String(meta.content.c_str());
        format_timestamp(meta.timestamp, item.timestamp_str, sizeof(item.timestamp_str));
        item.outgoing = !meta.incoming;
        item.delivered = (meta.state == static_cast<int>(::LXMF::Type::Message::DELIVERED));
        item.failed = (meta.state == static_cast<int>(::LXMF::Type::Message::FAILED));

        // Create bubble at index 0 (top of list)
        create_message_bubble(item);
        lv_obj_t* bubble_row = lv_obj_get_child(_message_list, lv_obj_get_child_cnt(_message_list) - 1);
        lv_obj_move_to_index(bubble_row, 0);

        // Prepend to deque (O(1) operation)
        _messages.push_front(item);
        items_added++;
    }
    _display_start_idx = new_start_idx;

    _loading_more = false;
    {
        char log_buf[48];
        snprintf(log_buf, sizeof(log_buf), "  Now displaying %zu messages", _messages.size());
        INFO(log_buf);
    }
}

void ChatScreen::on_scroll(lv_event_t* event) {
    ChatScreen* screen = (ChatScreen*)lv_event_get_user_data(event);

    // Check if scrolled to top
    lv_coord_t scroll_y = lv_obj_get_scroll_y(screen->_message_list);

    if (scroll_y <= 5) {  // Near top (with small threshold)
        screen->load_more_messages();
    }
}

void ChatScreen::create_message_bubble(const MessageItem& item) {
    // Create a full-width row container for alignment
    lv_obj_t* row = lv_obj_create(_message_list);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Track row for targeted status updates
    _message_rows[item.message_hash] = row;

    // Create the actual message bubble inside the row
    lv_obj_t* bubble = lv_obj_create(row);
    lv_obj_set_width(bubble, LV_PCT(80));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);

    // Style based on incoming/outgoing
    if (item.outgoing) {
        // Outgoing: purple, align right
        lv_obj_set_style_bg_color(bubble, Theme::primary(), 0);
        lv_obj_align(bubble, LV_ALIGN_RIGHT_MID, 0, 0);
    } else {
        // Incoming: gray, align left
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x424242), 0);
        lv_obj_align(bubble, LV_ALIGN_LEFT_MID, 0, 0);
    }

    lv_obj_set_style_radius(bubble, 10, 0);
    lv_obj_set_style_pad_all(bubble, 8, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    // Enable clickable for long-press detection
    lv_obj_add_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bubble, on_message_long_pressed, LV_EVENT_LONG_PRESSED, this);

    // Build status text using char buffer to avoid String fragmentation
    char status_text[32];
    build_status_text(status_text, sizeof(status_text), item.timestamp_str,
                      item.outgoing, item.delivered, item.failed);

    // Calculate text widths to decide layout
    // Bubble is 80% of 320 = 256px, minus 16px padding = 240px usable
    const lv_coord_t bubble_inner_width = 240;
    const lv_font_t* font = &lv_font_montserrat_14;
    const lv_coord_t gap = 12;  // Space between message and timestamp

    lv_coord_t msg_width = lv_txt_get_width(
        item.content.c_str(), item.content.length(), font, 0, LV_TEXT_FLAG_NONE);
    lv_coord_t status_width = lv_txt_get_width(
        status_text, strlen(status_text), font, 0, LV_TEXT_FLAG_NONE);

    // Use single-line layout if message + timestamp fit on one row
    bool single_line = (msg_width + status_width + gap) <= bubble_inner_width;

    if (single_line) {
        // Row layout: message and timestamp side by side
        lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(bubble, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        // Message content
        lv_obj_t* label_content = lv_label_create(bubble);
        lv_label_set_text(label_content, item.content.c_str());
        lv_obj_set_style_text_color(label_content, lv_color_white(), 0);

        // Timestamp on same row
        lv_obj_t* label_status = lv_label_create(bubble);
        lv_label_set_text(label_status, status_text);
        lv_obj_set_style_text_color(label_status, Theme::textTertiary(), 0);
        lv_obj_set_style_text_font(label_status, font, 0);
    } else {
        // Column layout: message above, timestamp below (for longer messages)
        lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(bubble, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        // Message content with wrapping
        lv_obj_t* label_content = lv_label_create(bubble);
        lv_label_set_text(label_content, item.content.c_str());
        lv_label_set_long_mode(label_content, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(label_content, LV_PCT(100));
        lv_obj_set_style_text_color(label_content, lv_color_white(), 0);

        // Timestamp on its own row
        lv_obj_t* label_status = lv_label_create(bubble);
        lv_label_set_text(label_status, status_text);
        lv_obj_set_width(label_status, LV_PCT(100));
        lv_obj_set_style_text_align(label_status, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(label_status, Theme::textTertiary(), 0);
        lv_obj_set_style_text_font(label_status, font, 0);
    }
}

void ChatScreen::add_message(const ::LXMF::LXMessage& message, bool outgoing) {
    LVGL_LOCK();
    MessageItem item;
    item.message_hash = message.hash();

    String content((const char*)message.content().data(), message.content().size());
    item.content = content;
    format_timestamp(message.timestamp(), item.timestamp_str, sizeof(item.timestamp_str));
    item.outgoing = outgoing;
    item.delivered = false;
    item.failed = false;

    // Remove oldest messages if we exceed the limit
    while (_messages.size() >= MAX_DISPLAYED_MESSAGES) {
        // Remove from tracking map
        _message_rows.erase(_messages.front().message_hash);
        _messages.erase(_messages.begin());
        // Remove first child (oldest) from message list
        lv_obj_t* first_row = lv_obj_get_child(_message_list, 0);
        if (first_row) {
            lv_obj_del(first_row);
        }
    }

    _messages.push_back(item);
    create_message_bubble(item);

    // Scroll to bottom
    lv_obj_scroll_to_y(_message_list, LV_COORD_MAX, LV_ANIM_ON);
}

void ChatScreen::update_message_status(const Bytes& message_hash, bool delivered) {
    LVGL_LOCK();
    // Find message and update status in our data
    for (auto& msg : _messages) {
        if (msg.message_hash == message_hash) {
            msg.delivered = delivered;
            msg.failed = !delivered;

            // Update just the status label instead of full refresh
            auto row_it = _message_rows.find(message_hash);
            if (row_it != _message_rows.end()) {
                lv_obj_t* row = row_it->second;
                // Structure: row -> bubble -> [content_label, status_label]
                lv_obj_t* bubble = lv_obj_get_child(row, 0);
                if (bubble) {
                    // Status label is always the last child
                    uint32_t child_count = lv_obj_get_child_cnt(bubble);
                    if (child_count > 0) {
                        lv_obj_t* status_label = lv_obj_get_child(bubble, child_count - 1);
                        if (status_label) {
                            char status_text[32];
                            build_status_text(status_text, sizeof(status_text), msg.timestamp_str,
                                              msg.outgoing, msg.delivered, msg.failed);
                            lv_label_set_text(status_label, status_text);
                        }
                    }
                }
            }
            break;
        }
    }
}

void ChatScreen::set_back_callback(BackCallback callback) {
    _back_callback = callback;
}

void ChatScreen::set_send_message_callback(SendMessageCallback callback) {
    _send_message_callback = callback;
}

void ChatScreen::show() {
    LVGL_LOCK();
    lv_obj_clear_flag(_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_screen);  // Bring to front for touch events

    // Add buttons to default group for trackball navigation
    // Note: text area not included since edit mode consumes arrow keys
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        if (_btn_back) lv_group_add_obj(group, _btn_back);
        if (_btn_send) lv_group_add_obj(group, _btn_send);

        // Focus on back button
        if (_btn_back) {
            lv_group_focus_obj(_btn_back);
        }
    }
}

void ChatScreen::hide() {
    LVGL_LOCK();
    // Remove from focus group when hiding
    lv_group_t* group = LVGL::LVGLInit::get_default_group();
    if (group) {
        if (_btn_back) lv_group_remove_obj(_btn_back);
        if (_btn_send) lv_group_remove_obj(_btn_send);
    }

    lv_obj_add_flag(_screen, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* ChatScreen::get_object() {
    return _screen;
}

void ChatScreen::on_back_clicked(lv_event_t* event) {
    ChatScreen* screen = (ChatScreen*)lv_event_get_user_data(event);

    if (screen->_back_callback) {
        screen->_back_callback();
    }
}

void ChatScreen::on_send_clicked(lv_event_t* event) {
    ChatScreen* screen = (ChatScreen*)lv_event_get_user_data(event);

    // Get message text
    const char* text = lv_textarea_get_text(screen->_text_area);
    String message(text);

    if (message.length() > 0 && screen->_send_message_callback) {
        screen->_send_message_callback(message);

        // Clear text area and keep focus for next message
        lv_textarea_set_text(screen->_text_area, "");
        lv_group_focus_obj(screen->_text_area);
    }
}

void ChatScreen::format_timestamp(double timestamp, char* buf, size_t buf_size) {
    // Convert to time_t for formatting
    time_t time = (time_t)timestamp;
    struct tm* timeinfo = localtime(&time);

    strftime(buf, buf_size, "%I:%M %p", timeinfo);
}

const char* ChatScreen::get_delivery_indicator(bool outgoing, bool delivered, bool failed) {
    if (!outgoing) {
        return "";  // No indicator for incoming messages
    }

    if (failed) {
        return LV_SYMBOL_CLOSE;  // X for failed
    } else if (delivered) {
        return LV_SYMBOL_OK LV_SYMBOL_OK;  // Double check for delivered
    } else {
        return LV_SYMBOL_OK;  // Single check for sent
    }
}

void ChatScreen::build_status_text(char* buf, size_t buf_size, const char* timestamp,
                                   bool outgoing, bool delivered, bool failed) {
    const char* indicator = get_delivery_indicator(outgoing, delivered, failed);
    if (indicator[0] != '\0') {
        snprintf(buf, buf_size, "%s %s", timestamp, indicator);
    } else {
        snprintf(buf, buf_size, "%s", timestamp);
    }
}

String ChatScreen::parse_display_name(const Bytes& app_data) {
    if (app_data.size() == 0) {
        return String();
    }

    // Check first byte to determine format
    uint8_t first_byte = app_data.data()[0];

    // Msgpack fixarray (0x90-0x9f) or array16 (0xdc) indicates LXMF 0.5.0+ format
    if ((first_byte >= 0x90 && first_byte <= 0x9f) || first_byte == 0xdc) {
        // Msgpack encoded: [display_name, stamp_cost]
        MsgPack::Unpacker unpacker;
        unpacker.feed(app_data.data(), app_data.size());

        // Read array header
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

        // Try to read as binary (bytes)
        MsgPack::bin_t<uint8_t> name_bin;
        if (unpacker.deserialize(name_bin)) {
            return String((const char*)name_bin.data(), name_bin.size());
        }

        return String();
    } else {
        // Legacy format: raw UTF-8 bytes
        return String(app_data.toString().c_str());
    }
}

void ChatScreen::on_message_long_pressed(lv_event_t* event) {
    ChatScreen* screen = (ChatScreen*)lv_event_get_user_data(event);
    lv_obj_t* bubble = lv_event_get_target(event);

    // Find the content label (first child of bubble)
    lv_obj_t* label = lv_obj_get_child(bubble, 0);
    if (!label) {
        return;
    }

    // Get the message text
    const char* text = lv_label_get_text(label);
    if (!text || strlen(text) == 0) {
        return;
    }

    // Store text for copy action
    screen->_pending_copy_text = String(text);

    // Show copy dialog
    static const char* btns[] = {"Copy", "Cancel", ""};
    lv_obj_t* mbox = lv_msgbox_create(NULL, "Copy Message",
        "Copy message to clipboard?", btns, false);
    lv_obj_center(mbox);
    lv_obj_add_event_cb(mbox, on_copy_dialog_action, LV_EVENT_VALUE_CHANGED, screen);
}

void ChatScreen::on_copy_dialog_action(lv_event_t* event) {
    lv_obj_t* mbox = lv_event_get_current_target(event);
    ChatScreen* screen = (ChatScreen*)lv_event_get_user_data(event);

    uint16_t btn_id = lv_msgbox_get_active_btn(mbox);

    if (btn_id == 0) {  // Copy button
        Clipboard::copy(screen->_pending_copy_text);
    }

    screen->_pending_copy_text = "";
    lv_msgbox_close(mbox);
}

void ChatScreen::on_textarea_long_pressed(lv_event_t* event) {
    ChatScreen* screen = (ChatScreen*)lv_event_get_user_data(event);

    // Only show paste if clipboard has content
    if (!Clipboard::has_content()) {
        return;
    }

    // Show paste dialog
    static const char* btns[] = {"Paste", "Cancel", ""};
    lv_obj_t* mbox = lv_msgbox_create(NULL, "Paste",
        "Paste from clipboard?", btns, false);
    lv_obj_center(mbox);
    lv_obj_add_event_cb(mbox, on_paste_dialog_action, LV_EVENT_VALUE_CHANGED, screen);
}

void ChatScreen::on_paste_dialog_action(lv_event_t* event) {
    lv_obj_t* mbox = lv_event_get_current_target(event);
    ChatScreen* screen = (ChatScreen*)lv_event_get_user_data(event);

    uint16_t btn_id = lv_msgbox_get_active_btn(mbox);

    if (btn_id == 0) {  // Paste button
        const String& content = Clipboard::paste();
        if (content.length() > 0) {
            // Insert at cursor position
            lv_textarea_add_text(screen->_text_area, content.c_str());
        }
    }

    lv_msgbox_close(mbox);
}

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
