// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_SETTINGSSCREEN_H
#define UI_LXMF_SETTINGSSCREEN_H

#ifdef ARDUINO
#include <Arduino.h>
#include <lvgl.h>
#include <Preferences.h>
#include <functional>
#include <atomic>

namespace UI {
namespace LXMF {

/**
 * Application settings structure
 */
struct AppSettings {
    // Network
    String wifi_ssid;
    String wifi_password;
    String tcp_host;
    uint16_t tcp_port;

    // Identity
    String display_name;

    // Display
    uint8_t brightness;
    uint16_t screen_timeout;  // seconds, 0 = never
    bool keyboard_light;      // Enable keyboard backlight on keypress

    // Notifications
    bool notification_sound;  // Play sound on message received
    uint8_t notification_volume;  // Volume 0-100

    // Interfaces
    bool tcp_enabled;
    bool lora_enabled;
    float lora_frequency;     // MHz
    float lora_bandwidth;     // kHz
    uint8_t lora_sf;          // Spreading factor (5-12)
    uint8_t lora_cr;          // Coding rate (5-8)
    int8_t lora_power;        // TX power dBm (2-22)
    bool auto_enabled;        // Enable AutoInterface (WiFi peer discovery)
    bool ble_enabled;         // Enable BLE mesh interface

    // Advanced
    uint32_t announce_interval;  // seconds (UI shows minutes; default 14400 = 4h)
    uint32_t sync_interval;      // seconds (0 = disabled; UI shows hours; default 14400 = 4h)
    bool gps_time_sync;
    bool transport_enabled;     // Route traffic for other nodes; default off, requires reboot

    // Propagation
    bool prop_auto_select;          // Auto-select best propagation node
    String prop_selected_node;      // Hex string of selected node hash
    bool prop_fallback_enabled;     // Fall back to propagation on direct failure
    bool prop_only;                 // Only send via propagation (no direct/opportunistic)

    // Defaults
    AppSettings() :
        tcp_host("sideband.connect.reticulum.network"),
        tcp_port(4965),
        brightness(180),
        screen_timeout(60),
        keyboard_light(false),
        notification_sound(true),
        notification_volume(10),
        tcp_enabled(true),
        lora_enabled(false),
        lora_frequency(927.25f),
        lora_bandwidth(62.5f),
        lora_sf(7),
        lora_cr(5),
        lora_power(17),
        auto_enabled(false),
        ble_enabled(false),
        announce_interval(14400),
        sync_interval(14400),
        gps_time_sync(true),
        transport_enabled(false),
        prop_auto_select(true),
        prop_selected_node(""),
        prop_fallback_enabled(true),
        prop_only(false)
    {}
};

/**
 * Settings Screen
 *
 * Hub-and-spoke layout (Columba-inspired): the top level is a list of
 * navigation cards; tapping a card opens a dedicated sub-view holding
 * that area's controls. Live status readouts live on the Status screen
 * (Route::STATUS); the Status card links there.
 *
 * Cards (order mirrors Columba; Transport Mode must stay last):
 *   Status, Network, Identity, Radio, Delivery, Appearance, Advanced,
 *   Transport
 *
 * Save model: simple controls apply immediately; a Save button appears
 * only on the form sub-views (Network, Radio, Identity) where a
 * multi-field value must commit as one unit.
 */
class SettingsScreen {
public:
    // Callback types
    using BackCallback = std::function<void()>;
    /** Return true only after the runtime snapshot was fully applied. */
    using SaveCallback = std::function<bool(const AppSettings&)>;
    using WifiReconnectCallback = std::function<void(const String&, const String&)>;
    using BrightnessChangeCallback = std::function<void(uint8_t)>;
    using PropagationNodesCallback = std::function<void()>;
    using StatusScreenCallback = std::function<void()>;
    using IdentityScreenCallback = std::function<void()>;

    /**
     * Create settings screen
     * @param parent Parent LVGL object
     */
    SettingsScreen(lv_obj_t* parent = nullptr);

