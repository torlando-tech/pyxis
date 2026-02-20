// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_UIMANAGER_H
#define UI_LXMF_UIMANAGER_H

#ifdef ARDUINO
#include <Arduino.h>
#include <lvgl.h>
#include <functional>
#include "ConversationListScreen.h"
#include "ChatScreen.h"
#include "ComposeScreen.h"
#include "AnnounceListScreen.h"
#include "StatusScreen.h"
#include "QRScreen.h"
#include "SettingsScreen.h"
#include "PropagationNodesScreen.h"
#include "CallScreen.h"
#include "LXMF/LXMRouter.h"
#include "LXMF/PropagationNodeManager.h"
#include "LXMF/MessageStore.h"
#include "Reticulum.h"
#include "Link.h"

class LXSTAudio;

namespace UI {
namespace LXMF {

/**
 * UI Manager
 *
 * Manages all LXMF UI screens and coordinates between:
 * - UI screens (ConversationList, Chat, Compose, Call)
 * - LXMF router (message sending/receiving)
 * - Message store (persistence)
 * - Reticulum (network layer)
 * - LXST voice calls (audio pipeline + Reticulum Links)
 *
 * Responsibilities:
 * - Screen navigation
 * - Message delivery callbacks
 * - UI updates on message events
 * - Integration with LXMF router
 * - Voice call state machine
 */
class UIManager {
public:
    /**
     * Create UI manager
     * @param reticulum Reticulum instance
     * @param router LXMF router instance
     * @param store Message store instance
     */
    UIManager(RNS::Reticulum& reticulum, ::LXMF::LXMRouter& router, ::LXMF::MessageStore& store);

    /**
     * Destructor
     */
    ~UIManager();

    /**
     * Initialize UI and show conversation list
     * @return true if initialization successful
     */
    bool init();

    /**
     * Update UI (call periodically from main loop)
     * Processes pending LXMF messages, updates UI, pumps voice call
     */
    void update();

    /**
     * Show conversation list screen
     */
    void show_conversation_list();

    /**
     * Show chat screen for a specific peer
     * @param peer_hash Peer destination hash
     */
    void show_chat(const RNS::Bytes& peer_hash);

    /**
     * Show compose new message screen
     */
    void show_compose();

    /**
     * Show announce list screen
     */
    void show_announces();

    /**
     * Show status screen
     */
    void show_status();

    /**
     * Show settings screen
     */
    void show_settings();

    /**
     * Show propagation nodes screen
     */
    void show_propagation_nodes();

    /**
     * Set propagation node manager
     * @param manager Propagation node manager instance
     */
    void set_propagation_node_manager(::LXMF::PropagationNodeManager* manager);

    /**
     * Set LoRa interface for RSSI display
     * @param iface LoRa interface
     */
    void set_lora_interface(RNS::Interface* iface);

    /**
     * Set BLE interface for connection count display
     * @param iface BLE interface
     */
    void set_ble_interface(RNS::Interface* iface);

    /**
     * Set GPS for satellite count display
     * @param gps TinyGPSPlus instance
     */
    void set_gps(TinyGPSPlus* gps);

    /**
     * Get settings screen for external configuration
     */
    SettingsScreen* get_settings_screen() { return _settings_screen; }

    /**
     * Get status screen for external updates (e.g., BLE peer info)
     */
    StatusScreen* get_status_screen() { return _status_screen; }

    /**
     * Update RNS connection status displayed on status screen
     * @param connected Whether connected to RNS server
     * @param server_name Server hostname (optional)
     */
    void set_rns_status(bool connected, const String& server_name = "");

    /**
     * Announce LXST voice call destination
     * Called periodically from main loop
     */
    void announce_lxst();

    /**
     * Handle incoming LXMF message
     * Called by LXMF router delivery callback
     * @param message Received message
     */
    void on_message_received(::LXMF::LXMessage& message);

    /**
     * Handle message delivery confirmation
     * @param message Message that was delivered
     */
    void on_message_delivered(::LXMF::LXMessage& message);

    /**
     * Handle message delivery failure
     * @param message Message that failed to deliver
     */
    void on_message_failed(::LXMF::LXMessage& message);

private:
    enum Screen {
        SCREEN_CONVERSATION_LIST,
        SCREEN_CHAT,
        SCREEN_COMPOSE,
        SCREEN_ANNOUNCES,
        SCREEN_STATUS,
        SCREEN_QR,
        SCREEN_SETTINGS,
        SCREEN_PROPAGATION_NODES,
        SCREEN_CALL
    };

    RNS::Reticulum& _reticulum;
    ::LXMF::LXMRouter& _router;
    ::LXMF::MessageStore& _store;
    RNS::Destination _lxst_destination;

    Screen _current_screen;
    RNS::Bytes _current_peer_hash;

