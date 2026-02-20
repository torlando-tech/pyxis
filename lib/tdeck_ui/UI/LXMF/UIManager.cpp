// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "UIManager.h"

#ifdef ARDUINO

#include <lvgl.h>
#include <Preferences.h>
#include "Log.h"
#include "tone/Tone.h"
#include "../LVGL/LVGLLock.h"
#include "lxst_audio.h"
#include "Packet.h"
#include "Transport.h"
#include "Destination.h"

using namespace RNS;

// NVS keys for propagation settings
static const char* NVS_NAMESPACE = "propagation";
static const char* KEY_AUTO_SELECT = "auto_select";
static const char* KEY_NODE_HASH = "node_hash";

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
      _propagation_manager(nullptr),
      _ble_interface(nullptr),
      _initialized(false),
      _call_state(CallState::IDLE),
      _lxst_audio(nullptr),
      _call_start_ms(0),
      _call_timeout_ms(0),
      _call_muted(false),
      _call_answer_pending(false),
      _call_link_closed_pending(false),
      _call_signal_pending(0xFF) {
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
            INFO("Sending LXMF announce");
            _router.announce();
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

    // Set identity and LXMF address on settings screen
    _settings_screen->set_identity_hash(_router.identity().hash());
    _settings_screen->set_lxmf_address(_router.delivery_destination().hash());

    // Set up callback for status button in conversation list
    _conversation_list_screen->set_status_callback(
        [this]() { show_status(); }
    );

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

    _current_screen = SCREEN_CONVERSATION_LIST;
}

void UIManager::show_chat(const Bytes& peer_hash) {
    LVGL_LOCK();
    std::string hash_hex = peer_hash.toHex().substr(0, 8);
    std::string msg = "Showing chat with peer " + hash_hex + "...";
    INFO(msg.c_str());

    _current_peer_hash = peer_hash;

    _chat_screen->load_conversation(peer_hash, _store);
    _chat_screen->show();
    _conversation_list_screen->hide();
    _compose_screen->hide();
    _announce_list_screen->hide();
    _status_screen->hide();
    _settings_screen->hide();
    _propagation_nodes_screen->hide();
    if (_call_screen) _call_screen->hide();

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

    _current_screen = SCREEN_ANNOUNCES;
}

void UIManager::show_status() {
    LVGL_LOCK();
    INFO("Showing status screen");

    _status_screen->refresh();
    _status_screen->show();
    _conversation_list_screen->hide();
    _chat_screen->hide();
    _compose_screen->hide();
    _announce_list_screen->hide();
    _settings_screen->hide();
    _propagation_nodes_screen->hide();

    _current_screen = SCREEN_STATUS;
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
    if (_conversation_list_screen) {
        _conversation_list_screen->set_gps(gps);
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

    // Save to NVS
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool(KEY_AUTO_SELECT, false);
    prefs.putBytes(KEY_NODE_HASH, node_hash.data(), node_hash.size());
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
        prefs.remove(KEY_NODE_HASH);  // Clear saved node when auto-select enabled
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

    // Save to store
    _store.save_message(message);

    // Update UI if we're viewing this conversation
    bool viewing_this_chat = (_current_screen == SCREEN_CHAT && _current_peer_hash == message.source_hash());
    if (viewing_this_chat) {
        _chat_screen->add_message(message, false);
    }

    // Play notification sound if enabled and not viewing this conversation
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
    }
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

    {
        std::string dh = peer_dest.hash().toHex().substr(0, 16);
        bool has_path = Transport::has_path(peer_dest.hash());
        char buf[80];
        snprintf(buf, sizeof(buf), "LXST: Dest hash=%s path=%s", dh.c_str(), has_path ? "yes" : "no");
        INFO(buf);
    }

    // Request path if not cached
    if (!Transport::has_path(peer_dest.hash())) {
        INFO("LXST: No path to destination, requesting...");
        Transport::request_path(peer_dest.hash());
    }

    // Establish Reticulum Link to peer's LXST destination
    INFO("LXST: Creating link...");
    _call_link = Link(peer_dest, on_call_link_established, on_call_link_closed);

    lxst_breadcrumb(6, ESP.getFreeHeap());

    _call_state = CallState::LINK_ESTABLISHING;
    _call_timeout_ms = millis() + 30000;
    INFO("LXST: Link establishing, 30s timeout");

    lxst_breadcrumb(7, ESP.getFreeHeap());
}

