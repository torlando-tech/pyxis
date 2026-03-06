// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "UIManager.h"

#ifdef ARDUINO

#include <lvgl.h>
#include <Preferences.h>
#include "Log.h"
#include "Tone.h"
#include "../LVGL/LVGLLock.h"
#include "lxst_audio.h"
#include "Packet.h"
#include "Transport.h"
#include "Destination.h"
#include <TinyGPSPlus.h>
#include "Utilities/OS.h"

// Direct UDP+Serial log (bypasses RNS log system which may not reach UDP)
extern "C" void pyxis_log(const char* msg);

using namespace RNS;

// NVS keys for propagation settings
static const char* NVS_NAMESPACE = "propagation";
static const char* KEY_AUTO_SELECT = "auto_select";
static const char* KEY_NODE_HASH = "node_hash";
static const char* KEY_STAMP_COST = "stamp_cost";

namespace UI {
namespace LXMF {

// Static singleton for Link callbacks
UIManager* UIManager::s_call_instance = nullptr;

// LXST announce handler — tracks peers that support voice calls
class LXSTAnnounceHandler : public AnnounceHandler {
public:
    LXSTAnnounceHandler() : AnnounceHandler("lxst.telephony") {}
    void received_announce(const Bytes& dest_hash, const Identity& identity, const Bytes& app_data) override {
        std::string hash_hex = dest_hash.toHex().substr(0, 16);
        INFO(("LXST: Voice announce from " + hash_hex + "...").c_str());
    }
};
static std::shared_ptr<LXSTAnnounceHandler> s_lxst_announce_handler;

UIManager::UIManager(Reticulum& reticulum, ::LXMF::LXMRouter& router, ::LXMF::MessageStore& store)
    : _reticulum(reticulum), _router(router), _store(store),
      _current_screen(SCREEN_CONVERSATION_LIST),
      _conversation_list_screen(nullptr),
      _chat_screen(nullptr),
      _compose_screen(nullptr),
      _announce_list_screen(nullptr),
      _status_screen(nullptr),
      _qr_screen(nullptr),
      _settings_screen(nullptr),
      _propagation_nodes_screen(nullptr),
      _call_screen(nullptr),
      _map_screen(nullptr),
      _propagation_manager(nullptr),
      _ble_interface(nullptr),
      _gps(nullptr),
      _initialized(false),
      _call_state(CallState::IDLE),
      _lxst_audio(nullptr),
      _call_start_ms(0),
      _call_timeout_ms(0),
      _call_muted(false),
      _call_answer_pending(false),
      _call_link_closed_pending(false),
      _call_signal_write(0),
      _call_signal_read(0),
      _call_audio_rx_count(0),
      _call_audio_tx_count(0) {
    memset((void*)_call_signal_queue, 0, sizeof(_call_signal_queue));
}

UIManager::~UIManager() {
    // Clean up call state
    if (_call_state != CallState::IDLE) {
        call_hangup();
    }
    delete _lxst_audio;

    if (_conversation_list_screen) delete _conversation_list_screen;
    if (_chat_screen) delete _chat_screen;
    if (_compose_screen) delete _compose_screen;
    if (_announce_list_screen) delete _announce_list_screen;
    if (_status_screen) delete _status_screen;
    if (_qr_screen) delete _qr_screen;
    if (_settings_screen) delete _settings_screen;
    if (_propagation_nodes_screen) delete _propagation_nodes_screen;
    if (_call_screen) delete _call_screen;
    if (_map_screen) delete _map_screen;
}

bool UIManager::init() {
    LVGL_LOCK();
    if (_initialized) {
        return true;
    }

    INFO("Initializing UIManager");

    // Create all screens
    _conversation_list_screen = new ConversationListScreen();
    _chat_screen = new ChatScreen();
    _compose_screen = new ComposeScreen();
    _announce_list_screen = new AnnounceListScreen();
    _status_screen = new StatusScreen();
    _qr_screen = new QRScreen();
    _settings_screen = new SettingsScreen();
    _propagation_nodes_screen = new PropagationNodesScreen();
    _call_screen = new CallScreen();
    _map_screen = new MapScreen();

    // Set up callbacks for conversation list screen
    _conversation_list_screen->set_conversation_selected_callback(
        [this](const Bytes& peer_hash) { on_conversation_selected(peer_hash); }
    );

    _conversation_list_screen->set_compose_callback(
        [this]() { on_new_message(); }
    );

    _conversation_list_screen->set_sync_callback(
        [this]() { on_propagation_sync(); }
    );

    _conversation_list_screen->set_settings_callback(
        [this]() { show_settings(); }
    );

    _conversation_list_screen->set_announces_callback(
        [this]() { show_announces(); }
    );

    // Set up callbacks for chat screen
    _chat_screen->set_back_callback(
        [this]() { on_back_to_conversation_list(); }
    );

    _chat_screen->set_send_message_callback(
        [this](const String& content) { on_send_message_from_chat(content); }
    );

    _chat_screen->set_call_callback(
        [this]() { on_call_from_chat(); }
    );

    // Set up callbacks for compose screen
    _compose_screen->set_cancel_callback(
        [this]() { on_cancel_compose(); }
    );

    _compose_screen->set_send_callback(
        [this](const Bytes& dest_hash, const String& message) {
            on_send_message_from_compose(dest_hash, message);
        }
    );

    // Set up callbacks for announce list screen
    _announce_list_screen->set_announce_selected_callback(
        [this](const Bytes& dest_hash) { on_announce_selected(dest_hash); }
    );

    _announce_list_screen->set_back_callback(
        [this]() { on_back_from_announces(); }
    );

    _announce_list_screen->set_send_announce_callback(
        [this]() {
            INFO("Sending LXMF announce...");
            try {
                _router.announce();
                INFO("LXMF announce sent successfully");
            } catch (const std::exception& e) {
                ERRORF("LXMF announce failed: %s", e.what());
            }
        }
    );

    // Set up callbacks for status screen
    _status_screen->set_back_callback(
        [this]() { on_back_from_status(); }
    );

    _status_screen->set_share_callback(
        [this]() { on_share_from_status(); }
    );

    // Set up callbacks for QR screen
    _qr_screen->set_back_callback(
        [this]() { on_back_from_qr(); }
    );

    // Set up callbacks for settings screen
    _settings_screen->set_back_callback(
        [this]() { on_back_from_settings(); }
    );

    _settings_screen->set_propagation_nodes_callback(
        [this]() { show_propagation_nodes(); }
    );

    // Set up callbacks for propagation nodes screen
    _propagation_nodes_screen->set_back_callback(
        [this]() { on_back_from_propagation_nodes(); }
    );

    _propagation_nodes_screen->set_node_selected_callback(
        [this](const Bytes& node_hash) { on_propagation_node_selected(node_hash); }
    );

    _propagation_nodes_screen->set_auto_select_changed_callback(
        [this](bool enabled) { on_propagation_auto_select_changed(enabled); }
    );

    _propagation_nodes_screen->set_sync_callback(
        [this]() { on_propagation_sync(); }
    );

    // Set up callbacks for call screen
    _call_screen->set_hangup_callback(
        [this]() { call_hangup(); }
    );

    _call_screen->set_mute_callback(
        [this](bool muted) { call_set_mute(muted); }
    );

    // Load settings from NVS
    _settings_screen->load_settings();

    // Restore propagation node selection from NVS
    {
        Preferences prefs;
        prefs.begin(NVS_NAMESPACE, true);
        bool auto_select = prefs.getBool(KEY_AUTO_SELECT, true);
        uint8_t stamp_cost = prefs.getUChar(KEY_STAMP_COST, 0);
        Bytes saved_hash;
        size_t hash_len = prefs.getBytesLength(KEY_NODE_HASH);
        if (hash_len > 0 && hash_len <= 32) {
            uint8_t buf[32];
            prefs.getBytes(KEY_NODE_HASH, buf, hash_len);
            saved_hash = Bytes(buf, hash_len);
        }
        prefs.end();

        if (!auto_select && saved_hash.size() > 0) {
            _router.set_outbound_propagation_node(saved_hash);
            _router.set_outbound_propagation_stamp_cost(stamp_cost);
            INFO(("Restored propagation node from NVS: " + saved_hash.toHex().substr(0, 16) + "...").c_str());
        }
    }

    // Set identity and LXMF address on settings screen
    _settings_screen->set_identity_hash(_router.identity().hash());
    _settings_screen->set_lxmf_address(_router.delivery_destination().hash());

    // Set up callback for status button in conversation list
    _conversation_list_screen->set_status_callback(
        [this]() { show_status(); }
    );

    // Set up callback for map button in conversation list
    _conversation_list_screen->set_map_callback(
        [this]() { show_map(); }
    );

    // Set up callback for map screen back button
    _map_screen->set_back_callback(
        [this]() { show_conversation_list(); }
    );

    // Set up location sharing callback for chat screen
    _chat_screen->set_location_share_callback(
        [this](int duration_index) {
            on_location_share_requested(_current_peer_hash, duration_index);
        }
    );

    // Load telemetry state from SPIFFS
    _telemetry_manager.load();

    // Set identity hash and LXMF address on status screen
    _status_screen->set_identity_hash(_router.identity().hash());
    _status_screen->set_lxmf_address(_router.delivery_destination().hash());

    // Set identity and LXMF address on QR screen
    _qr_screen->set_identity(_router.identity());
    _qr_screen->set_lxmf_address(_router.delivery_destination().hash());

    // Register LXMF delivery callback
    _router.register_delivery_callback(
        [this](::LXMF::LXMessage& message) { on_message_received(message); }
    );

    // Set up answer callback for incoming calls (deferred to main loop)
    _call_screen->set_answer_callback(
        [this]() { _call_answer_pending = true; }
    );

    // Load conversations and show conversation list
    _conversation_list_screen->load_conversations(_store);
    show_conversation_list();

    // Create LXST IN destination for incoming voice calls
    _lxst_destination = Destination(_router.identity(), Type::Destination::IN,
                                     Type::Destination::SINGLE, "lxst", "telephony");
    _lxst_destination.set_proof_strategy(Type::Destination::PROVE_NONE);
    _lxst_destination.set_link_established_callback(on_lxst_link_established);
    s_call_instance = this;

    // Register LXST announce handler
    s_lxst_announce_handler = std::make_shared<LXSTAnnounceHandler>();
    Transport::register_announce_handler(HAnnounceHandler(s_lxst_announce_handler));

    std::string lxst_hash = _lxst_destination.hash().toHex();
    INFO(("LXST: Listening on " + lxst_hash).c_str());

    _initialized = true;
    INFO("UIManager initialized");

    return true;
}

void UIManager::update() {
    LVGL_LOCK();
    // Process outbound LXMF messages
    _router.process_outbound();

    // Process inbound LXMF messages
    _router.process_inbound();

    // Pump voice call state machine
    if (_call_state != CallState::IDLE) {
        call_update();
    }

    // Update status indicators (WiFi/battery) on conversation list
    static uint32_t last_status_update = 0;
    uint32_t now = millis();
    if (now - last_status_update > 3000) {  // Update every 3 seconds
        last_status_update = now;
        if (_conversation_list_screen) {
            _conversation_list_screen->update_status();
        }
        // Update status screen if visible
        if (_current_screen == SCREEN_STATUS && _status_screen) {
            _status_screen->refresh();
        }
        // Update map GPS position if visible
        if (_current_screen == SCREEN_MAP && _map_screen) {
            _map_screen->update_gps_position();
            update_map_peer_markers();
        }

        // Update telemetry: check expired sessions, send to peers
        uint32_t time_now = (uint32_t)Utilities::OS::time();
        {
            // Periodic telemetry state debug (every ~30s)
            static uint32_t last_telem_debug = 0;
            if (now - last_telem_debug > 30000) {
                last_telem_debug = now;
                auto& sessions = _telemetry_manager.get_sessions();
                char dbg[128];
                snprintf(dbg, sizeof(dbg), "[TELEM] sessions=%zu time=%u gps=%s sats=%d",
                    sessions.size(), time_now,
                    (_gps && _gps->location.isValid()) ? "fix" : "none",
                    _gps ? (int)_gps->satellites.value() : -1);
                pyxis_log(dbg);
            }
        }
        std::vector<Bytes> peers_to_send = _telemetry_manager.update(time_now);
        if (!peers_to_send.empty()) {
            char log_buf[64];
            snprintf(log_buf, sizeof(log_buf), "[TELEM] %zu peers need send", peers_to_send.size());
            pyxis_log(log_buf);
        }
        for (const auto& peer : peers_to_send) {
            send_telemetry(peer);
        }
    }
}

void UIManager::show_conversation_list() {
    LVGL_LOCK();
    INFO("Showing conversation list");

    _conversation_list_screen->refresh();
    _conversation_list_screen->show();
    _chat_screen->hide();
    _compose_screen->hide();
    _announce_list_screen->hide();
    _status_screen->hide();
    _settings_screen->hide();
    _propagation_nodes_screen->hide();
    if (_call_screen) _call_screen->hide();
    if (_map_screen) _map_screen->hide();

    _current_screen = SCREEN_CONVERSATION_LIST;
}

void UIManager::show_chat(const Bytes& peer_hash) {
    LVGL_LOCK();
    std::string hash_hex = peer_hash.toHex().substr(0, 8);
    std::string msg = "Showing chat with peer " + hash_hex + "...";
    INFO(msg.c_str());

    _current_peer_hash = peer_hash;

    _chat_screen->load_conversation(peer_hash, _store);
    _chat_screen->set_sharing_state(_telemetry_manager.is_sharing(peer_hash));
    _chat_screen->show();
    _conversation_list_screen->hide();
    _compose_screen->hide();
    _announce_list_screen->hide();
    _status_screen->hide();
    _settings_screen->hide();
    _propagation_nodes_screen->hide();
    if (_call_screen) _call_screen->hide();
    if (_map_screen) _map_screen->hide();

    _current_screen = SCREEN_CHAT;
}

void UIManager::show_compose() {
    LVGL_LOCK();
    INFO("Showing compose screen");

    _compose_screen->clear();
    _compose_screen->show();
    _conversation_list_screen->hide();
    _chat_screen->hide();
    _announce_list_screen->hide();
    _status_screen->hide();
    _settings_screen->hide();
    _propagation_nodes_screen->hide();
    if (_map_screen) _map_screen->hide();

    _current_screen = SCREEN_COMPOSE;
}

void UIManager::show_announces() {
    LVGL_LOCK();
    INFO("Showing announces screen");

    _announce_list_screen->refresh();
    _announce_list_screen->show();
    _conversation_list_screen->hide();
    _chat_screen->hide();
    _compose_screen->hide();
    _status_screen->hide();
    _settings_screen->hide();
    _propagation_nodes_screen->hide();
    if (_map_screen) _map_screen->hide();

    _current_screen = SCREEN_ANNOUNCES;
}

void UIManager::show_status() {
    LVGL_LOCK();
    INFO("Showing status screen");

    // Build propagation node display string
    if (_propagation_manager) {
        Preferences prefs;
        prefs.begin(NVS_NAMESPACE, true);
        bool auto_select = prefs.getBool(KEY_AUTO_SELECT, true);

        Bytes saved_hash;
        size_t hash_len = prefs.getBytesLength(KEY_NODE_HASH);
        if (hash_len > 0 && hash_len <= 32) {
            uint8_t buf[32];
            prefs.getBytes(KEY_NODE_HASH, buf, hash_len);
            saved_hash = Bytes(buf, hash_len);
        }
        prefs.end();

        Bytes effective = auto_select ? _propagation_manager->get_effective_node() : saved_hash;

        String display;
        if (auto_select) {
            if (effective.size() > 0) {
                auto info = _propagation_manager->get_node(effective);
                if (info && !info.name.empty()) {
                    display = "Auto (" + String(info.name.c_str()) + ")";
                } else {
                    display = "Auto (" + String(effective.toHex().substr(0, 12).c_str()) + "...)";
                }
            } else {
                display = "Auto";
            }
        } else {
            if (effective.size() > 0) {
                auto info = _propagation_manager->get_node(effective);
                if (info && !info.name.empty()) {
                    display = String(info.name.c_str());
                } else {
                    display = String(effective.toHex().substr(0, 12).c_str()) + "...";
                }
            } else {
                display = "None";
            }
        }
        _status_screen->set_propagation_node(display);
    }

    _status_screen->refresh();
    _status_screen->show();
    _conversation_list_screen->hide();
    _chat_screen->hide();
    _compose_screen->hide();
    _announce_list_screen->hide();
    _settings_screen->hide();
    _propagation_nodes_screen->hide();
    if (_map_screen) _map_screen->hide();

    _current_screen = SCREEN_STATUS;
}

void UIManager::show_map() {
    LVGL_LOCK();
    INFO("Showing map screen");

    _map_screen->show();
    _conversation_list_screen->hide();
    _chat_screen->hide();
    _compose_screen->hide();
    _announce_list_screen->hide();
    _status_screen->hide();
    _settings_screen->hide();
    _propagation_nodes_screen->hide();
    if (_call_screen) _call_screen->hide();

    _current_screen = SCREEN_MAP;
}

void UIManager::on_conversation_selected(const Bytes& peer_hash) {
    show_chat(peer_hash);
}

void UIManager::on_new_message() {
    show_compose();
}

void UIManager::show_settings() {
    LVGL_LOCK();
    INFO("Showing settings screen");

    _settings_screen->refresh();
    _settings_screen->show();
    _conversation_list_screen->hide();
    _chat_screen->hide();
    _compose_screen->hide();
    _announce_list_screen->hide();
    _status_screen->hide();
    _propagation_nodes_screen->hide();
    if (_map_screen) _map_screen->hide();

    _current_screen = SCREEN_SETTINGS;
}

void UIManager::show_propagation_nodes() {
    LVGL_LOCK();
    INFO("Showing propagation nodes screen");

    if (_propagation_manager) {
        // Load settings from NVS
        Preferences prefs;
        prefs.begin(NVS_NAMESPACE, true);  // read-only
        bool auto_select = prefs.getBool(KEY_AUTO_SELECT, true);

        Bytes selected_hash;
        size_t hash_len = prefs.getBytesLength(KEY_NODE_HASH);
        if (hash_len > 0 && hash_len <= 32) {
            uint8_t buf[32];
            prefs.getBytes(KEY_NODE_HASH, buf, hash_len);
            selected_hash = Bytes(buf, hash_len);
        }
        prefs.end();

        // If not auto-select and we have a saved hash, use it
        if (!auto_select && selected_hash.size() > 0) {
            _router.set_outbound_propagation_node(selected_hash);
        }

        _propagation_nodes_screen->load_nodes(*_propagation_manager, selected_hash, auto_select);
    }

    _propagation_nodes_screen->show();
    _conversation_list_screen->hide();
    _chat_screen->hide();
    _compose_screen->hide();
    _announce_list_screen->hide();
    _status_screen->hide();
    _settings_screen->hide();
    if (_map_screen) _map_screen->hide();

    _current_screen = SCREEN_PROPAGATION_NODES;
}

void UIManager::set_propagation_node_manager(::LXMF::PropagationNodeManager* manager) {
    _propagation_manager = manager;
}

void UIManager::set_lora_interface(Interface* iface) {
    if (_conversation_list_screen) {
        _conversation_list_screen->set_lora_interface(iface);
    }
}

void UIManager::set_ble_interface(Interface* iface) {
    _ble_interface = iface;
    if (_conversation_list_screen) {
        _conversation_list_screen->set_ble_interface(iface);
    }
}

void UIManager::set_gps(TinyGPSPlus* gps) {
    _gps = gps;
    if (_conversation_list_screen) {
        _conversation_list_screen->set_gps(gps);
    }
    if (_map_screen) {
        _map_screen->set_gps(gps);
    }
}

void UIManager::on_back_to_conversation_list() {
    show_conversation_list();
}

void UIManager::on_send_message_from_chat(const String& content) {
    send_message(_current_peer_hash, content);
}

void UIManager::on_call_from_chat() {
    if (!_current_peer_hash) return;
    if (_call_state != CallState::IDLE) {
        WARNING("Already in a call");
        return;
    }

    call_initiate(_current_peer_hash);
}

void UIManager::on_send_message_from_compose(const Bytes& dest_hash, const String& message) {
    send_message(dest_hash, message);

    // Switch to chat screen for this conversation
    show_chat(dest_hash);
}

void UIManager::on_cancel_compose() {
    show_conversation_list();
}

void UIManager::on_announce_selected(const Bytes& dest_hash) {
    std::string hash_hex = dest_hash.toHex().substr(0, 8);
    std::string msg = "Announce selected: " + hash_hex + "...";
    INFO(msg.c_str());

    // Go directly to chat screen with this destination
    show_chat(dest_hash);
}

void UIManager::on_back_from_announces() {
    show_conversation_list();
}

void UIManager::on_back_from_status() {
    show_conversation_list();
}

void UIManager::on_share_from_status() {
    LVGL_LOCK();
    _status_screen->hide();
    _qr_screen->show();
    _current_screen = SCREEN_QR;
}

void UIManager::on_back_from_qr() {
    LVGL_LOCK();
    _qr_screen->hide();
    _status_screen->show();
    _current_screen = SCREEN_STATUS;
}

void UIManager::on_back_from_settings() {
    show_conversation_list();
}

void UIManager::on_back_from_propagation_nodes() {
    show_settings();
}

void UIManager::on_propagation_node_selected(const Bytes& node_hash) {
    std::string hash_hex = node_hash.toHex().substr(0, 16);
    std::string msg = "Propagation node selected: " + hash_hex + "...";
    INFO(msg.c_str());

    // Set the node in the router
    _router.set_outbound_propagation_node(node_hash);

    // Set stamp cost from node info if available
    uint8_t stamp_cost = 0;
    if (_propagation_manager) {
        auto node_info = _propagation_manager->get_node(node_hash);
        if (node_info) {
            stamp_cost = node_info.stamp_cost;
        }
    }
    _router.set_outbound_propagation_stamp_cost(stamp_cost);

    // Proactively request path if we don't have one
    if (!Transport::has_path(node_hash)) {
        DEBUG("Requesting path for propagation node");
        Transport::request_path(node_hash);
    }

    // Save to NVS
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool(KEY_AUTO_SELECT, false);
    prefs.putBytes(KEY_NODE_HASH, node_hash.data(), node_hash.size());
    prefs.putUChar(KEY_STAMP_COST, stamp_cost);
    prefs.end();
    DEBUG("Propagation node saved to NVS");
}

void UIManager::on_propagation_auto_select_changed(bool enabled) {
    std::string msg = "Propagation auto-select changed: ";
    msg += enabled ? "enabled" : "disabled";
    INFO(msg.c_str());

    if (enabled) {
        // Clear manual selection, router will use best node
        _router.set_outbound_propagation_node(Bytes());
    }

    // Save to NVS
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool(KEY_AUTO_SELECT, enabled);
    if (enabled) {
        prefs.remove(KEY_NODE_HASH);
        prefs.remove(KEY_STAMP_COST);
    }
    prefs.end();
    DEBUG("Propagation auto-select saved to NVS");
}

void UIManager::on_propagation_sync() {
    INFO("Requesting messages from propagation node");
    _router.request_messages_from_propagation_node();
}

void UIManager::set_rns_status(bool connected, const String& server_name) {
    if (_status_screen) {
        _status_screen->set_rns_status(connected, server_name);
    }
}

void UIManager::send_message(const Bytes& dest_hash, const String& content) {
    std::string hash_hex = dest_hash.toHex().substr(0, 8);
    std::string msg = "Sending message to " + hash_hex + "...";
    INFO(msg.c_str());

    // Mark recipient as a persistent contact (survives reboot)
    Identity::mark_persistent(dest_hash);

    // Get our source destination (needed for signing)
    Destination source = _router.delivery_destination();

    // Create message content
    Bytes content_bytes((const uint8_t*)content.c_str(), content.length());
    Bytes title;  // Empty title

    // Look up destination identity
    Identity dest_identity = Identity::recall(dest_hash);

    // Create destination object - either real or placeholder
    Destination destination(Type::NONE);
    if (dest_identity) {
        destination = Destination(dest_identity, Type::Destination::OUT, Type::Destination::SINGLE, "lxmf", "delivery");
        INFO("  Destination identity known");
    } else {
        WARNING("  Destination identity not known, message may fail until peer announces");
    }

    // Create message with destination and source objects
    // Source is needed for signing
    ::LXMF::LXMessage message(destination, source, content_bytes, title);

    // If destination identity was unknown, manually set the destination hash
    if (!dest_identity) {
        message.destination_hash(dest_hash);
        DEBUG("  Set destination hash manually");
    }

    // Pack the message to generate hash and signature before saving
    message.pack();

    // Add to UI immediately (optimistic update)
    if (_current_screen == SCREEN_CHAT && _current_peer_hash == dest_hash) {
        _chat_screen->add_message(message, true);
    }

    // Save to store (now has valid hash from pack())
    _store.save_message(message);

    // Queue for sending (pack already called, will use cached packed data)
    _router.handle_outbound(message);

    INFO("  Message queued for delivery");
}

void UIManager::on_message_received(::LXMF::LXMessage& message) {
    LVGL_LOCK();
    std::string source_hex = message.source_hash().toHex().substr(0, 8);
    std::string msg = "Message received from " + source_hex + "...";
    INFO(msg.c_str());

    // Mark sender as a persistent contact (survives reboot)
    RNS::Identity::mark_persistent(message.source_hash());

    // Check for telemetry fields FIRST — telemetry-only messages should not
    // be saved to store, shown in chat, or play notification sounds
    bool has_telemetry = false;
    bool has_cease = false;

    {
        uint8_t telem_key = Telemetry::FIELD_TELEMETRY;
        const Bytes* telemetry_field = message.fields_get(Bytes(&telem_key, 1));
        if (telemetry_field) {
            has_telemetry = true;
            char log_buf[128];
            snprintf(log_buf, sizeof(log_buf), "[TELEM-RX] Got telemetry field from %s (%d bytes)",
                     source_hex.c_str(), (int)telemetry_field->size());
            pyxis_log(log_buf);
            Telemetry::LocationData loc = Telemetry::decode_telemetry(*telemetry_field);
            if (loc.valid) {
                snprintf(log_buf, sizeof(log_buf), "[TELEM-RX] Location: %.6f, %.6f", loc.lat, loc.lon);
                pyxis_log(log_buf);
                _telemetry_manager.on_location_received(message.source_hash(), loc);
                if (_current_screen == SCREEN_MAP) {
                    update_map_peer_markers();
                }
            } else {
                pyxis_log("[TELEM-RX] Decode failed — location invalid");
            }
        }
    }

    {
        uint8_t meta_key = Telemetry::FIELD_COLUMBA_META;
        const Bytes* meta_field = message.fields_get(Bytes(&meta_key, 1));
        if (meta_field) {
            has_cease = true;
            if (Telemetry::decode_columba_cease(*meta_field)) {
                _telemetry_manager.on_cease_received(message.source_hash());
                if (_current_screen == SCREEN_MAP) {
                    update_map_peer_markers();
                }
            }
        }
    }

    // If message has telemetry/cease fields but no body content, treat as
    // telemetry-only — skip chat, notification, and message store
    bool is_telemetry_only = (has_telemetry || has_cease) && message.content().size() == 0;

    if (is_telemetry_only) {
        INFO("  Telemetry-only message — skipping chat/store/notification");
        return;
    }

    // Normal message flow: save, display, notify
    _store.save_message(message);

    bool viewing_this_chat = (_current_screen == SCREEN_CHAT && _current_peer_hash == message.source_hash());
    if (viewing_this_chat) {
        _chat_screen->add_message(message, false);
    }

    if (_settings_screen) {
        const auto& settings = _settings_screen->get_settings();
        if (settings.notification_sound && !viewing_this_chat) {
            Notification::tone_play(1000, 100, settings.notification_volume);  // 1kHz beep, 100ms
        }
    }

    // Update conversation list unread count
    // TODO: Track unread counts
    _conversation_list_screen->refresh();

    INFO("  Message processed");
}

void UIManager::on_message_delivered(::LXMF::LXMessage& message) {
    LVGL_LOCK();
    std::string hash_hex = message.hash().toHex().substr(0, 8);
    std::string msg = "Message delivered: " + hash_hex + "...";
    INFO(msg.c_str());

    // Update UI if we're viewing this conversation
    if (_current_screen == SCREEN_CHAT && _current_peer_hash == message.destination_hash()) {
        _chat_screen->update_message_status(message.hash(), true);
    }
}

void UIManager::on_message_failed(::LXMF::LXMessage& message) {
    LVGL_LOCK();
    std::string hash_hex = message.hash().toHex().substr(0, 8);
    std::string msg = "Message delivery failed: " + hash_hex + "...";
    WARNING(msg.c_str());

    // Update UI if we're viewing this conversation
    if (_current_screen == SCREEN_CHAT && _current_peer_hash == message.destination_hash()) {
        _chat_screen->update_message_status(message.hash(), false);
    }
}

void UIManager::refresh_current_screen() {
    LVGL_LOCK();
    switch (_current_screen) {
        case SCREEN_CONVERSATION_LIST:
            _conversation_list_screen->refresh();
            break;
        case SCREEN_CHAT:
            _chat_screen->refresh();
            break;
        case SCREEN_COMPOSE:
            // No refresh needed
            break;
        case SCREEN_ANNOUNCES:
            _announce_list_screen->refresh();
            break;
        case SCREEN_STATUS:
            _status_screen->refresh();
            break;
        case SCREEN_SETTINGS:
            _settings_screen->refresh();
            break;
        case SCREEN_PROPAGATION_NODES:
            _propagation_nodes_screen->refresh();
            break;
        case SCREEN_CALL:
            break;
        case SCREEN_QR:
            break;
        case SCREEN_MAP:
            break;
    }
}

// ── Telemetry / Location Sharing ──

void UIManager::send_telemetry(const Bytes& peer_hash) {
    if (!_gps) {
        pyxis_log("[TELEM] No GPS object, skipping send");
        return;
    }
    if (!_gps->location.isValid()) {
        char gps_buf[128];
        snprintf(gps_buf, sizeof(gps_buf),
            "[TELEM] No GPS fix (sats=%d, age=%lu, lat=%.4f, lon=%.4f)",
            (int)_gps->satellites.value(),
            _gps->location.age(),
            _gps->location.lat(),
            _gps->location.lng());
        pyxis_log(gps_buf);
        return;
    }

    Telemetry::LocationData loc;
    loc.lat = _gps->location.lat();
    loc.lon = _gps->location.lng();
    loc.altitude = _gps->altitude.isValid() ? _gps->altitude.meters() : 0.0;
    loc.speed = _gps->speed.isValid() ? _gps->speed.mps() : 0.0;
    loc.bearing = _gps->course.isValid() ? _gps->course.deg() : 0.0;
    loc.accuracy = _gps->hdop.isValid() ? _gps->hdop.hdop() * 5.0 : 0.0;  // HDOP to approx meters
    loc.timestamp = (uint32_t)Utilities::OS::time();

    Bytes telemetry_data = Telemetry::encode_telemetry(loc);

    // Find session to get end_time for Columba meta
    uint32_t end_time = 0;
    for (const auto& session : _telemetry_manager.get_sessions()) {
        if (session.peer_hash == peer_hash) {
            end_time = session.end_time;
            break;
        }
    }

    // Create a telemetry-only LXMF message (empty content)
    Destination source = _router.delivery_destination();
    Bytes content_bytes;
    Bytes title;

    Identity dest_identity = Identity::recall(peer_hash);
    Destination destination(Type::NONE);
    if (dest_identity) {
        destination = Destination(dest_identity, Type::Destination::OUT,
                                  Type::Destination::SINGLE, "lxmf", "delivery");
    }

    ::LXMF::LXMessage message(destination, source, content_bytes, title);
    message.set_method(::LXMF::Type::Message::OPPORTUNISTIC);
    if (!dest_identity) {
        message.destination_hash(peer_hash);
    }

    // Set telemetry field
    uint8_t telem_key = Telemetry::FIELD_TELEMETRY;
    message.fields_set(Bytes(&telem_key, 1), telemetry_data);

    // Set Columba meta field (expires in milliseconds to match Columba convention)
    uint64_t expires_ms = (uint64_t)end_time * 1000ULL;
    Bytes meta = Telemetry::encode_columba_meta(expires_ms, 0, false);
    uint8_t meta_key = Telemetry::FIELD_COLUMBA_META;
    message.fields_set(Bytes(&meta_key, 1), meta);

    message.pack();

    {
        char log_buf[128];
        snprintf(log_buf, sizeof(log_buf),
            "[TELEM] Sent to %.8s (%.4f,%.4f) packed=%zu",
            peer_hash.toHex().c_str(), loc.lat, loc.lon,
            message.packed().size());
        pyxis_log(log_buf);
    }

    _router.handle_outbound(message);
}

void UIManager::send_cease(const Bytes& peer_hash) {
    Destination source = _router.delivery_destination();
    Bytes content_bytes;
    Bytes title;

    Identity dest_identity = Identity::recall(peer_hash);
    Destination destination(Type::NONE);
    if (dest_identity) {
        destination = Destination(dest_identity, Type::Destination::OUT,
                                  Type::Destination::SINGLE, "lxmf", "delivery");
    }

    ::LXMF::LXMessage message(destination, source, content_bytes, title);
    message.set_method(::LXMF::Type::Message::OPPORTUNISTIC);
    if (!dest_identity) {
        message.destination_hash(peer_hash);
    }

    Bytes meta = Telemetry::encode_columba_meta(0, 0, true);
    uint8_t cease_meta_key = Telemetry::FIELD_COLUMBA_META;
    message.fields_set(Bytes(&cease_meta_key, 1), meta);

    message.pack();
    _router.handle_outbound(message);

    INFO("Sent cease to peer");
}

void UIManager::on_location_share_requested(const Bytes& peer_hash, int duration_index) {
    if (duration_index == 5) {
        // Stop sharing
        send_cease(peer_hash);
        _telemetry_manager.stop_sharing(peer_hash);
        _chat_screen->set_sharing_state(false);
        return;
    }

    Telemetry::ShareDuration duration;
    switch (duration_index) {
        case 0: duration = Telemetry::ShareDuration::MINUTES_15; break;
        case 1: duration = Telemetry::ShareDuration::HOURS_1; break;
        case 2: duration = Telemetry::ShareDuration::HOURS_4; break;
        case 3: duration = Telemetry::ShareDuration::UNTIL_MIDNIGHT; break;
        case 4: duration = Telemetry::ShareDuration::INDEFINITE; break;
        default: return;
    }

    _telemetry_manager.start_sharing(peer_hash, duration);
    _chat_screen->set_sharing_state(true);

    // Send initial telemetry immediately
    send_telemetry(peer_hash);
}

void UIManager::update_map_peer_markers() {
    if (!_map_screen) return;

    const auto& locs = _telemetry_manager.get_received_locations();
    if (locs.empty()) return;

    // Convert to MapScreen::PeerLocation array with display names
    std::vector<MapScreen::PeerLocation> peer_locs;
    peer_locs.reserve(locs.size());
    for (const auto& loc : locs) {
        MapScreen::PeerLocation pl;
        pl.peer_hash = loc.peer_hash;
        pl.lat = loc.lat;
        pl.lon = loc.lon;
        pl.timestamp = loc.timestamp;

        // Resolve display name from announce app_data
        Bytes app_data = RNS::Identity::recall_app_data(loc.peer_hash);
        if (app_data && app_data.size() > 0) {
            String display_name = ConversationListScreen::parse_display_name(app_data);
            if (display_name.length() > 0) {
                pl.name = display_name.c_str();
            }
        }

        peer_locs.push_back(pl);
    }

    _map_screen->update_peer_locations(peer_locs.data(), peer_locs.size());
}

// ── LXST Voice Call Implementation ──

// NVS breadcrumb for crash debugging (survives reboot, unlike USB CDC output)
static void lxst_breadcrumb(uint8_t step, uint32_t heap) {
    Preferences prefs;
    prefs.begin("lxst_dbg", false);
    prefs.putUChar("step", step);
    prefs.putUInt("heap", heap);
    prefs.putUInt("stack", (unsigned)uxTaskGetStackHighWaterMark(nullptr) * 4);
    prefs.end();
}

void UIManager::call_initiate(const Bytes& peer_hash) {
    {
        std::string h = peer_hash.toHex().substr(0, 16);
        INFO(("LXST: Initiating call to " + h + "...").c_str());
    }
    lxst_breadcrumb(1, ESP.getFreeHeap());

    // Check heap before attempting — Link establishment needs ~10KB for crypto
    size_t free_heap = ESP.getFreeHeap();
    if (free_heap < 40000) {
        char buf[64];
        snprintf(buf, sizeof(buf), "LXST: Insufficient heap (%u bytes), aborting call", (unsigned)free_heap);
        WARNING(buf);
        return;
    }

    _call_peer_hash = peer_hash;
    _call_muted = false;
    s_call_instance = this;

    lxst_breadcrumb(2, ESP.getFreeHeap());

    // Look up peer identity
    Identity peer_identity = Identity::recall(peer_hash);
    if (!peer_identity) {
        WARNING("LXST: Peer identity not known, cannot establish link");
        s_call_instance = nullptr;
        return;
    }

    lxst_breadcrumb(3, ESP.getFreeHeap());

    // Create LXST destination for the peer (aspect: lxst.telephony)
    Destination peer_dest(peer_identity, Type::Destination::OUT,
                          Type::Destination::SINGLE, "lxst", "telephony");

    lxst_breadcrumb(4, ESP.getFreeHeap());

    // Show call screen
    _call_screen->set_peer(peer_dest.hash());
    _call_screen->set_state(CallScreen::CallState::CONNECTING);
    _call_screen->set_muted(false);
    _call_screen->show();
    _chat_screen->hide();
    _current_screen = SCREEN_CALL;

    lxst_breadcrumb(5, ESP.getFreeHeap());

    _call_dest_hash = peer_dest.hash();
    _call_audio_rx_count = 0;
    _call_audio_tx_count = 0;
    _call_link_closed_pending = false;
    _call_signal_write = 0;
    _call_signal_read = 0;

    {
        std::string dh = peer_dest.hash().toHex().substr(0, 16);
        bool has_path = Transport::has_path(peer_dest.hash());
        char buf[80];
        snprintf(buf, sizeof(buf), "LXST: Dest hash=%s path=%s", dh.c_str(), has_path ? "yes" : "no");
        INFO(buf);
    }

    if (Transport::has_path(peer_dest.hash())) {
        // Path known — create link immediately
        INFO("LXST: Creating link...");
        _call_link = Link(peer_dest, on_call_link_established, on_call_link_closed);
        _call_state = CallState::LINK_ESTABLISHING;
        _call_timeout_ms = millis() + 30000;
        INFO("LXST: Link establishing, 30s timeout");
    } else {
        // Path unknown — request and wait for it in call_update()
        INFO("LXST: No path, requesting (10s timeout)...");
        Transport::request_path(peer_dest.hash());
        _call_state = CallState::PATH_REQUESTING;
        _call_timeout_ms = millis() + 10000;
    }

    lxst_breadcrumb(7, ESP.getFreeHeap());
}

void UIManager::call_hangup() {
    INFO("LXST: Hanging up");

    // Set IDLE first — prevents pump_call_tx() (which runs without LVGL lock)
    // from accessing _lxst_audio after we delete it.
    _call_state = CallState::IDLE;
    s_call_instance = nullptr;

    // Stop audio
    if (_lxst_audio) {
        _lxst_audio->stopCapture();
        _lxst_audio->stopPlayback();
        _lxst_audio->deinit();
        delete _lxst_audio;
        _lxst_audio = nullptr;
    }

    // Teardown link
    if (_call_link) {
        _call_link.teardown();
        _call_link = Link(Type::NONE);
    }

    _call_peer_hash = Bytes();

    // Return to chat screen
    if (_call_screen) {
        _call_screen->set_state(CallScreen::CallState::ENDED);
    }

    // Brief delay then return to conversation list
    show_conversation_list();
}

void UIManager::call_set_mute(bool muted) {
    _call_muted = muted;
    if (_lxst_audio) {
        _lxst_audio->setCaptureMute(muted);
    }
    INFO(muted ? "LXST: Mic muted" : "LXST: Mic unmuted");
}

void UIManager::call_send_signal(int signal) {
    if (!_call_link || _call_link.status() != Type::Link::ACTIVE) return;

    // Msgpack: {0x00: [signal]}
    // fixmap(1) + key(0) + fixarray(1) + msgpack-encoded integer
    uint8_t msgpack_buf[7];
    int len;

    msgpack_buf[0] = 0x81;  // fixmap(1)
    msgpack_buf[1] = 0x00;  // key: FIELD_SIGNAL
    msgpack_buf[2] = 0x91;  // fixarray(1)

    if (signal <= 0x7F) {
        // fixint: single byte
        msgpack_buf[3] = (uint8_t)signal;
        len = 4;
    } else if (signal <= 0xFF) {
        // uint8: 0xCC + byte
        msgpack_buf[3] = 0xCC;
        msgpack_buf[4] = (uint8_t)signal;
        len = 5;
    } else {
        // uint16: 0xCD + big-endian 2 bytes
        msgpack_buf[3] = 0xCD;
        msgpack_buf[4] = (uint8_t)(signal >> 8);
        msgpack_buf[5] = (uint8_t)(signal & 0xFF);
        len = 6;
    }

    try {
        Bytes signal_data(msgpack_buf, len);
        Packet packet(_call_link, signal_data);
        packet.send();

        char buf[48];
        snprintf(buf, sizeof(buf), "LXST: Sent signal 0x%03X", signal);
        DEBUG(buf);
    } catch (const std::exception& e) {
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "LXST: Signal send exception: %s", e.what());
        WARNING(dbg);
    }
}

void UIManager::call_send_audio_batch(const uint8_t* batch_data, int batch_len,
                                      int batch_count, int total_frames) {
    if (!_call_link || _call_link.status() != Type::Link::ACTIVE) {
        if (_call_audio_tx_count == 0) {
            char dbg[64];
            snprintf(dbg, sizeof(dbg), "LXST: TX drop: link=%p status=%d",
                     (void*)&_call_link, _call_link ? (int)_call_link.status() : -99);
            WARNING(dbg);
        }
        return;
    }

    // Match LXST-kt (Columba) wire format exactly:
    //   {0x01: bin8(batch)} for single batch, or
    //   {0x01: fixarray(N)[bin8(b1), bin8(b2), ...]} for multiple batches.
    // Each batch = [codec_type(0x02)] + [mode_header] + [10 * raw_codec2].
    // Columba's native ring buffer expects exactly frameSamples (1600) decoded
    // samples per writeEncodedPacket call.  For Codec2 3200: 10 * 160 = 1600.
    // batch_data contains batch_count concatenated batches of 82 bytes each.
    static constexpr int BATCH_BYTES = 82;  // codec_type(1) + mode(1) + 10*8

    uint8_t packet_buf[256];
    int pos = 0;

    packet_buf[pos++] = 0x81;  // fixmap(1)
    packet_buf[pos++] = 0x01;  // key: FIELD_FRAMES

    if (batch_count == 1) {
        // Single batch: bare bin8
        packet_buf[pos++] = 0xC4;               // bin8
        packet_buf[pos++] = (uint8_t)BATCH_BYTES;
        memcpy(packet_buf + pos, batch_data, BATCH_BYTES);
        pos += BATCH_BYTES;
    } else {
        // Multiple batches: fixarray(N) of bin8 entries
        packet_buf[pos++] = 0x90 | (uint8_t)batch_count;  // fixarray(N), N≤15
        for (int b = 0; b < batch_count; b++) {
            packet_buf[pos++] = 0xC4;               // bin8
            packet_buf[pos++] = (uint8_t)BATCH_BYTES;
            memcpy(packet_buf + pos, batch_data + b * BATCH_BYTES, BATCH_BYTES);
            pos += BATCH_BYTES;
        }
    }

    // Hex dump first TX packet for wire format verification
    if (_call_audio_tx_count < 2) {
        char hex[128];
        int hpos = 0;
        for (int i = 0; i < pos && i < 24 && hpos < 120; i++) {
            hpos += snprintf(hex + hpos, 128 - hpos, "%02X ", packet_buf[i]);
        }
        char dbg[196];
        snprintf(dbg, sizeof(dbg), "LXST: TX wire[%d] %d batches %d frames: %s",
                 pos, batch_count, total_frames, hex);
        INFO(dbg);
    }

    try {
        Bytes audio_data(packet_buf, pos);
        Packet packet(_call_link, audio_data);
        packet.send();
    } catch (const std::exception& e) {
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "LXST: TX send exception: %s", e.what());
        WARNING(dbg);
    }
}

