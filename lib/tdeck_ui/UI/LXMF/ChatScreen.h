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
#include <atomic>
#include <microReticulum/Bytes.h>
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
    using CallCallback = std::function<void()>;

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
     * Stream the rest of the first page in, a few messages per call. Call from
     * UIManager::update() on the main loop after the chat screen is shown.
     */
    void tick_background_fill();

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
     * Set callback for voice call button
     * @param callback Function to call when call button is pressed
     */
    void set_call_callback(CallCallback callback);

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
    lv_obj_t* _btn_call;

    RNS::Bytes _peer_hash;
    ::LXMF::MessageStore* _message_store;
    std::deque<MessageItem> _messages;

    // Map message hash to bubble row for targeted updates
    std::map<RNS::Bytes, lv_obj_t*> _message_rows;

    BackCallback _back_callback;
    SendMessageCallback _send_message_callback;
    CallCallback _call_callback;

    // UI construction
    void create_header();
    void create_message_list();
    void create_input_area();
    void create_message_bubble(const MessageItem& item);

    // Event handlers
    static void on_back_clicked(lv_event_t* event);
    static void on_call_clicked(lv_event_t* event);
    static void on_send_clicked(lv_event_t* event);
    static void on_message_long_pressed(lv_event_t* event);
    static void on_copy_dialog_action(lv_event_t* event);
    void show_full_message(const String& content);  // detail view for a long message
    static void on_full_message_copy(lv_event_t* event);
    static void on_full_message_close(lv_event_t* event);
    static void on_textarea_long_pressed(lv_event_t* event);
    static void on_paste_dialog_action(lv_event_t* event);

    // Copy/paste state
    String _pending_copy_text;

    // Pagination state for infinite scroll
    std::vector<RNS::Bytes> _all_message_hashes;  // All message hashes in conversation
    size_t _display_start_idx;                     // Index into _all_message_hashes where display starts
    // refresh() renders only INITIAL_RENDER messages synchronously (fast open);
    // tick_background_fill() then streams the rest of the first page in,
    // BG_FILL_BATCH at a time on the main loop. This keeps the per-step
    // under-LVGL-lock work tiny — a large conversation no longer freezes the UI
    // or trips LVGLLock's 5s timeout (which previously asserted and crashed).
    static constexpr size_t INITIAL_RENDER = 3;     // newest messages shown on open
    static constexpr size_t MESSAGES_PER_PAGE = 10; // full first page (filled in background)
    static constexpr size_t BG_FILL_BATCH = 2;      // older messages streamed per tick
    static constexpr size_t MAX_DISPLAYED_MESSAGES = 50;  // Cap to prevent memory exhaustion
    // Cap the text rendered per bubble. LVGL lays out (and re-draws on scroll) a
    // wrapped label in O(length); a multi-KB message (e.g. a large bz2-delivered
    // payload) becomes a 50+ line bubble that crawls when scrolled past. The full
    // content stays stored; only the rendered text is truncated.
    static constexpr size_t MAX_DISPLAY_CHARS = 600;
    bool _loading_more;                            // Prevent concurrent loads
    // Streaming state. _bg_fill_active is atomic because on_scroll() (LVGL task)
    // and refresh() may set it while tick_background_fill() (main loop) reads it;
    // writers always set _bg_fill_target BEFORE _bg_fill_active so the target is
    // visible once active is observed true.
    std::atomic<bool> _bg_fill_active{false};      // streaming the rest of the page in
    size_t _bg_fill_target = 0;                    // _display_start_idx to fill down to

    // Load more messages (infinite scroll + background fill)
    void load_more_messages(size_t batch = MESSAGES_PER_PAGE);
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