    ConversationListScreen* _conversation_list_screen;
    ChatScreen* _chat_screen;
    ComposeScreen* _compose_screen;
    AnnounceListScreen* _announce_list_screen;
    StatusScreen* _status_screen;
    QRScreen* _qr_screen;
    SettingsScreen* _settings_screen;
    PropagationNodesScreen* _propagation_nodes_screen;
    CallScreen* _call_screen;

    ::LXMF::PropagationNodeManager* _propagation_manager;
    RNS::Interface* _ble_interface;

    bool _initialized;

    // Screen navigation handlers
    void on_conversation_selected(const RNS::Bytes& peer_hash);
    void on_new_message();
    void on_back_to_conversation_list();
    void on_send_message_from_chat(const String& content);
    void on_call_from_chat();
    void on_send_message_from_compose(const RNS::Bytes& dest_hash, const String& message);
    void on_cancel_compose();
    void on_announce_selected(const RNS::Bytes& dest_hash);
    void on_back_from_announces();
    void on_back_from_status();
    void on_share_from_status();
    void on_back_from_qr();
    void on_back_from_settings();
    void on_back_from_propagation_nodes();
    void on_propagation_node_selected(const RNS::Bytes& node_hash);
    void on_propagation_auto_select_changed(bool enabled);
    void on_propagation_sync();

    // LXMF message handling
    void send_message(const RNS::Bytes& dest_hash, const String& content);

    // UI updates
    void refresh_current_screen();

    // ── LXST Voice Call ──

    // LXST signalling byte constants (matches Python LXST / LXST-kt)
    static constexpr uint8_t LXST_STATUS_BUSY        = 0x00;
    static constexpr uint8_t LXST_STATUS_REJECTED     = 0x01;
    static constexpr uint8_t LXST_STATUS_CALLING      = 0x02;
    static constexpr uint8_t LXST_STATUS_AVAILABLE    = 0x03;
    static constexpr uint8_t LXST_STATUS_RINGING      = 0x04;
    static constexpr uint8_t LXST_STATUS_CONNECTING   = 0x05;
    static constexpr uint8_t LXST_STATUS_ESTABLISHED  = 0x06;

    // LXST codec header byte
    static constexpr uint8_t LXST_CODEC_CODEC2 = 0x02;

    enum class CallState {
        IDLE,
        LINK_ESTABLISHING,  // Outgoing: waiting for Link to come up
        WAIT_AVAILABLE,     // Outgoing: link up, waiting for STATUS_AVAILABLE
        WAIT_RINGING,       // Outgoing: sent identify, waiting for STATUS_RINGING
        RINGING,            // Outgoing: remote is ringing
        INCOMING_RINGING,   // Incoming: waiting for user to answer/reject
        CONNECTING,         // Both: opening audio pipelines
        ACTIVE,             // Both: voice flowing
    };

    CallState _call_state;
    RNS::Bytes _call_peer_hash;
    RNS::Link _call_link;
    LXSTAudio* _lxst_audio;
    uint32_t _call_start_ms;       // millis() when call became ACTIVE
    uint32_t _call_timeout_ms;     // millis() deadline for current wait state
    bool _call_muted;
    volatile bool _call_answer_pending;  // Set by LVGL task, consumed by main loop
    volatile bool _call_link_closed_pending;  // Set by link callback, consumed by call_update
    volatile uint8_t _call_signal_pending;    // 0xFF = none; set by packet callback

    // Singleton instance pointer for static Link callbacks
    static UIManager* s_call_instance;

    // Voice call methods
    void call_initiate(const RNS::Bytes& peer_hash);
    void call_hangup();
    void call_set_mute(bool muted);
    void call_update();  // Called from update() — pumps audio packets + state machine

    // Process a received signalling byte (runs under LVGL lock in call_update)
    void call_process_signal(uint8_t signal);

    // Send a signalling byte over the call link
    void call_send_signal(uint8_t signal);

    // Send encoded audio packet over the call link
    void call_send_audio(const uint8_t* data, int length);

    // Handle received packet on call link (queues signals for call_update)
    void call_on_packet(const RNS::Bytes& data);

    // Transition to call ended and schedule return to chat
    void call_ended();

    // Incoming call callbacks (LXST IN destination)
    static void on_lxst_link_established(RNS::Link& link);
    static void on_lxst_caller_identified(const RNS::Link& link, const RNS::Identity& identity);
    void call_answer();

    // Static Link callbacks (delegate to s_call_instance)
    static void on_call_link_established(RNS::Link& link);
    static void on_call_link_closed(RNS::Link& link);
    static void on_call_link_packet(const RNS::Bytes& plaintext, const RNS::Packet& packet);
};

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
#endif // UI_LXMF_UIMANAGER_H