void UIManager::call_rx_audio_frame(const uint8_t* frame, size_t frame_len) {
    // Guard: packets can arrive after hangup from the network pipeline
    if (!_lxst_audio || _call_state == CallState::IDLE) return;

    // Wire format: [codec_type_byte] + [mode_header + codec2_subframes...]
    // codec_type: 0x00=Raw, 0x01=Opus, 0x02=Codec2 (matches LXST Codecs/__init__.py)
    // For Codec2: mode_header (0x00-0x06) + raw sub-frames
    uint8_t codec_type = frame[0];
    const uint8_t* codec_data = frame + 1;
    size_t codec_data_len = frame_len - 1;

    if (codec_type != LXST_CODEC_CODEC2) {
        if (_call_audio_rx_count == 0) {
            char dbg[64];
            snprintf(dbg, sizeof(dbg), "LXST: RX codec=0x%02X (need 0x02=Codec2), dropping",
                     codec_type);
            WARNING(dbg);
        }
        return;  // Can't decode Opus (0x01) or Raw (0x00) — only Codec2
    }

    if (_lxst_audio && _lxst_audio->isPlaying()) {
        _lxst_audio->writeEncodedPacket(codec_data, codec_data_len);
        _call_audio_rx_count++;
        if (_call_audio_rx_count <= 3) {
            char dbg[80];
            snprintf(dbg, sizeof(dbg), "LXST: RX audio #%lu mode=0x%02X len=%d",
                     (unsigned long)_call_audio_rx_count, codec_data[0], (int)codec_data_len);
            INFO(dbg);
        }
    } else if (_call_audio_rx_count == 0) {
        WARNING("LXST: RX audio dropped (playback not active)");
    }
}

