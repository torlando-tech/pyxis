// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_ANNOUNCELISTSCREEN_H
#define UI_LXMF_ANNOUNCELISTSCREEN_H

#ifdef ARDUINO
#include <Arduino.h>
#include <lvgl.h>
#include <vector>
#include <functional>
#include <string>
#include "Bytes.h"

namespace UI {
namespace LXMF {

/**
 * Announce List Screen
 *
 * Shows a scrollable list of announced LXMF destinations:
 * - Destination hash (truncated)
 * - Hop count / reachability
 * - Timestamp of last announce
 * - Tap to start conversation
 *
 * Layout:
 * ┌─────────────────────────────────────┐
 * │ ← Announces                [Refresh]│ 32px header
 * ├─────────────────────────────────────┤
 * │ ┌─ a1b2c3d4...                     │
 * │ │   2 hops • 5 min ago             │
 * │ └─                                  │
 * │ ┌─ e5f6a7b8...                     │ 168px scrollable
 * │ │   Direct • Just now              │
 * │ └─                                  │
 * ├─────────────────────────────────────┤
 * │  [💬] [📋] [📡] [⚙️]                │ 32px bottom nav
 * └─────────────────────────────────────┘
 */
class AnnounceListScreen {
public:
    /**
     * Announce item data
     */
    struct AnnounceItem {
        RNS::Bytes destination_hash;
        std::string hash_display;    // Truncated hash for display
        std::string display_name;    // Display name from announce (if available)
        uint8_t hops;           // Hop count (0 = direct)
        double timestamp;       // When announced
        std::string timestamp_str;   // Human-readable time
        bool has_path;          // Whether path exists
    };

    /**
     * Callback types
     */
    using AnnounceSelectedCallback = std::function<void(const RNS::Bytes& destination_hash)>;
    using BackCallback = std::function<void()>;
    using RefreshCallback = std::function<void()>;
    using SendAnnounceCallback = std::function<void()>;

    /**
     * Create announce list screen
     * @param parent Parent LVGL object (usually lv_scr_act())
     */
    AnnounceListScreen(lv_obj_t* parent = nullptr);

    /**
     * Destructor
     */
    ~AnnounceListScreen();

    /**
     * Refresh announce list from Transport layer
     */
    void refresh();

    /**
     * Set callback for announce selection (to start conversation)
     * @param callback Function to call when announce is selected
     */
    void set_announce_selected_callback(AnnounceSelectedCallback callback);

    /**
     * Set callback for back button
     * @param callback Function to call when back is pressed
     */
    void set_back_callback(BackCallback callback);

    /**
     * Set callback for send announce button
     * @param callback Function to call when announce button is pressed
     */
    void set_send_announce_callback(SendAnnounceCallback callback);

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
    lv_obj_t* _list;
    lv_obj_t* _btn_back;
    lv_obj_t* _btn_refresh;
    lv_obj_t* _btn_announce;
    lv_obj_t* _empty_label;

    std::vector<AnnounceItem> _announces;
    std::vector<lv_obj_t*> _announce_containers;  // For focus group management
    std::vector<RNS::Bytes> _dest_hash_pool;  // Object pool to avoid per-item allocations

    AnnounceSelectedCallback _announce_selected_callback;
    BackCallback _back_callback;
    SendAnnounceCallback _send_announce_callback;

    // UI construction
    void create_header();
    void create_list();
    void create_announce_item(const AnnounceItem& item);
    void show_empty_state();

    // Event handlers
    static void on_announce_clicked(lv_event_t* event);
    static void on_back_clicked(lv_event_t* event);
    static void on_refresh_clicked(lv_event_t* event);
    static void on_send_announce_clicked(lv_event_t* event);

    // Utility
    static std::string format_timestamp(double timestamp);
    static std::string format_hops(uint8_t hops);
    static std::string truncate_hash(const RNS::Bytes& hash);
    static std::string parse_display_name(const RNS::Bytes& app_data);
};

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
#endif // UI_LXMF_ANNOUNCELISTSCREEN_H