void UIManager::call_hangup() {
    INFO("LXST: Hanging up");

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

    _call_state = CallState::IDLE;
    _call_peer_hash = Bytes();
    s_call_instance = nullptr;

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
}

void UIManager::call_send_signal(uint8_t signal) {
    if (!_call_link || _call_link.status() != Type::Link::ACTIVE) return;

    // Msgpack: {0x00: [signal]} = fixmap(1) + key(0) + fixarray(1) + signal
    uint8_t msgpack_buf[4] = { 0x81, 0x00, 0x91, signal };
    Bytes signal_data(msgpack_buf, 4);
    Packet packet(_call_link, signal_data);
    packet.send();

    char buf[48];
    snprintf(buf, sizeof(buf), "LXST: Sent signal 0x%02X", signal);
    DEBUG(buf);
}

void UIManager::call_send_audio(const uint8_t* data, int length) {
    if (!_call_link || _call_link.status() != Type::Link::ACTIVE) return;

    // Msgpack: {0x01: bin8(codec_header + frame_data)}
    uint8_t packet_buf[256];
    int total_len = 1 + length;  // codec header + frame data
    if (total_len > 250 || total_len < 1) return;

    packet_buf[0] = 0x81;                  // fixmap(1)
    packet_buf[1] = 0x01;                  // key: FIELD_FRAMES
    packet_buf[2] = 0xC4;                  // bin8
    packet_buf[3] = (uint8_t)total_len;    // length
    packet_buf[4] = LXST_CODEC_CODEC2;     // codec header
    memcpy(packet_buf + 5, data, length);

    Bytes audio_data(packet_buf, 5 + length);
    Packet packet(_call_link, audio_data);
    packet.send();
}

void UIManager::call_on_packet(const Bytes& data) {
    // NOTE: This runs on the Reticulum transport thread (during reticulum->loop()),
    // NOT under the LVGL lock. Do NOT touch LVGL objects here.
    // Signals are queued and processed in call_update() under the LVGL lock.
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
        // Signalling: {0x00: [signal]} = 81 00 91 XX
        if (buf[2] != 0x91) return;
        uint8_t signal = buf[3];

        char dbg[48];
        snprintf(dbg, sizeof(dbg), "LXST: Received signal 0x%02X (queued)", signal);
        DEBUG(dbg);

        // Queue for processing in call_update() under LVGL lock
        _call_signal_pending = signal;

    } else if (field == 0x01) {
        // Audio: {0x01: bin8/bin16(codec_header + frame_data)}
        // Audio buffer writes don't touch LVGL — safe to process here
        size_t frame_offset;
        size_t frame_len;

        if (buf[2] == 0xC4) {
            // bin8
            if (data.size() < 5) return;
            frame_len = buf[3];
            frame_offset = 4;
        } else if (buf[2] == 0xC5) {
            // bin16
            if (data.size() < 6) return;
            frame_len = ((size_t)buf[3] << 8) | buf[4];
            frame_offset = 5;
        } else {
            return;
        }

        if (data.size() < frame_offset + frame_len || frame_len < 2) return;

        uint8_t codec = buf[frame_offset];
        const uint8_t* frame_data = buf + frame_offset + 1;
        size_t frame_data_len = frame_len - 1;

        if ((_call_state == CallState::ACTIVE || _call_state == CallState::CONNECTING)
            && codec == LXST_CODEC_CODEC2 && _lxst_audio) {
            if (_lxst_audio->state() == LXSTAudio::State::PLAYING) {
                _lxst_audio->writeEncodedPacket(frame_data, frame_data_len);
            }
        }
    }
}