void UIManager::call_on_packet(const Bytes& data) {
    // NOTE: This runs on the Reticulum transport thread (during reticulum->loop()),
    // NOT under the LVGL lock. Do NOT touch LVGL objects here.
    // Signals are queued and processed in call_update() under the LVGL lock.
    {
        char dbg[64];
        snprintf(dbg, sizeof(dbg), "LXST: call_on_packet len=%d state=%d", (int)data.size(), (int)_call_state);
        DEBUG(dbg);
    }
    if (data.size() < 4) return;

    const uint8_t* buf = data.data();

    // Expect msgpack fixmap(1): 0x81
    if (buf[0] != 0x81) {
        char dbg[64];
        snprintf(dbg, sizeof(dbg), "LXST: Invalid packet (0x%02X, expected fixmap)", buf[0]);
        DEBUG(dbg);
        return;
    }

    uint8_t field = buf[1];

    if (field == 0x00) {
        // Signalling: {0x00: [signal]}
        // fixarray(1) = 0x91, then signal is a msgpack integer:
        //   0x00-0x7F = fixint (1 byte)
        //   0xCC XX   = uint8  (2 bytes)
        //   0xCD XX XX = uint16 (3 bytes)
        if (buf[2] != 0x91) return;

        int signal = -1;
        if (buf[3] <= 0x7F) {
            // fixint: value is the byte itself
            signal = buf[3];
        } else if (buf[3] == 0xCC && data.size() >= 5) {
            // uint8
            signal = buf[4];
        } else if (buf[3] == 0xCD && data.size() >= 6) {
            // uint16 (big-endian)
            signal = ((int)buf[4] << 8) | buf[5];
        }

        if (signal < 0) {
            char dbg[64];
            snprintf(dbg, sizeof(dbg), "LXST: Unparseable signal (0x%02X), %d bytes", buf[3], (int)data.size());
            WARNING(dbg);
            return;
        }

        // Handle PREFERRED_PROFILE signals (0xFF+)
        // Remote sends PREFERRED_PROFILE + profile_id to request a codec profile.
        // Pyxis only supports Codec2, so respond with LBW (Codec2 3200bps).
        if (signal >= LXST_PREFERRED_PROFILE) {
            int remote_profile = signal - LXST_PREFERRED_PROFILE;
            char dbg[64];
            snprintf(dbg, sizeof(dbg), "LXST: Remote prefers profile 0x%02X, responding LBW (Codec2)",
                     remote_profile);
            INFO(dbg);
            // Send our preferred profile (LBW = Codec2 3200bps)
            call_send_signal(LXST_PREFERRED_PROFILE + LXST_PROFILE_LBW);
            return;
        }

        {
            char dbg[48];
            snprintf(dbg, sizeof(dbg), "LXST: Received signal 0x%02X (queued)", signal);
            INFO(dbg);
        }

        // Enqueue for processing in call_update() under LVGL lock
        uint8_t w = _call_signal_write;
        uint8_t next_w = (w + 1) % SIGNAL_QUEUE_SIZE;
        if (next_w != _call_signal_read) {  // Not full
            _call_signal_queue[w] = (uint8_t)signal;
            _call_signal_write = next_w;
        } else {
            WARNING("LXST: Signal queue full, dropping signal!");
        }

    } else if (field == 0x01) {
        // Audio: {0x01: value} where value is either:
        //   - bin8/bin16: single frame (codec_header + frame_data)
        //   - fixarray: batched frames [bin8(...), bin8(...), ...]
        // Audio buffer writes don't touch LVGL — safe to process here

        if ((_call_state != CallState::ACTIVE && _call_state != CallState::CONNECTING)
            || !_lxst_audio) {
            return;
        }

        uint8_t fmt = buf[2];

        if ((fmt & 0xF0) == 0x90) {
            // fixarray: batched frames — Columba sends up to 3 per packet
            int array_len = fmt & 0x0F;
            size_t pos = 3;  // start after fixarray byte

            for (int i = 0; i < array_len; i++) {
                if (pos >= data.size()) break;

                size_t frame_len;
                size_t frame_start;

                if (buf[pos] == 0xC4) {
                    // bin8
                    if (pos + 1 >= data.size()) break;
                    frame_len = buf[pos + 1];
                    frame_start = pos + 2;
                } else if (buf[pos] == 0xC5) {
                    // bin16
                    if (pos + 2 >= data.size()) break;
                    frame_len = ((size_t)buf[pos + 1] << 8) | buf[pos + 2];
                    frame_start = pos + 3;
                } else {
                    // Unknown format in array — skip rest
                    break;
                }

                if (frame_start + frame_len > data.size() || frame_len < 2) break;

                call_rx_audio_frame(buf + frame_start, frame_len);
                pos = frame_start + frame_len;
            }
        } else if (fmt == 0xC4) {
            // bin8: single frame
            if (data.size() < 5) return;
            size_t frame_len = buf[3];
            if (data.size() < 4 + frame_len || frame_len < 2) return;
            call_rx_audio_frame(buf + 4, frame_len);
        } else if (fmt == 0xC5) {
            // bin16: single frame
            if (data.size() < 6) return;
            size_t frame_len = ((size_t)buf[3] << 8) | buf[4];
            if (data.size() < 5 + frame_len || frame_len < 2) return;
            call_rx_audio_frame(buf + 5, frame_len);
        }
    }
}

