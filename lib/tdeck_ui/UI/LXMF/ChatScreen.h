// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_CHATSCREEN_H
#define UI_LXMF_CHATSCREEN_H

#ifdef ARDUINO
#include <Arduino.h>
#include <lvgl.h>
#include <vector>
#include <deque>
#include <map>
#include <functional>
#include "Bytes.h"
#include "LXMF/LXMessage.h"
#include "LXMF/MessageStore.h"

namespace UI {
namespace LXMF {

/**
 * Chat Screen
 *
 * Shows messages in a conversation with:
 * - Scrollable message list
 * - Message bubbles (incoming/outgoing styled differently)
 * - Delivery status indicators (✓ sent, ✓✓ delivered)
 * - Message input area
 * - Send button
 *
 * Layout:
 * ┌─────────────────────────────────────┐
 * │ ← Alice (a1b2c3d4...)              │ 32px Header
 * ├─────────────────────────────────────┤
 * │                      [Hey there!]   │ Outgoing (right)
 * │                      [10:23 AM ✓]   │
 * │ [How are you doing?]                │ Incoming (left)
 * │ [10:25 AM]                          │ 156px scrollable
 * │             [I'm good, thanks!]     │
 * │             [10:26 AM ✓✓]           │
 * ├─────────────────────────────────────┤
 * │ [Type message...      ]   [Send]    │ 52px Input area
 * └─────────────────────────────────────┘
 */
class ChatScreen {
public:
    /**
     * Message item data
     */
    struct MessageItem {
        RNS::Bytes message_hash;
        String content;
        char timestamp_str[16];  // "12:34 PM" format - fixed buffer to avoid fragmentation
        bool outgoing;      // true if sent by us
        bool delivered;     // true if delivery confirmed
        bool failed;        // true if delivery failed
    };

    /**
     * Callback types
     */
    using BackCallback = std::function<void()>;
    using SendMessageCallback = std::function<void(const String& content)>;

    /**
     * Create chat screen
     * @param parent Parent LVGL object (usually lv_scr_act())
     */
    ChatScreen(lv_obj_t* parent = nullptr);

    /**
     * Destructor
     */
    ~ChatScreen();

    /**
     * Load conversation with a specific peer
     * @param peer_hash Peer destination hash
     * @param store Message store to load from
     */
    void load_conversation(const RNS::Bytes& peer_hash, ::LXMF::MessageStore& store);

    /**
     * Add a new message to the chat
     * @param message LXMF message to add
     * @param outgoing true if message is outgoing
     */
    void add_message(const ::LXMF::LXMessage& message, bool outgoing);

    /**
     * Update delivery status of a message
     * @param message_hash Hash of message to update
     * @param delivered true if delivered, false if failed
     */
    void update_message_status(const RNS::Bytes& message_hash, bool delivered);

    /**
     * Refresh message list (reload from store)
     */
    void refresh();

    /**
     * Set callback for back button
     * @param callback Function to call when back button is pressed
     */
    void set_back_callback(BackCallback callback);

    /**
     * Set callback for sending messages
     * @param callback Function to call when send button is pressed
     */
    void set_send_message_callback(SendMessageCallback callback);

    /**
     * Show the screen
     */
    void show();

    /**
     * Hide the screen
     */
    void hide();

    /**
     * Get the root LVGL object
     * @return Root object
     */
    lv_obj_t* get_object();

private:
    lv_obj_t* _screen;
    lv_obj_t* _header;
    lv_obj_t* _message_list;
    lv_obj_t* _input_area;
    lv_obj_t* _text_area;
    lv_obj_t* _btn_send;
    lv_obj_t* _btn_back;

    RNS::Bytes _peer_hash;
    ::LXMF::MessageStore* _message_store;
    std::deque<MessageItem> _messages;

    // Map message hash to bubble row for targeted updates
    std::map<RNS::Bytes, lv_obj_t*> _message_rows;

    BackCallback _back_callback;
    SendMessageCallback _send_message_callback;

    // UI construction
    void create_header();
    void create_message_list();
    void create_input_area();
    void create_message_bubble(const MessageItem& item);

    // Event handlers
    static void on_back_clicked(lv_event_t* event);
    static void on_send_clicked(lv_event_t* event);
    static void on_message_long_pressed(lv_event_t* event);
    static void on_copy_dialog_action(lv_event_t* event);
    static void on_textarea_long_pressed(lv_event_t* event);
    static void on_paste_dialog_action(lv_event_t* event);

    // Copy/paste state
    String _pending_copy_text;

    // Pagination state for infinite scroll
    std::vector<RNS::Bytes> _all_message_hashes;  // All message hashes in conversation
    size_t _display_start_idx;                     // Index into _all_message_hashes where display starts
    static constexpr size_t MESSAGES_PER_PAGE = 20;
    static constexpr size_t MAX_DISPLAYED_MESSAGES = 50;  // Cap to prevent memory exhaustion
    bool _loading_more;                            // Prevent concurrent loads

    // Load more messages (for infinite scroll)
    void load_more_messages();
    static void on_scroll(lv_event_t* event);

    // Utility
    static void format_timestamp(double timestamp, char* buf, size_t buf_size);
    static const char* get_delivery_indicator(bool outgoing, bool delivered, bool failed);
    static String parse_display_name(const RNS::Bytes& app_data);
    static void build_status_text(char* buf, size_t buf_size, const char* timestamp,
                                  bool outgoing, bool delivered, bool failed);
};

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
#endif // UI_LXMF_CHATSCREEN_H
