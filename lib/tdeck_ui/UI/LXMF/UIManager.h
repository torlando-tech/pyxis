// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_UIMANAGER_H
#define UI_LXMF_UIMANAGER_H

#ifdef ARDUINO
#include <Arduino.h>
#include <lvgl.h>
#include <atomic>
#include <functional>
#include "HomeScreen.h"
#include "NetworkScreen.h"
#include "NomadNetScreen.h"
#include "NavigationStack.h"
#include "NomadNetUrl.h"
#include "NomadNetDocument.h"
#include "NomadNetProtocol.h"
#include "NomadNetHistory.h"
#include "NomadNetMailbox.h"
#include "NomadNetRequestPolicy.h"
#include "NomadNetActionMailbox.h"
#include "NomadNetLibrary.h"
#include "ConversationListScreen.h"
#include "ChatScreen.h"
#include "ComposeScreen.h"
#include "AnnounceListScreen.h"
#include "StatusScreen.h"
#include "RadioActivityScreen.h"
#include "QRScreen.h"
#include "SettingsScreen.h"
#include "PropagationNodesScreen.h"
#include "CallScreen.h"
#include "CallCommandMailbox.h"
#include "CallStartMailbox.h"
#include "CallGenerationGuard.h"
#include "CallLinkOwnership.h"
#include "CallLivenessWatchdog.h"
#include "LXSTSignalParser.h"
#include "MapScreen.h"
#include "LXMF/LXMRouter.h"
#include "LXMF/PropagationNodeManager.h"
#include "LXMF/MessageStore.h"
#include "Telemetry/LocationMessagePolicy.h"
#include "Telemetry/LocationFixAdapter.h"
#include "Telemetry/LocationLxmfAdapter.h"
#include "Telemetry/LocationPersistenceController.h"
#include "Telemetry/LocationPersistenceLittleFS.h"
#include <microReticulum/Reticulum.h>
#include <microReticulum/Link.h>

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
    UIManager(RNS::Reticulum& reticulum, ::LXMF::LXMRouter& router,
              ::LXMF::MessageStore& store,
              bool location_filesystem_available);

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
     * Pump TX audio without LVGL lock — call from main loop for low-latency TX.
     * Safe to call on every loop iteration; no-ops when not in a call.
     */
    void pump_call_tx();

    /**
     * Audio loopback test mode (firmware-local). Captures mic -> encode ->
     * frame -> parse -> decode entirely on-device and dumps the decoded PCM
     * over UDP for an automated harness to score. Uses the Codec2 mode set by
     * the preferred profile (T:CALL_PROFILE). No real call/link required.
     */
    void start_loopback();
    void stop_loopback();
    bool is_loopback() const { return _call_loopback; }

    void show_home();
    void show_network();
    void show_nomadnet();
    void navigate(Route route);
    void back();
    void home();

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
    void show_map();

    /**
     * Show announce list screen
     */
    void show_announces();

    /**
     * Show status screen
     */
    void show_status();

    /** Show current-channel activity history from the Status screen. */
    void show_radio_activity();

    /** True only while sampling/rendering the dedicated activity screen. */
    bool radio_activity_visible() const { return _navigation.current() == Route::RADIO_ACTIVITY; }

    /** Install a copied snapshot provider and immutable active RF settings. */
    void set_radio_activity_source(
        std::function<RadioActivity::Snapshot()> snapshot_provider,
        const RadioActivityScreen::RadioConfig& config);

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

    // Location sharing is always explicit and peer-scoped. No session exists
    // until the UI calls start_location_sharing().
    Telemetry::LocationConsentResult start_location_sharing(
        const RNS::Bytes& peer_hash,
        const Telemetry::ShareStartOptions& options);
    Telemetry::LocationConsentResult stop_location_sharing(
        const RNS::Bytes& peer_hash);
    bool get_location_share_session(
        const RNS::Bytes& peer_hash,
        Telemetry::ShareSession& output) const;

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