// Process received signal — runs under LVGL lock from call_update()
void UIManager::call_process_signal(uint8_t signal) {
    {
        char dbg[48];
        snprintf(dbg, sizeof(dbg), "LXST: Processing signal 0x%02X (state=%d)", signal, (int)_call_state);
        INFO(dbg);
    }

    switch (_call_state) {
        case CallState::WAIT_AVAILABLE:
            if (signal == LXST_STATUS_AVAILABLE) {
                INFO("LXST: Remote is available, identifying...");
                _call_link.identify(_router.identity());
                _call_state = CallState::WAIT_RINGING;
                _call_timeout_ms = millis() + 15000;
            } else if (signal == LXST_STATUS_BUSY) {
                INFO("LXST: Remote is busy");
                call_ended();
            }
            break;

        case CallState::WAIT_RINGING:
            if (signal == LXST_STATUS_RINGING) {
                INFO("LXST: Remote is ringing");
                // Tell remote we need Codec2 (LBW = 3200bps)
                call_send_signal(LXST_PREFERRED_PROFILE + LXST_PROFILE_LBW);
                _call_state = CallState::RINGING;
                _call_timeout_ms = millis() + 60000;
                _call_screen->set_state(CallScreen::CallState::RINGING);
            } else if (signal == LXST_STATUS_BUSY || signal == LXST_STATUS_REJECTED) {
                INFO("LXST: Call rejected or busy");
                call_ended();
            }
            break;

        case CallState::RINGING:
            if (signal == LXST_STATUS_CONNECTING) {
                INFO("LXST: Remote is connecting audio...");
                _call_state = CallState::CONNECTING;
                lxst_breadcrumb(20, ESP.getFreeHeap());

                if (!_lxst_audio) {
                    _lxst_audio = new LXSTAudio();
                }
                lxst_breadcrumb(21, ESP.getFreeHeap());
                if (!_lxst_audio->init(CODEC2_MODE_3200)) {
                    WARNING("LXST: Audio init failed");
                    call_ended();
                    return;
                }
                lxst_breadcrumb(22, ESP.getFreeHeap());
                // Start full-duplex audio (mic + speaker)
                if (!_lxst_audio->startFullDuplex()) {
                    WARNING("LXST: Full-duplex start failed");
                }
                lxst_breadcrumb(23, ESP.getFreeHeap());

            } else if (signal == LXST_STATUS_ESTABLISHED) {
                INFO("LXST: Call established!");
                _call_state = CallState::ACTIVE;
                _call_start_ms = millis();
                _call_screen->set_state(CallScreen::CallState::ACTIVE);
                lxst_breadcrumb(24, ESP.getFreeHeap());

                if (!_lxst_audio) {
                    _lxst_audio = new LXSTAudio();
                    if (!_lxst_audio->init(CODEC2_MODE_3200)) {
                        WARNING("LXST: Audio init failed");
                        call_ended();
                        return;
                    }
                }
                lxst_breadcrumb(25, ESP.getFreeHeap());
                if (!_lxst_audio->isPlaying()) {
                    if (!_lxst_audio->startFullDuplex()) {
                        WARNING("LXST: Full-duplex start failed");
                    }
                }
                lxst_breadcrumb(26, ESP.getFreeHeap());
                INFO("LXST: Call active (caller, full-duplex)");

            } else if (signal == LXST_STATUS_REJECTED) {
                INFO("LXST: Call rejected");
                call_ended();
            }
            break;

        case CallState::CONNECTING:
            if (signal == LXST_STATUS_ESTABLISHED) {
                INFO("LXST: Call established!");
                _call_state = CallState::ACTIVE;
                _call_start_ms = millis();
                _call_screen->set_state(CallScreen::CallState::ACTIVE);

                // Ensure full-duplex is running
                if (_lxst_audio && !_lxst_audio->isPlaying()) {
                    if (!_lxst_audio->startFullDuplex()) {
                        WARNING("LXST: Full-duplex start failed");
                    }
                }
                INFO("LXST: Call active (full-duplex)");
            }
            break;

        default:
            break;
    }
}

