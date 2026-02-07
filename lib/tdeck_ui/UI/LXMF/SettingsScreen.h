// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_SETTINGSSCREEN_H
#define UI_LXMF_SETTINGSSCREEN_H

#ifdef ARDUINO
#include <Arduino.h>
#include <lvgl.h>
#include <Preferences.h>
#include <functional>
#include "Bytes.h"
#include "Identity.h"

// Forward declaration
class TinyGPSPlus;

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
    uint8_t lora_sf;          // Spreading factor (7-12)
    uint8_t lora_cr;          // Coding rate (5-8)
    int8_t lora_power;        // TX power dBm (2-22)
    bool auto_enabled;        // Enable AutoInterface (WiFi peer discovery)
    bool ble_enabled;         // Enable BLE mesh interface

    // Advanced
    uint32_t announce_interval;  // seconds
    uint32_t sync_interval;      // seconds (0 = disabled, default 3600 = hourly)
    bool gps_time_sync;

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
        announce_interval(60),
        sync_interval(3600),
        gps_time_sync(true),
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
 * Also shows GPS status and system info.
 *
 * Layout:
 * +---------------------------------------+
 * | [<]  Settings                 [Save] | 36px
 * +---------------------------------------+
 * | == Network ==                        |
 * |   WiFi SSID: [__________________]    |
 * |   Password:  [******************]    |
 * |   TCP Server: [_________________]    |
 * |   TCP Port: [____]    [Reconnect]    |
 * |                                      |
 * | == Identity ==                       |
 * |   Display Name: [_______________]    |
 * |                                      |
 * | == Display ==                        |
 * |   Brightness: [=======o------] 180   |
 * |   Timeout: [1 min        v]          |
 * |                                      |
 * | == GPS Status ==                     |
 * |   Satellites: 8                      |
 * |   Location: 40.7128, -74.0060        |
 * |   Altitude: 10.5m                    |
 * |   HDOP: 1.2 (Excellent)              |
 * |                                      |
 * | == System Info ==                    |
 * |   Identity: a1b2c3d4e5f6...          |
 * |   LXMF: f7e8d9c0b1a2...              |
 * |   Firmware: v1.0.0                   |
 * |   Storage: 1.2 MB free               |
 * |   RAM: 145 KB free                   |
 * |                                      |
 * | == Advanced ==                       |
 * |   Announce: [60] seconds             |
 * |   GPS Sync: [ON]                     |
 * +---------------------------------------+
 */
class SettingsScreen {
public:
    // Callback types
    using BackCallback = std::function<void()>;
    using SaveCallback = std::function<void(const AppSettings&)>;
    using WifiReconnectCallback = std::function<void(const String&, const String&)>;
    using BrightnessChangeCallback = std::function<void(uint8_t)>;
    using PropagationNodesCallback = std::function<void()>;

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

    /**
     * Save settings to NVS
     */
    void save_settings();

    /**
     * Get current settings
     */
    const AppSettings& get_settings() const { return _settings; }

    /**
     * Set identity hash for display
     */
    void set_identity_hash(const RNS::Bytes& hash);

    /**
     * Set LXMF delivery address hash
     */
    void set_lxmf_address(const RNS::Bytes& hash);

    /**
     * Set GPS pointer for status display
     */
    void set_gps(TinyGPSPlus* gps);

    /**
     * Refresh GPS and system info displays
     */
    void refresh();

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

    // GPS status labels (read-only)
    lv_obj_t* _label_gps_sats;
    lv_obj_t* _label_gps_coords;
    lv_obj_t* _label_gps_alt;
    lv_obj_t* _label_gps_hdop;

    // System info labels (read-only)
    lv_obj_t* _label_identity_hash;
    lv_obj_t* _label_lxmf_address;
    lv_obj_t* _label_firmware;
    lv_obj_t* _label_storage;
    lv_obj_t* _label_ram;

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

    // Delivery/Propagation section
    lv_obj_t* _btn_propagation_nodes;
    lv_obj_t* _switch_prop_fallback;
    lv_obj_t* _switch_prop_only;

    // Data
    AppSettings _settings;
    RNS::Bytes _identity_hash;
    RNS::Bytes _lxmf_address;
    TinyGPSPlus* _gps;

    // Callbacks
    BackCallback _back_callback;
    SaveCallback _save_callback;
    WifiReconnectCallback _wifi_reconnect_callback;
    BrightnessChangeCallback _brightness_change_callback;
    PropagationNodesCallback _propagation_nodes_callback;

    // UI construction
    void create_header();
    void create_content();
    void create_network_section(lv_obj_t* parent);
    void create_identity_section(lv_obj_t* parent);
    void create_display_section(lv_obj_t* parent);
    void create_notifications_section(lv_obj_t* parent);
    void create_interfaces_section(lv_obj_t* parent);
    void create_gps_section(lv_obj_t* parent);
    void create_system_section(lv_obj_t* parent);
    void create_advanced_section(lv_obj_t* parent);
    void create_delivery_section(lv_obj_t* parent);

    // Helpers
    lv_obj_t* create_section_header(lv_obj_t* parent, const char* title);
    lv_obj_t* create_label_row(lv_obj_t* parent, const char* label);
    lv_obj_t* create_text_input(lv_obj_t* parent, const char* placeholder,
                                 bool password = false, int max_len = 64);

    // Update UI from settings
    void update_ui_from_settings();
    void update_settings_from_ui();
    void update_gps_display();
    void update_system_info();

    // Event handlers
    static void on_back_clicked(lv_event_t* event);
    static void on_save_clicked(lv_event_t* event);
    static void on_reconnect_clicked(lv_event_t* event);
    static void on_brightness_changed(lv_event_t* event);
    static void on_lora_enabled_changed(lv_event_t* event);
    static void on_lora_power_changed(lv_event_t* event);
    static void on_propagation_nodes_clicked(lv_event_t* event);
    static void on_notification_volume_changed(lv_event_t* event);
};

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
#endif // UI_LXMF_SETTINGSSCREEN_H