#ifdef PYXIS_TEST_HOOKS
    /**
     * Test-only API surface for the soak / LXST harness. None of these
     * are reachable from the UI; they're called from the T:CALL serial
     * commands defined in main.cpp under PYXIS_TEST_HOOKS.
     */

    enum class TestCallInitiateResult {
        FAILED,
        BUSY,
        STARTED,
    };

    /** Initiate an outgoing call and report its exact admission outcome. */
    TestCallInitiateResult test_call_initiate(const RNS::Bytes& peer_hash) {
        call_initiate(peer_hash);
        return _test_call_initiate_result;
    }

    /** Hang up the active call on loopTask (calls private call_hangup). */
    void test_call_hangup() { call_hangup(); }

    /**
     * Programmatic answer for incoming calls. Sets the same
     * _call_answer_pending flag the UI button does so the main loop
     * picks it up and runs call_answer() in its proper context. Used
     * by the harness for pyxis-as-callee interop tests with real
     * LXST.Telephony.Telephone clients (Sideband, MeshChatX). Returns
     * true if there was an incoming ring to accept.
     */
    bool test_call_answer();

    /** Pyxis's lxst.telephony destination hash, hex. Empty if the
     *  LXST destination has not been registered yet (early boot). */
    std::string test_lxst_dest_hex() const;

    /**
     * String name of the current call state, eg "IDLE", "ACTIVE",
     * "INCOMING_RINGING". Stable for harness assertions.
     */
    const char* test_call_state_name() const;

    /** Number of audio frames TX'd since the active call started. */
    uint32_t test_call_audio_tx_count() const { return _call_audio_tx_count; }

    /** Number of audio frames RX'd since the active call started. */
    uint32_t test_call_audio_rx_count() const { return _call_audio_rx_count; }

    /**
     * QoS counters from the playback decode path. decode_ok counts
     * successful Codec2 decodes; decode_fail counts decode errors
     * (malformed frame, bad mode header, codec internal error).
     * Together they validate wire-level audio fidelity. Reset by
     * call_initiate; the harness samples them after ACTIVE.
     *
     * Defined in UIManager.cpp (LXSTAudio is forward-declared here).
     */
    uint32_t test_call_decode_ok() const;
    uint32_t test_call_decode_fail() const;

    /**
     * PCM energy on the decoded audio. Together with sample_count
     * gives a running RMS for content-fidelity validation. Reset by
     * call_initiate.
     */
    uint32_t test_call_pcm_sample_count() const;
    uint64_t test_call_pcm_sum_squares() const;

    /**
     * Replace mic input with a synthesized sine for the active call.
     * Bypasses ES7210 capture and the voice filter chain so the
     * remote sees a clean tone. Used by the LXST harness for
     * bidirectional content-fidelity validation.
     */
    void test_call_set_inject_sine(bool enabled, int freq = 1000, float amp = 0.5f);

    /**
     * Get/set the production voice profile. Only ULBW (0x10,
     * Codec2-700C) is accepted; wider profiles violate the LoRa budget.
     */
    int test_call_get_profile() const { return _preferred_profile; }
    bool test_call_set_profile(int profile);
#endif