void UIManager::call_ended() {
    INFO("LXST: Call ended");

    // Set IDLE first — prevents pump_call_tx() (which runs without LVGL lock)
    // from accessing _lxst_audio after we delete it.
    _call_state = CallState::IDLE;
    s_call_instance = nullptr;

    // Stop audio
    if (_lxst_audio) {
        _lxst_audio->stopCapture();
        _lxst_audio->stopPlayback();
        _lxst_audio->deinit();
        delete _lxst_audio;
        _lxst_audio = nullptr;
    }

    // Teardown link
    if (_call_link) {
        _call_link.teardown();
        _call_link = Link(Type::NONE);
    }

    _call_peer_hash = Bytes();

    _call_screen->set_state(CallScreen::CallState::ENDED);

    // Return to conversation list after brief display
    show_conversation_list();
}

void UIManager::pump_call_tx() {
    if (_call_state == CallState::IDLE) return;
    if (!_lxst_audio || !_lxst_audio->isCapturing()) return;
    if (!_call_link || _call_link.status() != Type::Link::ACTIVE) return;

    int available = _lxst_audio->capturePacketsAvailable();

    // Drain all available batches — this runs on loopTask (core 1)
    // and doesn't touch LVGL, so no lock needed.
    while (available > 0) {
        uint8_t encoded_buf[128];
        int encoded_len = 0;
        if (!_lxst_audio->readEncodedPacket(encoded_buf, sizeof(encoded_buf), &encoded_len)) {
            break;
        }
        if (encoded_len < 2) { available--; continue; }

        // Prepend codec type byte: [0x02] + [encoded: mode_header + 10*8 raw]
        uint8_t batch_data[128];
        batch_data[0] = LXST_CODEC_CODEC2;
        memcpy(batch_data + 1, encoded_buf, encoded_len);
        int batch_len = 1 + encoded_len;

        call_send_audio_batch(batch_data, batch_len, 1, encoded_len / 8);
        _call_audio_tx_count++;
        available--;

        if (_call_audio_tx_count <= 10 || (_call_audio_tx_count % 100 == 0)) {
            char dbg[96];
            snprintf(dbg, sizeof(dbg), "LXST: TX batch #%lu (%d bytes, avail=%d)",
                     (unsigned long)_call_audio_tx_count, batch_len, available);
            INFO(dbg);
        }
    }
}