// Process received signal — runs under LVGL lock from call_update()
void UIManager::call_process_signal(uint8_t signal) {
    char dbg[48];
    snprintf(dbg, sizeof(dbg), "LXST: Processing signal 0x%02X (state=%d)", signal, (int)_call_state);
    DEBUG(dbg);

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

                if (!_lxst_audio) {
                    _lxst_audio = new LXSTAudio();
                }
                if (!_lxst_audio->init(CODEC2_MODE_1600)) {
                    WARNING("LXST: Audio init failed");
                    call_ended();
                    return;
                }
                // Start playback (RX) so we can hear the remote
                if (!_lxst_audio->startPlayback()) {
                    WARNING("LXST: Playback start failed");
                }

            } else if (signal == LXST_STATUS_ESTABLISHED) {
                INFO("LXST: Call established!");
                _call_state = CallState::ACTIVE;
                _call_start_ms = millis();
                _call_screen->set_state(CallScreen::CallState::ACTIVE);

                if (!_lxst_audio) {
                    _lxst_audio = new LXSTAudio();
                    if (!_lxst_audio->init(CODEC2_MODE_1600)) {
                        WARNING("LXST: Audio init failed");
                        call_ended();
                        return;
                    }
                }
                if (_lxst_audio->state() != LXSTAudio::State::PLAYING) {
                    if (!_lxst_audio->startPlayback()) {
                        WARNING("LXST: Playback start failed");
                    }
                }
                INFO("LXST: Call active (caller, RX mode)");

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

                // Ensure playback is running
                if (_lxst_audio && _lxst_audio->state() != LXSTAudio::State::PLAYING) {
                    if (!_lxst_audio->startPlayback()) {
                        WARNING("LXST: Playback start failed");
                    }
                }
                INFO("LXST: Call active (RX mode)");
            }
            break;

        default:
            break;
    }
}

void UIManager::call_ended() {
    INFO("LXST: Call ended");

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

    _call_state = CallState::IDLE;
    _call_peer_hash = Bytes();
    s_call_instance = nullptr;

    _call_screen->set_state(CallScreen::CallState::ENDED);

    // Return to conversation list after brief display
    show_conversation_list();
}

void UIManager::call_update() {
    uint32_t now = millis();

    // Process deferred link closed (set by Reticulum callback, consumed here under LVGL lock)
    if (_call_link_closed_pending) {
        _call_link_closed_pending = false;
        call_ended();
        return;
    }

    // Process deferred signal (set by Reticulum packet callback, consumed here under LVGL lock)
    uint8_t pending_sig = _call_signal_pending;
    if (pending_sig != 0xFF) {
        _call_signal_pending = 0xFF;
        call_process_signal(pending_sig);
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

    // Check timeouts
    if (_call_timeout_ms > 0 && now > _call_timeout_ms) {
        switch (_call_state) {
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

    // Check link health during active call
    if (_call_state == CallState::ACTIVE) {
        if (!_call_link || _call_link.status() == Type::Link::CLOSED) {
            WARNING("LXST: Link closed during active call");
            call_ended();
            return;
        }

        // Update duration display
        uint32_t duration_secs = (now - _call_start_ms) / 1000;
        _call_screen->set_duration(duration_secs);

        // Pump TX: read encoded packets from capture and send over link
        if (_lxst_audio && _lxst_audio->state() == LXSTAudio::State::CAPTURING) {
            uint8_t encoded_buf[64];
            int encoded_len = 0;
            // Send up to a few packets per update cycle
            for (int i = 0; i < 4; i++) {
                if (_lxst_audio->readEncodedPacket(encoded_buf, sizeof(encoded_buf), &encoded_len)) {
                    call_send_audio(encoded_buf, encoded_len);
                } else {
                    break;
                }
            }
        }
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

    // Transition to waiting for STATUS_AVAILABLE
    s_call_instance->_call_state = CallState::WAIT_AVAILABLE;
    s_call_instance->_call_timeout_ms = millis() + 10000;  // 10s timeout
    INFO("LXST: Waiting for STATUS_AVAILABLE (10s timeout)");
}

void UIManager::on_call_link_closed(Link& link) {
    if (!s_call_instance) return;
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

    // Update screen FIRST (before audio init which may block briefly)
    _call_state = CallState::CONNECTING;
    _call_screen->set_state(CallScreen::CallState::ACTIVE);
    _call_screen->set_muted(_call_muted);

    // Send STATUS_CONNECTING
    call_send_signal(LXST_STATUS_CONNECTING);

    // Initialize audio pipeline
    if (!_lxst_audio) {
        _lxst_audio = new LXSTAudio();
    }
    if (!_lxst_audio->init(CODEC2_MODE_1600)) {
        WARNING("LXST: Audio init failed");
        call_ended();
        return;
    }

    // Start playback (RX) so we can hear the caller
    if (!_lxst_audio->startPlayback()) {
        WARNING("LXST: Playback start failed");
    }

    // Send STATUS_ESTABLISHED
    call_send_signal(LXST_STATUS_ESTABLISHED);

    // Transition to active call
    _call_state = CallState::ACTIVE;
    _call_start_ms = millis();
    INFO("LXST: Call active (answerer, RX mode)");
}

void UIManager::announce_lxst() {
    if (_lxst_destination) {
        _lxst_destination.announce();
    }
}

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