    /**
     * Destructor
     */
    ~SettingsScreen();

    /**
     * Load settings from NVS
     */
    void load_settings();

    /** Capture a save request from LVGL without persistence or network I/O. */
    void save_settings();

    /** Persist and apply one pending snapshot from the main owner loop. */
    void service_pending_save();

    /**
     * Get current settings
     */
    const AppSettings& get_settings() const { return _settings; }

    /** Set the exact running firmware build for post-reboot verification. */
    void set_firmware_version(const String& version);

    /**
     * Set callback for back button (leaves Settings entirely; when a
     * sub-view is open, the back button first returns to the hub)
     */
    void set_back_callback(BackCallback callback);

    /**
     * Set callback for save button
     */
    void set_save_callback(SaveCallback callback);

    /**
     * Set callback for WiFi reconnect button
     */
    void set_wifi_reconnect_callback(WifiReconnectCallback callback);

    /**
     * Set callback for brightness changes (immediate)
     */
    void set_brightness_change_callback(BrightnessChangeCallback callback);

    /**
     * Set callback for propagation nodes button
     */
    void set_propagation_nodes_callback(PropagationNodesCallback callback);

    /**
     * Set callback for the Status card (opens Route::STATUS)
     */
    void set_status_callback(StatusScreenCallback callback);

    /**
     * Set callback for the Identity "View Identity" row (opens Route::QR)
     */
    void set_identity_callback(IdentityScreenCallback callback);

    /**
     * Show the screen (always on the hub)
     */
    void show();

    /**
     * Hide the screen
     */
    void hide();

    /**
     * Get the root LVGL object
     */
    lv_obj_t* get_object();

private:
    enum SaveState : std::uint8_t {
        SAVE_IDLE = 0U,
        SAVE_PENDING = 1U,
        SAVE_PROCESSING = 2U,
        SAVE_APPLY_RETRY = 3U
    };

    // Sub-views; HUB is the card list, the rest are dedicated pages.
    enum View : std::uint8_t {
        VIEW_HUB = 0,
        VIEW_NETWORK,
        VIEW_IDENTITY,
        VIEW_RADIO,
        VIEW_DELIVERY,
        VIEW_APPEARANCE,
        VIEW_ADVANCED,
        VIEW_TRANSPORT,
        VIEW_COUNT
    };

    // Header (shared across hub + sub-views)
    lv_obj_t* _screen;
    lv_obj_t* _header;
    lv_obj_t* _title;
    lv_obj_t* _btn_back;
    lv_obj_t* _btn_save;

    // Hub: navigation cards
    lv_obj_t* _hub;
    lv_obj_t* _cards[VIEW_COUNT - 1];

    // Sub-view content containers (one per non-hub view)
    lv_obj_t* _pages[VIEW_COUNT];

    // Network sub-view inputs
    lv_obj_t* _ta_wifi_ssid;
    lv_obj_t* _ta_wifi_password;
    lv_obj_t* _ta_tcp_host;
    lv_obj_t* _ta_tcp_port;
    lv_obj_t* _btn_reconnect;

    // Identity sub-view
    lv_obj_t* _ta_display_name;
    lv_obj_t* _btn_view_identity;

    // Appearance sub-view (display + notifications)
    lv_obj_t* _slider_brightness;
    lv_obj_t* _label_brightness_value;
    lv_obj_t* _switch_kb_light;
    lv_obj_t* _dropdown_timeout;
    lv_obj_t* _switch_notification_sound;
    lv_obj_t* _slider_notification_volume;
    lv_obj_t* _label_notification_volume_value;

    // Radio sub-view (LoRa)
    lv_obj_t* _switch_lora_enabled;
    lv_obj_t* _ta_lora_frequency;
    lv_obj_t* _dropdown_lora_bandwidth;
    lv_obj_t* _dropdown_lora_sf;
    lv_obj_t* _dropdown_lora_cr;
    lv_obj_t* _slider_lora_power;
    lv_obj_t* _label_lora_power_value;
    lv_obj_t* _lora_params_container;  // Container for LoRa params (shown/hidden based on enabled)