void UIManager::call_update() {
    uint32_t now = millis();

    // Process deferred link closed (set by Reticulum callback, consumed here under LVGL lock)
    if (_call_link_closed_pending) {
        _call_link_closed_pending = false;
        call_ended();
        return;
    }

    // Process all queued signals (set by Reticulum packet callback, consumed here under LVGL lock)
    while (_call_signal_read != _call_signal_write) {
        uint8_t sig = _call_signal_queue[_call_signal_read];
        _call_signal_read = (_call_signal_read + 1) % SIGNAL_QUEUE_SIZE;
        call_process_signal(sig);
        if (_call_state == CallState::IDLE) return;  // Signal caused call to end
    }

    // Process deferred answer (set by LVGL task, consumed here on main thread)
    if (_call_answer_pending) {
        _call_answer_pending = false;
        call_answer();
    }

    // Show incoming call UI (deferred from link callback to LVGL-safe context)
    if (_call_state == CallState::INCOMING_RINGING && _current_screen != SCREEN_CALL) {
        _call_screen->set_peer(_call_peer_hash);
        _call_screen->set_state(CallScreen::CallState::INCOMING_RINGING);
        _call_screen->set_muted(false);
        _call_screen->show();
        _current_screen = SCREEN_CALL;

        // Play notification tone
        if (_settings_screen) {
            const auto& settings = _settings_screen->get_settings();
            if (settings.notification_sound) {
                Notification::tone_play(800, 200, settings.notification_volume);
            }
        }
    }

    // Poll for path resolution (PATH_REQUESTING state)
    if (_call_state == CallState::PATH_REQUESTING) {
        if (Transport::has_path(_call_dest_hash)) {
            INFO("LXST: Path resolved, creating link...");
            Identity peer_identity = Identity::recall(_call_peer_hash);
            if (!peer_identity) {
                WARNING("LXST: Peer identity lost during path request");
                call_ended();
                return;
            }
            Destination peer_dest(peer_identity, Type::Destination::OUT,
                                  Type::Destination::SINGLE, "lxst", "telephony");
            _call_link = Link(peer_dest, on_call_link_established, on_call_link_closed);
            _call_state = CallState::LINK_ESTABLISHING;
            _call_timeout_ms = millis() + 30000;
            INFO("LXST: Link establishing, 30s timeout");
        }
    }

    // Check timeouts
    if (_call_timeout_ms > 0 && now > _call_timeout_ms) {
        switch (_call_state) {
            case CallState::PATH_REQUESTING:
                WARNING("LXST: Path request timed out");
                call_ended();
                return;
            case CallState::LINK_ESTABLISHING:
                WARNING("LXST: Link establishment timed out");
                call_ended();
                return;
            case CallState::WAIT_AVAILABLE:
            case CallState::WAIT_RINGING:
                WARNING("LXST: Call setup timed out");
                call_ended();
                return;
            case CallState::RINGING:
                WARNING("LXST: Ring timed out (no answer)");
                call_ended();
                return;
            case CallState::INCOMING_RINGING:
                WARNING("LXST: Incoming call timed out (no answer)");
                call_ended();
                return;
            default:
                _call_timeout_ms = 0;  // Clear timeout for active states
                break;
        }
    }

    // Check link health during active/connecting call
    if (_call_state == CallState::ACTIVE || _call_state == CallState::CONNECTING) {
        if (!_call_link || _call_link.status() == Type::Link::CLOSED) {
            WARNING("LXST: Link closed during call");
            call_ended();
            return;
        }

        // Update duration display (ACTIVE only)
        if (_call_state == CallState::ACTIVE) {
            uint32_t duration_secs = (now - _call_start_ms) / 1000;
            _call_screen->set_duration(duration_secs);

            // Periodic audio stats (every 2 seconds)
            if (duration_secs > 0 && duration_secs % 2 == 0 &&
                now - _call_start_ms > duration_secs * 1000 - 500) {
                static uint32_t last_stats_sec = 0;
                if (duration_secs != last_stats_sec) {
                    last_stats_sec = duration_secs;
                    char dbg[128];
                    snprintf(dbg, sizeof(dbg), "LXST: Audio stats: TX=%lu RX=%lu playBuf=%d capAvail=%d state=%d link=%d",
                             (unsigned long)_call_audio_tx_count,
                             (unsigned long)_call_audio_rx_count,
                             _lxst_audio ? _lxst_audio->playbackFramesBuffered() : -1,
                             _lxst_audio ? _lxst_audio->capturePacketsAvailable() : -1,
                             _lxst_audio ? (int)_lxst_audio->state() : -1,
                             _call_link ? (int)_call_link.status() : -99);
                    INFO(dbg);
                }
            }
        }

        // TX pump — also called from main loop without LVGL lock for low latency
        pump_call_tx();
    }
}

