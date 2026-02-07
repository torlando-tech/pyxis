// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_CONVERSATIONLISTSCREEN_H
#define UI_LXMF_CONVERSATIONLISTSCREEN_H

#ifdef ARDUINO
#include <Arduino.h>
#include <lvgl.h>
#include <vector>
#include <functional>
#include "Bytes.h"
#include "LXMF/MessageStore.h"
#include "Interface.h"

class TinyGPSPlus;  // Forward declaration

namespace UI {
namespace LXMF {

/**
 * Conversation List Screen
 *
 * Shows a scrollable list of all LXMF conversations with:
 * - Peer name/hash (truncated)
 * - Last message preview
 * - Timestamp
 * - Unread count indicator
 * - Navigation buttons (New message, Settings)
 *
 * Layout:
 * ┌─────────────────────────────────────┐
 * │ LXMF Messages          [New] [☰]   │ 32px header
 * ├─────────────────────────────────────┤
 * │ ┌─ Alice (a1b2c3...)              │
 * │ │   Hey, how are you?              │
 * │ │   2 hours ago          [2]       │ Unread count
 * │ └─                                  │
 * │ ┌─ Bob (d4e5f6...)                │ 176px scrollable
 * │ │   See you tomorrow!              │
 * │ │   Yesterday                      │
 * │ └─                                  │
 * ├─────────────────────────────────────┤
 * │  [💬] [👤] [📡] [⚙️]                │ 32px bottom nav
 * └─────────────────────────────────────┘
 */
class ConversationListScreen {
public:
    /**
     * Conversation item data
     */
    struct ConversationItem {
        RNS::Bytes peer_hash;
        String peer_name;      // Or truncated hash if no name
        String last_message;   // Preview of last message
        String timestamp_str;  // Human-readable time
        uint32_t timestamp;    // Unix timestamp
        uint16_t unread_count;
    };

    /**
     * Callback types
     */
    using ConversationSelectedCallback = std::function<void(const RNS::Bytes& peer_hash)>;
    using ComposeCallback = std::function<void()>;
    using SyncCallback = std::function<void()>;
    using SettingsCallback = std::function<void()>;
    using AnnouncesCallback = std::function<void()>;
    using StatusCallback = std::function<void()>;

    /**
     * Create conversation list screen
     * @param parent Parent LVGL object (usually lv_scr_act())
     */
    ConversationListScreen(lv_obj_t* parent = nullptr);

    /**
     * Destructor
     */
    ~ConversationListScreen();

    /**
     * Load conversations from message store
     * @param store Message store to load from
     */
    void load_conversations(::LXMF::MessageStore& store);

    /**
     * Refresh conversation list (reload from store)
     */
    void refresh();

    /**
     * Update unread count for a specific conversation
     * @param peer_hash Peer hash
     * @param unread_count New unread count
     */
    void update_unread_count(const RNS::Bytes& peer_hash, uint16_t unread_count);

    /**
     * Set callback for conversation selection
     * @param callback Function to call when conversation is selected
     */
    void set_conversation_selected_callback(ConversationSelectedCallback callback);

    /**
     * Set callback for compose (envelope icon in bottom nav)
     * @param callback Function to call when compose is requested
     */
    void set_compose_callback(ComposeCallback callback);

    /**
     * Set callback for sync button
     * @param callback Function to call when sync button is pressed
     */
    void set_sync_callback(SyncCallback callback);

    /**
     * Set callback for settings button
     * @param callback Function to call when settings button is pressed
     */
    void set_settings_callback(SettingsCallback callback);

    /**
     * Set callback for announces button
     * @param callback Function to call when announces button is pressed
     */
    void set_announces_callback(AnnouncesCallback callback);

    /**
     * Set callback for status button
     * @param callback Function to call when status button is pressed
     */
    void set_status_callback(StatusCallback callback);

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

    /**
     * Update status indicators (WiFi RSSI, LoRa RSSI, and battery)
     * Call periodically from main loop
     */
    void update_status();

    /**
     * Set LoRa interface for RSSI display
     * @param iface LoRa interface implementation
     */
    void set_lora_interface(RNS::Interface* iface) { _lora_interface = iface; }

    /**
     * Set BLE interface for connection count display
     * @param iface BLE interface implementation
     */
    void set_ble_interface(RNS::Interface* iface) { _ble_interface = iface; }

    /**
     * Set GPS for satellite count display
     * @param gps TinyGPSPlus instance
     */
    void set_gps(TinyGPSPlus* gps) { _gps = gps; }

private:
    lv_obj_t* _screen;
    lv_obj_t* _header;
    lv_obj_t* _list;
    lv_obj_t* _bottom_nav;
    lv_obj_t* _btn_new;
    lv_obj_t* _btn_settings;
    lv_obj_t* _label_wifi;
    lv_obj_t* _label_lora;
    lv_obj_t* _label_gps;
    lv_obj_t* _label_ble;
    lv_obj_t* _battery_container;
    lv_obj_t* _label_battery_icon;
    lv_obj_t* _label_battery_pct;

    RNS::Interface* _lora_interface;
    RNS::Interface* _ble_interface;
    TinyGPSPlus* _gps;

    ::LXMF::MessageStore* _message_store;
    std::vector<ConversationItem> _conversations;
    std::vector<lv_obj_t*> _conversation_containers;  // For focus group management
    std::vector<RNS::Bytes> _peer_hash_pool;  // Object pool to avoid per-item allocations
    RNS::Bytes _pending_delete_hash;  // Hash of conversation pending deletion

    ConversationSelectedCallback _conversation_selected_callback;
    ComposeCallback _compose_callback;
    SyncCallback _sync_callback;
    SettingsCallback _settings_callback;
    AnnouncesCallback _announces_callback;
    StatusCallback _status_callback;

    // UI construction
    void create_header();
    void create_list();
    void create_bottom_nav();
    void create_conversation_item(const ConversationItem& item);

    // Event handlers
    static void on_conversation_clicked(lv_event_t* event);
    static void on_conversation_long_pressed(lv_event_t* event);
    static void on_delete_confirmed(lv_event_t* event);
    static void on_sync_clicked(lv_event_t* event);
    static void on_settings_clicked(lv_event_t* event);
    static void on_bottom_nav_clicked(lv_event_t* event);
    static void msgbox_close_cb(lv_event_t* event);

    // Utility
    static String format_timestamp(uint32_t timestamp);
    static String truncate_hash(const RNS::Bytes& hash);
    static String parse_display_name(const RNS::Bytes& app_data);
};

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
#endif // UI_LXMF_CONVERSATIONLISTSCREEN_H