private:
    RNS::Reticulum& _reticulum;
    ::LXMF::LXMRouter& _router;
    ::LXMF::MessageStore& _store;
    Telemetry::PeerLocationStore _peer_locations;
    Telemetry::LocationShareScheduler _location_shares;
    Telemetry::LocationPersistenceLittleFS* _location_storage;
    Telemetry::TransactionalLocationPersistence* _location_transaction;
    Telemetry::LocationPersistenceController* _location_persistence_controller;
    TinyGPSPlus* _gps;
    RNS::Destination _lxst_destination;

    NavigationStack _navigation;
    RNS::Bytes _current_peer_hash;

    // Conversation-list refresh debouncing. on_message_received() used
    // to call refresh() unconditionally per message — under propagation
    // sync flood (50+ queued messages delivered back-to-back) that's
    // 50 full LVGL redraws of the list, each holding LVGL_LOCK,
    // saturating the SPI display flush, and starving the USB CDC
    // serial-handler. Now we just set the pending flag here; the main
    // loop drains it at most once every COALESCE_MS.
    volatile bool _pending_conversation_refresh;
    uint32_t _last_conversation_refresh_ms;

    HomeScreen* _home_screen;
    NetworkScreen* _network_screen;
    NomadNetScreen* _nomadnet_screen;
    ConversationListScreen* _conversation_list_screen;
    ChatScreen* _chat_screen;
    ComposeScreen* _compose_screen;
    AnnounceListScreen* _announce_list_screen;
    StatusScreen* _status_screen;
    RadioActivityScreen* _radio_activity_screen;
    QRScreen* _qr_screen;
    SettingsScreen* _settings_screen;
    PropagationNodesScreen* _propagation_nodes_screen;
    CallScreen* _call_screen;
    std::function<RadioActivity::Snapshot()> _radio_activity_snapshot_provider;
    RadioActivityScreen::RadioConfig _radio_activity_config;
    MapScreen* _map_screen;

    ::LXMF::PropagationNodeManager* _propagation_manager;
    RNS::Interface* _ble_interface;

    bool _initialized;

    NomadNet::Url _nomad_url;
    NomadNet::DocumentParser _nomad_parser;
    NomadNet::ResponseBuffer _nomad_response;
    NomadNet::PageHistory _nomad_history;
    NomadNet::AsyncMailbox _nomad_mailbox;
    NomadNet::ActionMailbox _nomad_actions;
    NomadNet::Library _nomad_library;
    NomadNet::RequestPolicy _nomad_request_policy;

    std::atomic<bool> _nomad_directory_refresh_pending{false};
    bool _nomad_library_dirty = false;
    uint32_t _nomad_last_library_save_ms = 0;
    uint32_t _nomad_last_directory_refresh_ms = 0;
    RNS::Bytes _nomad_destination_hash;
    RNS::Link _nomad_link{RNS::Type::NONE};
    RNS::RequestReceipt _nomad_request{RNS::Type::NONE};
    enum class NomadState { IDLE, PATH, LINK, REQUEST };
    NomadState _nomad_state = NomadState::IDLE;
    uint32_t _nomad_deadline_ms = 0;
    static UIManager* s_nomad_instance;

    void render_route(Route route);
    void replace_route(Route route);
    void hide_all_screens();
    void nomad_open(const std::string& address, bool add_history = true);
    void nomad_reload();
    void nomad_update();
    void nomad_start_link();
    void nomad_send_request();
    void nomad_release_request();
    void nomad_stop_transport();
    bool nomad_refresh_path_after_link_failure();
    void nomad_refresh_nodes();
    bool nomad_load_library();
    bool nomad_save_library();
    void nomad_update_library();
    void nomad_update_user_actions();
    static void on_nomad_link_established(RNS::Link& link);
    static void on_nomad_link_closed(RNS::Link& link);
    static void on_nomad_response(const RNS::RequestReceipt& receipt);
    static void on_nomad_failed(const RNS::RequestReceipt& receipt);
    static void on_nomad_progress(const RNS::RequestReceipt& receipt);

    // Screen navigation handlers
    void on_conversation_selected(const RNS::Bytes& peer_hash);
    void on_new_message();
    void on_back_to_conversation_list();
    bool on_send_message_from_chat(const String& content);
    void on_call_from_chat();
    bool on_send_message_from_compose(const RNS::Bytes& dest_hash, const String& message);
    void on_cancel_compose();
    void on_back_from_map();
    void on_announce_selected(const RNS::Bytes& dest_hash);
    void on_back_from_announces();
    void on_back_from_status();
    void on_share_from_status();
    void on_back_from_radio_activity();
    void on_back_from_qr();
    void on_back_from_settings();
    void on_back_from_propagation_nodes();
    void on_propagation_node_selected(const RNS::Bytes& node_hash);
    void on_propagation_auto_select_changed(bool enabled);
    void on_propagation_sync();

    // LXMF message handling
    bool send_message(const RNS::Bytes& dest_hash, const String& content);

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
    // Backward-compatible extension. Legacy peers ignore this status and still
    // receive the Reticulum Link-close fallback.
    static constexpr uint8_t LXST_STATUS_TERMINATED   = 0x07;
    static constexpr int TERMINAL_SIGNAL_SEND_COUNT = 3;
    static constexpr uint32_t TERMINAL_SIGNAL_DRAIN_MS = 20;

    // An accepted incoming link must identify before it can start ringing.
    static constexpr uint32_t INCOMING_IDENTIFY_TIMEOUT_MS = 15000;
    // Codec2 media is continuous even during silence. Ninety seconds permits
    // substantial temporary impairment while bounding orphaned active calls.
    static constexpr uint32_t CALL_MEDIA_LIVENESS_TIMEOUT_MS = 90000;

    // LXST codec type bytes (match LXST Codecs/__init__.py)
    static constexpr uint8_t LXST_CODEC_CODEC2 = 0x02;

    // LXST profile negotiation
    static constexpr int LXST_PREFERRED_MODE    = 0xF0;
    static constexpr int LXST_MODE_FULL_DUPLEX  = 0x01;
    static constexpr int LXST_PREFERRED_PROFILE = 0xFF;
    static constexpr int LXST_PROFILE_ULBW      = 0x10;  // Codec2 700C; sole production profile
    static constexpr int LXST_PROFILE_VLBW      = 0x20;  // protocol value; unsupported locally
    static constexpr int LXST_PROFILE_LBW       = 0x30;  // protocol value; unsupported locally

    // The sole profile Pyxis asks the peer for and configures locally.
    static int _preferred_profile;

    // Map profile byte to the Codec2 library mode constant
    // (CODEC2_MODE_*). Returns -1 for unknown profiles.
    static int profile_to_codec2_mode(int profile);

    enum class CallState {
        IDLE,
        PATH_REQUESTING,    // Outgoing: waiting for path to resolve
        LINK_ESTABLISHING,  // Outgoing: waiting for Link to come up
        WAIT_AVAILABLE,     // Outgoing: link up, waiting for STATUS_AVAILABLE
        WAIT_RINGING,       // Outgoing: sent identify, waiting for STATUS_RINGING
        RINGING,            // Outgoing: remote is ringing
        INCOMING_IDENTIFYING, // Incoming: link reserved, waiting for caller identity
        INCOMING_RINGING,   // Incoming: waiting for user to answer/reject
        CONNECTING,         // Both: opening audio pipelines
        ACTIVE,             // Both: voice flowing
    };

    CallState _call_state;
    // Audio loopback test mode active: relaxes call/link state gates in the
    // TX pump, the wire-packet send site, and the RX parse/decode path so the
    // pipeline runs locally without a real call. Every loopback bypass is
    // gated strictly on this flag — real calls are unaffected.
    bool _call_loopback;
    RNS::Bytes _call_peer_hash;
    RNS::Bytes _call_dest_hash;   // LXST destination hash (for deferred link creation)
    // Vanilla upstream Link's default ctor crashes in load_private_key()
    // when constructed without a real Destination — explicit Type::NONE
    // takes the safe NoneConstructor branch. Same fix as DirectLinkSlot.
    RNS::Link _call_link{RNS::Type::NONE};
    LXSTAudio* _lxst_audio;
    CallStartMailbox _call_starts;
    CallCommandMailbox _call_commands;
    CallGenerationGuard _call_generation_guard;
    CallLinkOwnership _call_link_ownership;
    CallLivenessWatchdog _call_liveness;