// ── Static Link Callbacks ──

void UIManager::on_call_link_established(Link& link) {
    if (!s_call_instance) return;

    char buf[80];
    snprintf(buf, sizeof(buf), "LXST: Outgoing link established (status=%d)", (int)link.status());
    INFO(buf);

    // Update stored link with the established reference and register callbacks
    s_call_instance->_call_link = link;
    s_call_instance->_call_link.set_packet_callback(on_call_link_packet);
    s_call_instance->_call_link.set_link_closed_callback(on_call_link_closed);
    INFO("LXST: Packet callback registered on outgoing link");

    // Transition to waiting for STATUS_AVAILABLE
    s_call_instance->_call_state = CallState::WAIT_AVAILABLE;
    s_call_instance->_call_timeout_ms = millis() + 10000;  // 10s timeout
    INFO("LXST: Waiting for STATUS_AVAILABLE (10s timeout)");
}

void UIManager::on_call_link_closed(Link& link) {
    if (!s_call_instance) return;

    // Ignore stale link closures (e.g. old link teardown completing after new call started)
    if (s_call_instance->_call_link && link != s_call_instance->_call_link) {
        WARNING("LXST: Stale link closed (ignoring)");
        return;
    }

    WARNING("LXST: Link closed (deferred)");

    // Don't call call_ended() here — runs on Reticulum thread without LVGL lock.
    // Defer to call_update() which runs under LVGL lock.
    if (s_call_instance->_call_state != CallState::IDLE) {
        s_call_instance->_call_link_closed_pending = true;
    }
}