    // Interface row on the Network sub-view (TCP / Auto / BLE / LoRa)
    lv_obj_t* _switch_tcp_enabled;
    lv_obj_t* _switch_auto_enabled;
    lv_obj_t* _switch_ble_enabled;
    lv_obj_t* _switch_lora_interface;

    // Advanced sub-view
    lv_obj_t* _ta_announce_interval;
    lv_obj_t* _ta_sync_interval;
    lv_obj_t* _switch_gps_sync;

    // Dangerous transport-mode sub-view (Transport must remain the last card)
    lv_obj_t* _switch_transport_enabled;
    lv_obj_t* _transport_warning_modal;
    lv_group_t* _transport_modal_group;
    bool _transport_enable_confirmed;

    // Delivery/Propagation sub-view
    lv_obj_t* _btn_propagation_nodes;
    lv_obj_t* _switch_prop_fallback;
    lv_obj_t* _switch_prop_only;

    // Data
    AppSettings _settings;
    AppSettings _pending_save_settings;
    std::atomic<std::uint8_t> _save_state; // 0 idle, 1 pending, 2 processing
    std::uint32_t _apply_retry_at_ms;
    View _view;

    // Callbacks
    BackCallback _back_callback;
    SaveCallback _save_callback;
    WifiReconnectCallback _wifi_reconnect_callback;
    BrightnessChangeCallback _brightness_change_callback;
    PropagationNodesCallback _propagation_nodes_callback;
    StatusScreenCallback _status_callback;
    IdentityScreenCallback _identity_callback;

    // UI construction
    void create_header();
    void create_content();
    lv_obj_t* create_page(View view);
    lv_obj_t* create_card(lv_obj_t* parent, const char* symbol, const char* title,
                          const char* detail, View target);
    void create_identity_view(lv_obj_t* parent);
    void create_network_view(lv_obj_t* parent);
    void create_radio_view(lv_obj_t* parent);
    void create_delivery_view(lv_obj_t* parent);
    void create_appearance_view(lv_obj_t* parent);
    void create_advanced_view(lv_obj_t* parent);
    void create_transport_mode_view(lv_obj_t* parent);

    // View switching
    void ensure_view_built(View view);
    void switch_view(View view);
    void focus_group_for(View view);
    static const char* view_title(View view);

    // Helpers
    lv_obj_t* create_label_row(lv_obj_t* parent, const char* label);
    lv_obj_t* create_text_input(lv_obj_t* parent, const char* placeholder,
                                 bool password = false, int max_len = 64);

    // Update UI from settings
    void update_ui_from_settings();
    void update_settings_from_ui();

    // Event handlers
    static void on_back_clicked(lv_event_t* event);
    static void on_save_clicked(lv_event_t* event);
    static void on_card_clicked(lv_event_t* event);
    static void on_view_identity_clicked(lv_event_t* event);
    static void on_reconnect_clicked(lv_event_t* event);
    static void on_brightness_changed(lv_event_t* event);
    static void on_lora_enabled_changed(lv_event_t* event);
    static void on_lora_power_changed(lv_event_t* event);
    static void on_propagation_nodes_clicked(lv_event_t* event);
    static void on_kb_light_changed(lv_event_t* event);
    static void on_notif_sound_changed(lv_event_t* event);
    static void on_notification_volume_changed(lv_event_t* event);
    static void on_interface_switch_changed(lv_event_t* event);
    static void on_prop_switch_changed(lv_event_t* event);
    static void on_timeout_changed(lv_event_t* event);
    static void on_gps_sync_changed(lv_event_t* event);
    static void on_transport_enabled_changed(lv_event_t* event);
    static void on_transport_confirm_enable(lv_event_t* event);
    static void on_transport_cancel_enable(lv_event_t* event);
    void show_transport_warning();
    void close_transport_warning();
};

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
#endif // UI_LXMF_SETTINGSSCREEN_H