#ifdef PYXIS_TEST_HOOKS
    TestCallInitiateResult _test_call_initiate_result =
        TestCallInitiateResult::FAILED;
#endif
    uint32_t _call_start_ms;       // millis() when call became ACTIVE
    uint32_t _call_timeout_ms;     // millis() deadline for current wait state
    bool _call_muted;
    // Set by the LVGL task and consumed/reset by loopTask.
    std::atomic<bool> _call_answer_pending;

    // Signal queue: written by Reticulum thread, consumed by call_update under LVGL lock
    static constexpr int SIGNAL_QUEUE_SIZE =
        static_cast<int>(LXSTSignalParser::MIN_SENTINEL_QUEUE_SIZE);
    volatile uint8_t _call_signal_queue[SIGNAL_QUEUE_SIZE];
    volatile uint8_t _call_signal_write;  // Next write index (Reticulum thread)
    volatile uint8_t _call_signal_read;   // Next read index (main thread)
    uint32_t _call_audio_rx_count;    // Count of received audio frames (for diagnostics)
    uint32_t _call_audio_tx_count;    // Count of sent audio frames (for diagnostics)

    // Singleton instance pointer for static Link callbacks
    static UIManager* s_call_instance;

    // Voice call methods
    void call_initiate(const RNS::Bytes& peer_hash);
    void call_hangup();
    void call_set_mute(bool muted);
    void call_request_hangup();
    void call_request_mute(bool muted);
    uint32_t call_begin_generation();
    void call_clear_generation(uint32_t expected_generation);
    uint32_t call_current_generation() const;
    static bool call_extract_link_id(
        const RNS::Link& link, CallLinkOwnership::LinkId& id);
    bool call_publish_link(const RNS::Link& link, uint32_t generation);
    bool call_owns_link(
        const CallLinkOwnership::LinkId& id, uint32_t generation) const;
    void call_teardown_audio();
    void call_update();  // Called from update() — pumps audio packets + state machine

    // Process a received signalling byte (runs under LVGL lock in call_update)
    void call_process_signal(uint8_t signal);

    // Send a signalling byte over the call link
    void call_send_signal(int signal);
    static void call_send_signal_on_link(const RNS::Link& link, int signal);
    void call_send_terminal_burst();

    // Send batched audio frames over the call link (10 sub-frames per batch)
    void call_send_audio_batch(const uint8_t* batch_data, int batch_len, int batch_count, int total_frames);

    // Process a single received audio frame (codec_header + data)
    void call_rx_audio_frame(const uint8_t* frame, size_t frame_len);

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