void UIManager::on_call_link_packet(const Bytes& plaintext, const Packet& packet) {
    if (!s_call_instance) return;
    s_call_instance->call_on_packet(plaintext);
}

// ── LXST Incoming Call Callbacks ──

void UIManager::on_lxst_link_established(Link& link) {
    if (!s_call_instance) return;
    auto* self = s_call_instance;
    lxst_breadcrumb(10, ESP.getFreeHeap());
    INFO("LXST: Incoming link established");

    if (self->_call_state != CallState::IDLE) {
        // Already in a call — send busy directly on the new link
        INFO("LXST: Busy, rejecting incoming link");
        uint8_t busy_buf[4] = { 0x81, 0x00, 0x91, LXST_STATUS_BUSY };
        Bytes busy_data(busy_buf, 4);
        Packet pkt(link, busy_data);
        pkt.send();
        link.teardown();
        return;
    }

    // Accept the incoming link
    lxst_breadcrumb(11, ESP.getFreeHeap());
    self->_call_link = link;
    self->_call_muted = false;

    // Send STATUS_AVAILABLE
    lxst_breadcrumb(12, ESP.getFreeHeap());
    self->call_send_signal(LXST_STATUS_AVAILABLE);

    // Wait for caller to identify themselves
    lxst_breadcrumb(13, ESP.getFreeHeap());
    link.set_remote_identified_callback(on_lxst_caller_identified);
    link.set_link_closed_callback(on_call_link_closed);
    lxst_breadcrumb(14, ESP.getFreeHeap());
}

void UIManager::on_lxst_caller_identified(const Link& link, const Identity& identity) {
    if (!s_call_instance) return;
    auto* self = s_call_instance;
    lxst_breadcrumb(15, ESP.getFreeHeap());

    std::string hash_hex = identity.hash().toHex().substr(0, 16);
    INFO(("LXST: Caller identified: " + hash_hex + "...").c_str());

    // Store peer info
    self->_call_peer_hash = identity.hash();

    // Set packet callback for signalling + audio on this link
    self->_call_link.set_packet_callback(on_call_link_packet);

    // Send STATUS_RINGING
    self->call_send_signal(LXST_STATUS_RINGING);

    // Transition to incoming ringing — UI will be shown in call_update()
    self->_call_state = CallState::INCOMING_RINGING;
    self->_call_timeout_ms = millis() + 60000;  // 60s ring timeout
    lxst_breadcrumb(16, ESP.getFreeHeap());
}

void UIManager::call_answer() {
    if (_call_state != CallState::INCOMING_RINGING) {
        char buf[64];
        snprintf(buf, sizeof(buf), "LXST: call_answer() skipped, state=%d", (int)_call_state);
        WARNING(buf);
        return;
    }
    INFO("LXST: Answering incoming call");
    _call_audio_rx_count = 0;
    _call_audio_tx_count = 0;

    // Update screen FIRST (before audio init which may block briefly)
    _call_state = CallState::CONNECTING;
    _call_screen->set_state(CallScreen::CallState::ACTIVE);
    _call_screen->set_muted(_call_muted);

    // Send STATUS_CONNECTING
    call_send_signal(LXST_STATUS_CONNECTING);

    // Initialize audio pipeline
    lxst_breadcrumb(30, ESP.getFreeHeap());
    if (!_lxst_audio) {
        _lxst_audio = new LXSTAudio();
    }
    lxst_breadcrumb(31, ESP.getFreeHeap());
    if (!_lxst_audio->init(CODEC2_MODE_3200)) {
        WARNING("LXST: Audio init failed");
        call_ended();
        return;
    }
    lxst_breadcrumb(32, ESP.getFreeHeap());

    // Start full-duplex audio (mic + speaker)
    if (!_lxst_audio->startFullDuplex()) {
        WARNING("LXST: Full-duplex start failed");
    }
    lxst_breadcrumb(33, ESP.getFreeHeap());

    // Send profile preference: LBW (Codec2 3200bps) — answerer sends last and "wins"
    call_send_signal(LXST_PREFERRED_PROFILE + LXST_PROFILE_LBW);

    // Send STATUS_ESTABLISHED
    call_send_signal(LXST_STATUS_ESTABLISHED);

    // Transition to active call
    _call_state = CallState::ACTIVE;
    _call_start_ms = millis();
    INFO("LXST: Call active (answerer, full-duplex)");
}

void UIManager::announce_lxst() {
    if (_lxst_destination) {
        _lxst_destination.announce();
    }
}

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
