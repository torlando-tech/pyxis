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
 * Allows configuration of WiFi, TCP server, display, and other settings.
 * Live GPS/system readouts live on the Status screen (Route::STATUS);
 * Settings links there and holds only actionable controls.
 *
 * Layout:
 * +---------------------------------------+
 * | [<]  Settings                 [Save] | 36px
 * +---------------------------------------+
 * | Status >                        (link)
 * | == General ==                      |
 * |   Display Name: [_______________]  |
 * |   Brightness: [=======o------] 180 |
 * |   Keyboard Light: [OFF]            |
 * |   Screen Timeout: [1 min v]        |
 * |                                      |
 * | == Notifications ==                 |
 * |   Message Sound: [ON]               |
 * |   Volume: [=====o--------] 10       |
 * |                                      |
 * | == Network ==                       |
 * |   WiFi SSID: [__________________]   |
 * |   Password:  [******************]   |
 * |   TCP Server: [_________________]   |
 * |   Port: [____]    [Reconnect]       |
 * |   TCP Interface: [ON]               |
 * |   Auto Discovery: [OFF]             |
 * |   BLE P2P: [OFF]                    |
 * |                                      |
 * | == Radio ==                         |
 * |   LoRa Interface: [OFF]             |
 * |   (params shown when enabled)       |
 * |                                      |
 * | == Delivery ==                      |
 * |   Propagation Nodes: [View]         |
 * |   Fallback to Prop: [ON]            |
 * |   Propagation Only: [OFF]           |
 * |                                      |
 * | == Advanced ==                      |
 * |   Announce Interval (min): [240]    |
 * |   Prop Sync Interval (hrs): [4]     |
 * |   GPS Time Sync: [ON]               |
 * |                                      |
 * | == DANGER: Transport Mode ==        |
 * |   (warning + confirm switch)        |
 * +---------------------------------------+
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
     * Set callback for back button
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
     * Set callback for the Status link (opens Route::STATUS)
     */
    void set_status_callback(StatusScreenCallback callback);

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
     */
    lv_obj_t* get_object();

private:
    enum SaveState : std::uint8_t {
        SAVE_IDLE = 0U,
        SAVE_PENDING = 1U,
        SAVE_PROCESSING = 2U,
        SAVE_APPLY_RETRY = 3U
    };
    // Main UI components
    lv_obj_t* _screen;
    lv_obj_t* _header;
    lv_obj_t* _content;
    lv_obj_t* _btn_back;
    lv_obj_t* _btn_save;

    // Network section inputs
    lv_obj_t* _ta_wifi_ssid;
    lv_obj_t* _ta_wifi_password;
    lv_obj_t* _ta_tcp_host;
    lv_obj_t* _ta_tcp_port;
    lv_obj_t* _btn_reconnect;

    // Identity section
    lv_obj_t* _ta_display_name;

    // Display section
    lv_obj_t* _slider_brightness;
    lv_obj_t* _label_brightness_value;
    lv_obj_t* _switch_kb_light;
    lv_obj_t* _dropdown_timeout;

    // Notifications section
    lv_obj_t* _switch_notification_sound;
    lv_obj_t* _slider_notification_volume;
    lv_obj_t* _label_notification_volume_value;

    // Status link row (opens the Status screen with the live readouts)
    lv_obj_t* _btn_status;

    // Interfaces section
    lv_obj_t* _switch_tcp_enabled;
    lv_obj_t* _switch_lora_enabled;
    lv_obj_t* _ta_lora_frequency;
    lv_obj_t* _dropdown_lora_bandwidth;
    lv_obj_t* _dropdown_lora_sf;
    lv_obj_t* _dropdown_lora_cr;
    lv_obj_t* _slider_lora_power;
    lv_obj_t* _label_lora_power_value;
    lv_obj_t* _lora_params_container;  // Container for LoRa params (shown/hidden based on enabled)
    lv_obj_t* _switch_auto_enabled;
    lv_obj_t* _switch_ble_enabled;

    // Advanced section
    lv_obj_t* _ta_announce_interval;
    lv_obj_t* _ta_sync_interval;
    lv_obj_t* _switch_gps_sync;

    // Dangerous transport-mode section (must remain last in Settings)
    lv_obj_t* _switch_transport_enabled;
    lv_obj_t* _transport_warning_modal;
    lv_group_t* _transport_modal_group;
    bool _transport_enable_confirmed;

    // Delivery/Propagation section
    lv_obj_t* _btn_propagation_nodes;
    lv_obj_t* _switch_prop_fallback;
    lv_obj_t* _switch_prop_only;

    // Data
    AppSettings _settings;
    AppSettings _pending_save_settings;
    std::atomic<std::uint8_t> _save_state; // 0 idle, 1 pending, 2 processing
    std::uint32_t _apply_retry_at_ms;

    // Callbacks
    BackCallback _back_callback;
    SaveCallback _save_callback;
    WifiReconnectCallback _wifi_reconnect_callback;
    BrightnessChangeCallback _brightness_change_callback;
    PropagationNodesCallback _propagation_nodes_callback;
    StatusScreenCallback _status_callback;

    // UI construction
    void create_header();
    void create_content();
    void create_general_section(lv_obj_t* parent);
    void create_notifications_section(lv_obj_t* parent);
    void create_network_section(lv_obj_t* parent);
    void create_radio_section(lv_obj_t* parent);
    void create_delivery_section(lv_obj_t* parent);
    void create_advanced_section(lv_obj_t* parent);
    void create_transport_mode_section(lv_obj_t* parent);

    // Helpers
    lv_obj_t* create_section_header(lv_obj_t* parent, const char* title);
    lv_obj_t* create_label_row(lv_obj_t* parent, const char* label);
    lv_obj_t* create_text_input(lv_obj_t* parent, const char* placeholder,
                                 bool password = false, int max_len = 64);

    // Update UI from settings
    void update_ui_from_settings();
    void update_settings_from_ui();

    // Event handlers
    static void on_back_clicked(lv_event_t* event);
    static void on_save_clicked(lv_event_t* event);
    static void on_status_clicked(lv_event_t* event);
    static void on_reconnect_clicked(lv_event_t* event);
    static void on_brightness_changed(lv_event_t* event);
    static void on_lora_enabled_changed(lv_event_t* event);
    static void on_lora_power_changed(lv_event_t* event);
    static void on_propagation_nodes_clicked(lv_event_t* event);
    static void on_notification_volume_changed(lv_event_t* event);
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
