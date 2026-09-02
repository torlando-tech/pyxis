// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "UIManager.h"
#include "RouterLock.h"

#ifdef ARDUINO

#include <lvgl.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <microReticulum/Log.h>
#include <microReticulum/Utilities/OS.h>
#ifdef PYXIS_TEST_HOOKS
#include "pyxis_test_hooks.h"
#endif
#include "Tone.h"
#include "../LVGL/LVGLLock.h"
#include "lxst_audio.h"
#include "LXSTSignalParser.h"
#include "ULBWVoiceProfilePolicy.h"
#include <microReticulum/Packet.h>
#include <microReticulum/Resource.h>
#include <microReticulum/Transport.h>
#include <microReticulum/Destination.h>
#include <microReticulum/Utilities/OS.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include "UnknownSourceKeyRequest.h"
#include <cstring>
#include <new>
#include <utility>

using namespace RNS;

// Arm/disarm the decoded-PCM dump used by the audio-loopback test mode.
// Defined in src/main.cpp (owns the UDP multicast socket).
extern "C" void pyxis_audio_dump_arm(bool on);

// NVS keys for propagation settings
static const char* NVS_NAMESPACE = "propagation";
static const char* KEY_AUTO_SELECT = "auto_select";
static const char* KEY_NODE_HASH = "node_hash";
static const char* KEY_STAMP_COST = "stamp_cost";

namespace UI {
namespace LXMF {

namespace {

constexpr const char* NOMAD_LIBRARY_DIR = "/nomadnet";
constexpr const char* NOMAD_LIBRARY_PATH = "/nomadnet/library.pxnn";
constexpr const char* NOMAD_LIBRARY_TMP = "/nomadnet/library.tmp";
constexpr const char* NOMAD_LIBRARY_STAGE = "/nomadnet/library.new";
constexpr const char* NOMAD_LIBRARY_BACKUP = "/nomadnet/library.bak";
constexpr const char* NOMAD_LIBRARY_OLD = "/nomadnet/library.old";

bool bytes_equal_lower_hex(const Bytes& bytes, const std::string& hex) {
    static constexpr char DIGITS[] = "0123456789abcdef";
    if (hex.size() != bytes.size() * 2U) return false;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const uint8_t value = bytes[index];
        if (hex[index * 2U] != DIGITS[value >> 4U] ||
                hex[index * 2U + 1U] != DIGITS[value & 0x0fU])
            return false;
    }
    return true;
}

#if defined(MEMORY_INSTRUMENTATION_ENABLED) || defined(PYXIS_NOMAD_MEMORY_DIAGNOSTIC)
void nomad_heap_checkpoint(const char* phase) {
    Serial.printf(
        "T:NOMAD_HEAP phase=%s free=%u largest=%u minimum=%u psram=%u\n",
        phase,
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}
#else
void nomad_heap_checkpoint(const char*) {}
#endif

bool read_nomad_library_file(const char* path, NomadNet::ExternalVector<uint8_t>& bytes) {
    File file = LittleFS.open(path, "r");
    if (!file) return false;
    const std::size_t size = file.size();
    if (size == 0 || size > NomadNet::Library::MAX_ENCODED_BYTES) {
        file.close();
        return false;
    }
    bytes.resize(size);
    const std::size_t read = file.read(bytes.data(), size);
    file.close();
    return read == size;
}

lv_obj_t* storage_error_dialog = nullptr;

void close_storage_error(lv_event_t* event) {
    lv_obj_t* message_box = lv_event_get_current_target(event);
    if (lv_msgbox_get_active_btn(message_box) == 0) {
        storage_error_dialog = nullptr;
        lv_msgbox_close(message_box);
    }
}

void show_storage_error(const char* message) {
    // Coalesce receive bursts into one dialog rather than allocating one per
    // rejected packet while storage remains unavailable.
    if (storage_error_dialog) return;
    static const char* buttons[] = {"OK", ""};
    storage_error_dialog = lv_msgbox_create(
        nullptr, "Message not saved", message, buttons, false
    );
    lv_obj_center(storage_error_dialog);
    lv_obj_add_event_cb(storage_error_dialog, close_storage_error, LV_EVENT_VALUE_CHANGED, nullptr);
}

std::vector<uint8_t> token(const RNS::Bytes& bytes) {
    if (!bytes || bytes.size() == 0) return {};
    return std::vector<uint8_t>(bytes.data(), bytes.data() + bytes.size());
}

struct OutboundPersistenceContext {
    ::LXMF::MessageStore* store;
    ::LXMF::LXMessage* message;
};

bool persistOutgoingMessage(void* raw_context) {
    auto& context = *static_cast<OutboundPersistenceContext*>(raw_context);
    return context.store->save_message(*context.message);
}

}  // namespace

// Static singletons for Link callbacks
UIManager* UIManager::s_call_instance = nullptr;
UIManager* UIManager::s_nomad_instance = nullptr;

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

class NomadNetAnnounceHandler : public AnnounceHandler {
public:
    using Callback = std::function<void(const Bytes&, const Bytes&)>;

    explicit NomadNetAnnounceHandler(Callback callback)
        : AnnounceHandler("nomadnetwork.node"), callback_(std::move(callback)) {}

    void received_announce(const Bytes& destination_hash, const Identity& /*identity*/,
                           const Bytes& app_data) override {
        if (callback_) callback_(destination_hash, app_data);
    }

private:
    Callback callback_;
};

uint64_t monotonicMillis() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000LL);
}

// ---------------------------------------------------------------------------
// On-demand identity acquisition for unknown LXMF sources (mirrors
// Sideband's "Query Network For Keys" button).
//
// When an LXMF message arrives from a source whose RNS identity has not been
// learned, the router accepts the message but UIManager::on_message_received
// skips location ingest for unauthenticated senders. Nothing in the firmware
// ever triggered the identity learning that microLXMF's comment at
// LXMRouter.cpp:1267 expects ("signature will be validated later if the
// source identity is learned via announce"). This fires a bounded
// RNS path request — the exact mechanism of Sideband's
// RNS.Transport.request_path — so any peer that already knows the source's
// key (the hub, a phone, or another node) can answer with the cached
// announce. The next message from that peer then validates and plots.
//
// Rate limiting (UnknownSourceKeyRequest.h, host-tested): one automatic
// path request per 30 minutes per unknown identity, with at most
// kMaxTrackedSources open request windows in total. While the table is
// saturated with unexpired windows, new identities are deferred (at most
// one window) instead of evicting someone else's open window, so neither
// an honest user's repeat traffic nor a flood of bogus identities can
// force the network to answer unbounded path requests.
namespace {
UnknownSourceKeyRequestPolicy s_key_request_policy;

void request_key_for_unknown_source(const ::LXMF::LXMessage& message) {
    if (message.unverified_reason() !=
        ::LXMF::Type::Message::SOURCE_UNKNOWN) {
        return;  // Bad signature is not recoverable by asking the network.
    }
    const RNS::Bytes& source_hash = message.source_hash();
    if (source_hash.size() != Telemetry::PEER_ID_SIZE) {
        return;
    }
    const std::string source_hex = source_hash.toHex();
    const uint64_t now = monotonicMillis();
    if (!s_key_request_policy.should_request(source_hex, now)) {
        return;  // Recently requested — wait for the answer to land.
    }
    s_key_request_policy.record_request(source_hex, now);
    INFO(("Requesting network keys for unknown LXMF source " +
          source_hex.substr(0, 8) + "...").c_str());
    Transport::request_path(source_hash);
}
}  // namespace

bool peerIdFromHash(const Bytes& hash, Telemetry::PeerId& output) {
    if (hash.size() != Telemetry::PEER_ID_SIZE) return false;
    std::memcpy(output.bytes, hash.data(), Telemetry::PEER_ID_SIZE);
    return true;
}

template <typename T, typename... Args>
T* allocateLocationObject(Args&&... args) {
    void* memory = heap_caps_calloc(
        1, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory ? new (memory) T(std::forward<Args>(args)...) : nullptr;
}

template <typename T>
void releaseLocationObject(T*& object) {
    if (!object) return;
    object->~T();
    heap_caps_free(object);
    object = nullptr;
}

class LiveLocationEnvelopeRouter : public Telemetry::LocationEnvelopeRouter {
public:
    explicit LiveLocationEnvelopeRouter(::LXMF::LXMRouter& router)
        : router_(router) {}

    bool queue(
        const Telemetry::OutboundLocationEnvelope& envelope,
        uint64_t exclusive_deadline_monotonic_millis,
        uint64_t& ownership_monotonic_millis) override {
        RouterLock router_lock;
        if (!router_lock.acquired()) return false;
        try {
            Bytes destination_hash(
                envelope.destination.bytes, Telemetry::PEER_ID_SIZE);
            Identity destination_identity = Identity::recall(destination_hash);
            Destination destination(Type::NONE);
            if (destination_identity) {
                destination = Destination(
                    destination_identity, Type::Destination::OUT,
                    Type::Destination::SINGLE, "lxmf", "delivery");
            }

            ::LXMF::LXMessage message(
                destination,
                router_.delivery_destination(),
                Bytes(),
                Bytes(),
                ::LXMF::Type::Message::OPPORTUNISTIC);
            if (!destination_identity) {
                message.destination_hash(destination_hash);
            }
            for (std::size_t index = 0; index < envelope.field_count; ++index) {
                const auto& field = envelope.fields[index];
                if (!message.fields_set(
                        Bytes(field.key, field.key_size),
                        Bytes(field.value, field.value_size))) {
                    return false;
                }
            }

            GuardContext guard_context{
                exclusive_deadline_monotonic_millis,
                &ownership_monotonic_millis};
            return router_.try_handle_outbound(
                       message, claimOwnership, &guard_context) ==
                   ::LXMF::OutboundAdmissionResult::ACCEPTED;
        } catch (const std::exception& error) {
            WARNINGF("Location outbound preparation failed: %s", error.what());
            return false;
        }
    }

private:
    struct GuardContext {
        uint64_t deadline;
        uint64_t* ownership_time;
    };

    static bool claimOwnership(void* raw_context) {
        auto& context = *static_cast<GuardContext*>(raw_context);
        const uint64_t now = monotonicMillis();
        if (now >= context.deadline) return false;
        *context.ownership_time = now;
        return true;
    }

    ::LXMF::LXMRouter& router_;
};

// LoRa bandwidth is a production constraint: Pyxis advertises and accepts only
// LXST ULBW/Codec2-700C.
int UIManager::_preferred_profile = UIManager::LXST_PROFILE_ULBW;

int UIManager::profile_to_codec2_mode(int profile) {
    return ULBWVoiceProfilePolicy::codecModeForProfile(profile);
}

UIManager::UIManager(Reticulum& reticulum, ::LXMF::LXMRouter& router,
                     ::LXMF::MessageStore& store,
                     bool location_filesystem_available)
    : _reticulum(reticulum), _router(router), _store(store),
      _location_storage(nullptr),
      _location_transaction(nullptr),
      _location_persistence_controller(nullptr),
      _gps(nullptr),
      // Vanilla upstream RNS::Destination has no default ctor; construct in
      // a Type::NONE state, then assign a real Destination later. (The fork
      // had a default ctor that pyxis was implicitly relying on.)
      _lxst_destination(RNS::Type::NONE),
      _home_screen(nullptr),
      _network_screen(nullptr),
      _nomadnet_screen(nullptr),
      _conversation_list_screen(nullptr),
      _chat_screen(nullptr),
      _compose_screen(nullptr),
      _announce_list_screen(nullptr),
      _status_screen(nullptr),
      _radio_activity_screen(nullptr),
      _qr_screen(nullptr),
      _settings_screen(nullptr),
      _propagation_nodes_screen(nullptr),
      _location_share_screen(nullptr),
      _call_screen(nullptr),
      _map_screen(nullptr),
      _propagation_manager(nullptr),
      _ble_interface(nullptr),
      _initialized(false),
      _nomad_cache(_nomad_storage),
      _nomad_cache_flow(_nomad_cache),
      _call_state(CallState::IDLE),
      _call_loopback(false),
      _lxst_audio(nullptr),
      _call_start_ms(0),
      _call_timeout_ms(0),
      _call_muted(false),
      _call_answer_pending(false),
      _call_signal_write(0),
      _call_signal_read(0),
      _call_audio_rx_count(0),
      _call_audio_tx_count(0),
      _pending_conversation_refresh(false),
      _last_conversation_refresh_ms(0) {
    memset((void*)_call_signal_queue, 0, sizeof(_call_signal_queue));
    _location_storage = allocateLocationObject<
        Telemetry::LocationPersistenceLittleFS>(location_filesystem_available);
    if (_location_storage) {
        _location_transaction = allocateLocationObject<
            Telemetry::TransactionalLocationPersistence>(*_location_storage);
    }
    if (_location_transaction) {
        _location_persistence_controller = allocateLocationObject<
            Telemetry::LocationPersistenceController>(
                _location_shares, _peer_locations, *_location_transaction);
    }
    if (!_location_persistence_controller) {
        ERROR("Location persistence allocation failed; sharing remains disabled");
    }
}

UIManager::~UIManager() {
    // Joins the sole SD/decoder worker before any map buffers are released.
    if (_map_screen) delete _map_screen;
    _map_screen = nullptr;
    releaseLocationObject(_location_persistence_controller);
    releaseLocationObject(_location_transaction);
    releaseLocationObject(_location_storage);
    // Clean up call state
    if (_call_state != CallState::IDLE ||
        call_current_generation() != 0) {
        call_hangup();
    }
    call_teardown_audio();

    // This pointer owns the long-lived incoming-destination callback, not only
    // the current call. Clear it only when the manager itself is destroyed.
    if (s_call_instance == this) s_call_instance = nullptr;
    if (s_nomad_instance == this) s_nomad_instance = nullptr;
    if (_nomad_announce_handler) {
        RouterLock router_lock;
        if (router_lock.acquired()) {
            Transport::deregister_announce_handler(_nomad_announce_handler);
            _nomad_announce_handler = HAnnounceHandler();
        }
    }
    nomad_stop_transport();

    if (_home_screen) delete _home_screen;
    if (_network_screen) delete _network_screen;
    if (_nomadnet_screen) delete _nomadnet_screen;
    if (_conversation_list_screen) delete _conversation_list_screen;
    if (_chat_screen) delete _chat_screen;
    if (_compose_screen) delete _compose_screen;
    if (_announce_list_screen) delete _announce_list_screen;
    if (_status_screen) delete _status_screen;
    if (_radio_activity_screen) delete _radio_activity_screen;
    if (_qr_screen) delete _qr_screen;
    if (_settings_screen) delete _settings_screen;
    if (_propagation_nodes_screen) delete _propagation_nodes_screen;
    if (_location_share_screen) delete _location_share_screen;
    if (_call_screen) delete _call_screen;
}

bool UIManager::init() {
    if (_initialized) {
        return true;
    }
    // Read the bounded NomadNet index before taking the render lock. A corrupt
    // primary never replaces the in-memory library; the backup is tried next.
    nomad_load_library();
    LVGL_LOCK();

    INFO("Initializing UIManager");

    // Create all screens
    _home_screen = new HomeScreen();
    _network_screen = new NetworkScreen();
    _nomadnet_screen = new NomadNetScreen();
    _conversation_list_screen = new ConversationListScreen();
    _chat_screen = new ChatScreen();
    _compose_screen = new ComposeScreen();
    _announce_list_screen = new AnnounceListScreen();
    _status_screen = new StatusScreen();
    _radio_activity_screen = new RadioActivityScreen();
    _qr_screen = new QRScreen();
    _settings_screen = new SettingsScreen();
    _propagation_nodes_screen = new PropagationNodesScreen();
    _location_share_screen = new LocationShareScreen();
    _call_screen = new CallScreen();
    _map_screen = new MapScreen();

    _home_screen->set_messages_callback([this]() { show_conversation_list(); });
    _home_screen->set_nomadnet_callback([this]() { show_nomadnet(); });
    _home_screen->set_network_callback([this]() { show_network(); });
    _home_screen->set_settings_callback([this]() { show_settings(); });
    _home_screen->set_map_callback([this]() { show_map(); });
    _network_screen->set_back_callback([this]() { back(); });
    _network_screen->set_home_callback([this]() { home(); });
    _network_screen->set_status_callback([this]() { show_status(); });
    _network_screen->set_radio_activity_callback([this]() { show_radio_activity(); });
    _network_screen->set_propagation_nodes_callback([this]() { show_propagation_nodes(); });
    _nomadnet_screen->set_back_callback([this]() { _nomad_actions.publish(NomadNet::UserActionKind::BACK, {}); });
    _nomadnet_screen->set_home_callback([this]() { _nomad_actions.publish(NomadNet::UserActionKind::HOME, {}); });
    _nomadnet_screen->set_reload_callback([this](const std::string& address) {
        return _nomad_actions.publish(NomadNet::UserActionKind::RELOAD, address);
    });
    _nomadnet_screen->set_open_callback([this](const std::string& address) {
        return _nomad_actions.publish(NomadNet::UserActionKind::OPEN, address);
    });
    _nomadnet_screen->set_link_callback([this](const std::string& target) {
        return _nomad_actions.publish(NomadNet::UserActionKind::OPEN, target);
    });
    _nomadnet_screen->set_submit_callback([this](uint16_t link_id, uint32_t generation) {
        return _nomad_actions.publish_submit(link_id, generation);
    });
    _nomadnet_screen->set_save_callback([this](const std::string& target) {
        return _nomad_actions.publish(NomadNet::UserActionKind::SAVE, target);
    });
    _nomadnet_screen->set_identify_callback([this](const std::string& target, bool identified) {
        return _nomad_actions.publish_identify(target, identified);
    });
    _nomadnet_screen->set_library(_nomad_library);

    // Set up callbacks for conversation list screen
    _conversation_list_screen->set_conversation_selected_callback(
        [this](const Bytes& peer_hash) { on_conversation_selected(peer_hash); }
    );

    _conversation_list_screen->set_compose_callback(
        [this]() { on_new_message(); }
    );

    _map_screen->set_back_callback(
        [this]() { on_back_from_map(); }
    );

    _conversation_list_screen->set_sync_callback(
        [this]() { on_propagation_sync(); }
    );
    _conversation_list_screen->set_home_callback([this]() { home(); });
    _conversation_list_screen->set_peers_callback([this]() { show_announces(); });

    // Set up callbacks for chat screen
    _chat_screen->set_back_callback(
        [this]() { on_back_to_conversation_list(); }
    );

    _chat_screen->set_send_message_callback(
        [this](const String& content) { return on_send_message_from_chat(content); }
    );

    _chat_screen->set_call_callback(
        [this]() { on_call_from_chat(); }
    );
    _chat_screen->set_location_callback(
        [this]() { on_location_from_chat(); }
    );

    _location_share_screen->set_back_callback(
        [this]() { on_back_from_location_sharing(); }
    );
    _location_share_screen->set_start_callback(
        [this](const uint8_t* peer, std::size_t peer_size, uint8_t duration,
               uint32_t cadence, bool approximate, int32_t radius) {
            return _location_share_commands.requestStart(
                peer, peer_size, duration, cadence, approximate, radius);
        }
    );
    _location_share_screen->set_stop_callback(
        [this](const uint8_t* peer, std::size_t peer_size) {
            return _location_share_commands.requestStop(peer, peer_size);
        }
    );

    // Set up callbacks for compose screen
    _compose_screen->set_cancel_callback(
        [this]() { on_cancel_compose(); }
    );

    _compose_screen->set_send_callback(
        [this](const Bytes& dest_hash, const String& message) {
            return on_send_message_from_compose(dest_hash, message);
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
            RouterLock router_lock(0);
            if (!router_lock.acquired()) {
                WARNING("Router busy; announce deferred by user retry");
                return;
            }
            try {
                _router.announce();
                announce_lxst();
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

    _status_screen->set_radio_activity_callback(
        [this]() { show_radio_activity(); }
    );

    _radio_activity_screen->set_back_callback(
        [this]() { on_back_from_radio_activity(); }
    );

    // Set up callbacks for QR screen
    _qr_screen->set_back_callback(
        [this]() { on_back_from_qr(); }
    );

    // Set up callbacks for settings screen
    _settings_screen->set_back_callback(
        [this]() { on_back_from_settings(); }
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
        [this]() { call_request_hangup(); }
    );

    _call_screen->set_mute_callback(
        [this](bool muted) { call_request_mute(muted); }
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
        [this]() {
            _call_answer_pending.store(true, std::memory_order_release);
        }
    );

    // Load conversations; outer launcher is the application root.
    _conversation_list_screen->load_conversations(_store);
    show_home();

    // Create LXST IN destination for incoming voice calls
    _lxst_destination = Destination(_router.identity(), Type::Destination::IN,
                                     Type::Destination::SINGLE, "lxst", "telephony");
    _lxst_destination.set_proof_strategy(Type::Destination::PROVE_NONE);
    _lxst_destination.set_link_established_callback(on_lxst_link_established);
    s_call_instance = this;
    s_nomad_instance = this;

    // Canonical NomadNet records matching announces as they arrive and then
    // persists its bounded stream. Do the same instead of depending on the
    // directory screen being opened before the underlying route expires.
    _nomad_announce_handler = HAnnounceHandler(
        std::make_shared<NomadNetAnnounceHandler>(
        [this](const Bytes& destination_hash, const Bytes& app_data) {
            nomad_hear_node(destination_hash, app_data);
        }));
    {
        RouterLock router_lock;
        if (!router_lock.acquired()) return false;
        Transport::register_announce_handler(_nomad_announce_handler);
    }

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
    // Settings Save only publishes a snapshot from the LVGL event. Persistence
    // and interface changes execute here on the main owner loop, before LVGL.
    if (_settings_screen) _settings_screen->service_pending_save();
    // Flush display-name write-throughs the last conversation-list refresh
    // deferred. Done here, BEFORE LVGL_LOCK, so the microStore/LittleFS I/O
    // never runs under the render lock (same reason as on_message_received).
    if (_conversation_list_screen) {
        _conversation_list_screen->flush_pending_name_writes();
    }

    // SERVICE ORDER CONTRACT: consume peer consent commands and complete the
    // existing controller-owned durable start/stop transaction before location
    // service/dispatch and before LVGL_LOCK. LVGL callbacks perform no I/O.
    const LocationShareCommandMailbox::Command location_command =
        _location_share_commands.take();
    bool has_location_ui_result = false;
    Telemetry::LocationConsentResult location_ui_result =
        Telemetry::LocationConsentResult::INVALID_ARGUMENT;
    Telemetry::ShareSession location_ui_session{};
    bool has_location_ui_session = false;
    if (location_command.action != LocationShareCommandMailbox::Action::NONE) {
        Bytes command_peer(location_command.peer, Telemetry::PEER_ID_SIZE);
        if (location_command.action == LocationShareCommandMailbox::Action::QUERY) {
            const Telemetry::LocationControllerState query_state =
                _location_persistence_controller
                    ? _location_persistence_controller->service(
                          static_cast<uint64_t>(RNS::Utilities::OS::ltime()),
                          monotonicMillis())
                    : Telemetry::LocationControllerState::BLOCKED;
            if (query_state == Telemetry::LocationControllerState::READY) {
                has_location_ui_session = get_location_share_session(
                    command_peer, location_ui_session);
                location_ui_result = has_location_ui_session
                    ? Telemetry::LocationConsentResult::UPDATED
                    : Telemetry::LocationConsentResult::NOT_FOUND;
            } else {
                location_ui_result =
                    query_state == Telemetry::LocationControllerState::WAITING_FOR_CLOCK
                        ? Telemetry::LocationConsentResult::CLOCK_UNAVAILABLE
                        : Telemetry::LocationConsentResult::STORAGE_FAILURE;
            }
        } else if (location_command.action == LocationShareCommandMailbox::Action::STOP) {
            location_ui_result = stop_location_sharing(command_peer);
        } else {
            Telemetry::ShareStartOptions options{};
            bool valid = true;
            switch (location_command.duration) {
                case 0: options.duration = Telemetry::ShareDuration::MINUTES_15; break;
                case 1: options.duration = Telemetry::ShareDuration::HOUR_1; break;
                case 2: options.duration = Telemetry::ShareDuration::HOURS_4; break;
                default: valid = false; break;
            }
            if (location_command.cadenceMillis != 60000U &&
                location_command.cadenceMillis != 300000U &&
                location_command.cadenceMillis != 900000U) valid = false;
            if ((!location_command.hasApproximation && location_command.approximationMeters != 0) ||
                (location_command.hasApproximation &&
                 location_command.approximationMeters != 100 &&
                 location_command.approximationMeters != 1000 &&
                 location_command.approximationMeters != 10000)) valid = false;
            options.cadence_millis = location_command.cadenceMillis;
            options.has_approx_radius = location_command.hasApproximation;
            options.approx_radius_meters = location_command.approximationMeters;
            location_ui_result = valid
                ? start_location_sharing(command_peer, options)
                : Telemetry::LocationConsentResult::INVALID_ARGUMENT;
        }
        has_location_ui_result = true;
        if (location_command.action != LocationShareCommandMailbox::Action::QUERY) {
            has_location_ui_session = get_location_share_session(
                command_peer, location_ui_session);
        }
    }
    // Stream the rest of the open conversation's first page in a few at a time
    // (the newest were rendered synchronously on open). Done here, on the main
    // loop, so each small batch only briefly holds the LVGL lock instead of the
    // whole page blocking it past LVGLLock's 5s timeout.
    if (_navigation.current() == Route::CHAT && _chat_screen) {
        _chat_screen->tick_background_fill();
    }
    // Gather + render the announce list off the LVGL lock on the main loop (its
    // refresh() just arms a flag). Iterating the path table on the LVGL task raced
    // the main-loop path-table writes and blocked past the lock timeout once the
    // table was non-empty — this serializes the gather with the writes instead.
    if (_navigation.current() == Route::ANNOUNCES && _announce_list_screen) {
        _announce_list_screen->tick();
    }
    if (_navigation.current() == Route::SETTINGS && _settings_screen) {
        _settings_screen->tick();  // keep the live clock / GPS / system readouts ticking
    }
    const uint32_t now = millis();
    nomad_update_library();
    nomad_update();
    // Snapshot acquisition is deliberately before LVGL_LOCK and is coalesced
    // to the screen's render cadence. The provider never touches radio or SPI.
    if (_navigation.current() == Route::RADIO_ACTIVITY &&
        _radio_activity_screen && _radio_activity_snapshot_provider &&
        _radio_activity_screen->render_due(now)) {
        const RadioActivity::Snapshot snapshot = _radio_activity_snapshot_provider();
        _radio_activity_screen->render(snapshot, _radio_activity_config, now);
    }

    // Poll one consent-gated location item outside LVGL_LOCK. Packing and the
    // router's synchronous ownership copy may perform crypto work; neither is
    // a rendering operation. With no explicit sessions this remains a no-op.
    const uint64_t wall_now_millis =
        static_cast<uint64_t>(RNS::Utilities::OS::ltime());
    const uint64_t monotonic_now_millis = monotonicMillis();
    const Telemetry::LocationControllerState location_state =
        _location_persistence_controller
            ? _location_persistence_controller->service(
                  wall_now_millis, monotonic_now_millis)
            : Telemetry::LocationControllerState::BLOCKED;
    Telemetry::GpsFixSample gps_sample{};
    if (_gps) {
        gps_sample.location_valid = _gps->location.isValid();
        gps_sample.location_age_millis = _gps->location.age();
        gps_sample.latitude_degrees = _gps->location.lat();
        gps_sample.longitude_degrees = _gps->location.lng();
        gps_sample.altitude_valid = _gps->altitude.isValid();
        gps_sample.altitude_meters = _gps->altitude.meters();
        gps_sample.speed_valid = _gps->speed.isValid();
        gps_sample.speed_kilometers_per_hour = _gps->speed.kmph();
        gps_sample.bearing_valid = _gps->course.isValid();
        gps_sample.bearing_degrees = _gps->course.deg();

    }
    Telemetry::LocationTelemetry current_location{};
    const bool current_location_valid = Telemetry::locationTelemetryFromGpsFix(
        gps_sample, wall_now_millis, current_location);
    Telemetry::DispatchResult location_result = Telemetry::DispatchResult::NO_WORK;
    if (location_state == Telemetry::LocationControllerState::READY) {
        LiveLocationEnvelopeRouter location_router(_router);
        location_result = Telemetry::dispatchLocationShare(
            _location_shares,
            wall_now_millis,
            monotonic_now_millis,
            current_location_valid,
            current_location,
            location_router);
    }
    if (location_result == Telemetry::DispatchResult::QUEUED) {
        INFO("Location telemetry queued");
    } else if (location_result == Telemetry::DispatchResult::CEASE_QUEUED) {
        INFO("Location cease queued");
    }

    // Build the fixed map model and service its worker before LVGL_LOCK. All
    // authenticated ingress drains on this router-owner loop, so this fixed
    // peer snapshot cannot race PeerLocationStore::apply().
    if (_navigation.current() == Route::MAP && _map_screen) {
        Telemetry::PeerLocationRecord peers[Telemetry::MAX_PEER_LOCATIONS]{};
        static constexpr uint64_t MAP_PEER_MAX_AGE_MS =
            24ULL * 60ULL * 60ULL * 1000ULL;
        const std::size_t peer_count = _peer_locations.snapshot(
            wall_now_millis, MAP_PEER_MAX_AGE_MS, peers,
            Telemetry::MAX_PEER_LOCATIONS);
        // App-only diagnostic: edge-triggered so it stays quiet while the
        // map sits idle. Temporary until the missing-map-pin investigation
        // is closed; remove with the fix.
        {
            const std::size_t total = _peer_locations.size();
            const int blocked =
                location_state != Telemetry::LocationControllerState::READY
                    ? 1 : 0;
            static std::size_t last_total = static_cast<std::size_t>(-1);
            static std::size_t last_visible = static_cast<std::size_t>(-1);
            static int last_blocked = -1;
            static bool have_last = false;
            if (!have_last || last_total != total ||
                last_visible != peer_count || last_blocked != blocked) {
                have_last = true;
                last_total = total;
                last_visible = peer_count;
                last_blocked = blocked;
                INFOF(
                    "  Map snapshot: store=%s total=%llu visible=%llu",
                    blocked ? "BLOCKED" : "READY",
                    (unsigned long long)total,
                    (unsigned long long)peer_count);
            }
        }
        Pyxis::MapView::Request map_request{};
        map_request.center = {0.0, 0.0};
        map_request.zoom = 2U;
        map_request.width = Pyxis::MapScreenPresenter::VIEWPORT_WIDTH;
        map_request.height = Pyxis::MapScreenPresenter::VIEWPORT_HEIGHT;
        map_request.include_tile_border = false;
        map_request.has_local_location = current_location_valid;
        map_request.local_location = current_location;
        map_request.peers = peers;
        map_request.peer_count = peer_count;
        map_request.wall_now_millis = wall_now_millis;
        _map_screen->updateModel(map_request);
        _map_screen->serviceIo();
    }
    LVGL_LOCK();

    // Outgoing starts are initiated here while the recursive LVGL mutex is
    // held. Interface workers (notably BLE) can synchronously dispatch
    // Reticulum callbacks on their own tasks, so every call-lifecycle callback
    // takes this same mutex. A response therefore cannot run until Link
    // construction, exact-ID publication, and state assignment finish.
    CallStartMailbox::PeerHash startPeerHash{};
    if (_call_starts.take(startPeerHash)) {
        if (_call_state == CallState::IDLE &&
            call_current_generation() == 0) {
            call_initiate(Bytes(startPeerHash.data(), startPeerHash.size()));
        } else {
            WARNING("LXST: Discarding stale/busy call start request");
        }
    }

    if (_navigation.current() == Route::MAP && _map_screen) {
        // One predecoded completion at most per tick, then fixed-pool positions.
        (void)_map_screen->applyOneCompletion();
        _map_screen->applyFrame();
    }
    if (has_location_ui_result && _location_share_screen &&
        _location_share_screen->matches_peer(
            location_command.peer, Telemetry::PEER_ID_SIZE)) {
        _location_share_screen->apply_result(
            location_ui_result,
            has_location_ui_session ? &location_ui_session : nullptr);
    }
    // Consume UI commands unconditionally. LVGL callbacks only publish into
    // the mailbox; loopTask remains the sole owner of the audio pipeline.
    const uint32_t generation = call_current_generation();
    const CallCommandMailbox::Command command =
        _call_commands.takeForGeneration(generation);
    if (command.action == CallCommandMailbox::Action::HANGUP) {
        call_hangup();
    } else if (command.action == CallCommandMailbox::Action::MUTE) {
        call_set_mute(command.muted);
    }

    // Pump voice call state while a call is active or its generation remains
    // reserved during owner-side teardown.
    if (_call_state != CallState::IDLE ||
        call_current_generation() != 0) {
        call_update();
    }

    // Update status indicators (WiFi/battery) on conversation list
    static uint32_t last_status_update = 0;
    if (now - last_status_update > 3000) {  // Update every 3 seconds
        last_status_update = now;
        if (_conversation_list_screen) {
            _conversation_list_screen->update_status();
        }
        // Update status screen if visible
        if (_navigation.current() == Route::STATUS && _status_screen) {
            _status_screen->refresh();
        }
    }

    // Drain coalesced conversation-list refresh requests. Only when
    // the user is actually viewing the list — if they're in chat or
    // settings, the list rebuilds the next time they navigate back
    // (show_conversation_list already calls refresh()). Throttle to
    // at most once every 750ms even when the screen IS visible so
    // the SPI flush isn't saturated by sustained inbound traffic.
    static constexpr uint32_t COALESCE_MS = 750;
    if (_pending_conversation_refresh
        && _navigation.current() == Route::MESSAGES
        && (now - _last_conversation_refresh_ms) >= COALESCE_MS) {
        _pending_conversation_refresh = false;
        _last_conversation_refresh_ms = now;
        if (_conversation_list_screen) {
            _conversation_list_screen->refresh();
        }
    }
}

void UIManager::hide_all_screens() {
    if (_home_screen) _home_screen->hide();
    if (_network_screen) _network_screen->hide();
    if (_nomadnet_screen) _nomadnet_screen->hide();
    if (_conversation_list_screen) _conversation_list_screen->hide();
    if (_chat_screen) _chat_screen->hide();
    if (_compose_screen) _compose_screen->hide();
    if (_announce_list_screen) _announce_list_screen->hide();
    if (_status_screen) _status_screen->hide();
    if (_radio_activity_screen) _radio_activity_screen->hide();
    if (_qr_screen) _qr_screen->hide();
    if (_settings_screen) _settings_screen->hide();
    if (_propagation_nodes_screen) _propagation_nodes_screen->hide();
    _location_share_screen->hide();
    if (_call_screen) _call_screen->hide();
    if (_map_screen) _map_screen->hide();
}

void UIManager::show_home() {
    home();
}

void UIManager::show_network() {
    navigate(Route::NETWORK);
}

void UIManager::show_nomadnet() {
    nomad_heap_checkpoint("show-enter");
    _nomad_history.clear();
    _nomad_directory_refresh_pending.store(true, std::memory_order_release);
    {
        LVGL_LOCK();
        _nomadnet_screen->show_start();
    }
    navigate(Route::NOMADNET);
    nomad_heap_checkpoint("show-ready");
}

void UIManager::navigate(Route route) {
    const bool leaving_nomadnet = _navigation.current() == Route::NOMADNET && route != Route::NOMADNET;
    if (leaving_nomadnet) {
        nomad_advance_navigation_generation();
        _nomad_cache_flow.cancel();
        _nomad_state = NomadState::IDLE;
        _nomad_mailbox.seal();
    }
    {
        LVGL_LOCK();
        _navigation.navigate(route);
        render_route(route);
    }
    if (leaving_nomadnet) nomad_stop_transport();
}

void UIManager::replace_route(Route route) {
    LVGL_LOCK();
    _navigation.replace(route);
    render_route(route);
}

void UIManager::back() {
    if (_navigation.current() == Route::NOMADNET) {
        _nomad_actions.publish(NomadNet::UserActionKind::BACK, {});
        return;
    }
    LVGL_LOCK();
    if (!_navigation.back()) return;
    render_route(_navigation.current());
}

void UIManager::nomad_back_empty() {
    if (_navigation.current() == Route::NOMADNET) {
        nomad_advance_navigation_generation();
        bool handled = false;
        {
            LVGL_LOCK();
            handled = _nomadnet_screen->handle_library_back();
        }
        if (handled) {
            _nomad_history.clear();
            nomad_stop_transport();
            _nomad_directory_refresh_pending.store(true, std::memory_order_release);
            return;
        }
        _nomad_cache_flow.cancel();
        _nomad_state = NomadState::IDLE;
        _nomad_mailbox.seal();
    }
    const bool leaving_nomadnet = _navigation.current() == Route::NOMADNET;
    {
        LVGL_LOCK();
        if (!_navigation.back()) return;
        render_route(_navigation.current());
    }
    if (leaving_nomadnet) nomad_stop_transport();
}

void UIManager::home() {
    const bool leaving_nomadnet = _navigation.current() == Route::NOMADNET;
    if (leaving_nomadnet) {
        nomad_advance_navigation_generation();
        _nomad_cache_flow.cancel();
        _nomad_state = NomadState::IDLE;
        _nomad_mailbox.seal();
    }
    {
        LVGL_LOCK();
        _navigation.home();
        render_route(Route::HOME);
    }
    if (leaving_nomadnet) nomad_stop_transport();
}

void UIManager::render_route(Route route) {
    // This is the only place that changes screen visibility. Every hide()
    // removes its focus objects before the selected screen adds its own.
    hide_all_screens();
    switch (route) {
        case Route::HOME:
            _home_screen->show();
            break;
        case Route::MESSAGES:
            _conversation_list_screen->refresh();
            _pending_conversation_refresh = false;
            _last_conversation_refresh_ms = millis();
            _conversation_list_screen->show();
            break;
        case Route::MAP:
            _map_screen->show();
            break;
        case Route::LOCATION_SHARING:
            _location_share_screen->show();
            break;
        case Route::CHAT:
            _chat_screen->load_conversation(_current_peer_hash, _store);
            _chat_screen->show();
            break;
        case Route::COMPOSE:
            _compose_screen->clear();
            _compose_screen->show();
            break;
        case Route::NETWORK:
            _network_screen->show();
            break;
        case Route::ANNOUNCES:
            _announce_list_screen->refresh();
            _announce_list_screen->show();
            break;
        case Route::STATUS:
            _status_screen->refresh();
            _status_screen->show();
            break;
        case Route::RADIO_ACTIVITY:
            _radio_activity_screen->show();
            break;
        case Route::QR:
            _qr_screen->show();
            break;
        case Route::PROPAGATION_NODES:
            _propagation_nodes_screen->show();
            break;
        case Route::NOMADNET:
            _nomadnet_screen->show();
            break;
        case Route::SETTINGS:
            _settings_screen->refresh();
            _settings_screen->show();
            break;
        case Route::CALL: if (_call_screen) _call_screen->show(); break;
    }
}

void UIManager::show_conversation_list() {
    INFO("Showing conversation list");
    navigate(Route::MESSAGES);
}

void UIManager::show_chat(const Bytes& peer_hash) {
    std::string hash_hex = peer_hash.toHex().substr(0, 8);
    std::string msg = "Showing chat with peer " + hash_hex + "...";
    INFO(msg.c_str());

    _current_peer_hash = peer_hash;
    navigate(Route::CHAT);
}

void UIManager::show_location_sharing(const Bytes& peer_hash) {
    if (peer_hash.size() != Telemetry::PEER_ID_SIZE ||
        !_current_peer_hash || peer_hash != _current_peer_hash) {
        WARNING("Refusing location controls for invalid or non-current peer");
        return;
    }
    if (!_location_share_screen->open_for_peer(
            peer_hash.data(), peer_hash.size(), nullptr)) {
        WARNING("Refusing location controls: peer must be exactly 16 bytes");
        return;
    }
    if (!_location_share_commands.requestQuery(peer_hash.data(), peer_hash.size())) {
        WARNING("Location controls busy; current status will refresh on retry");
    }
    navigate(Route::LOCATION_SHARING);
}

void UIManager::on_back_from_location_sharing() {
    // _current_peer_hash is retained while controls are open, so Back always
    // returns to the exact chat that opened this peer-scoped screen.
    back();
}

void UIManager::show_compose() {
    INFO("Showing compose screen");
    navigate(Route::COMPOSE);
}

void UIManager::show_announces() {
    INFO("Showing announces screen");
    navigate(Route::ANNOUNCES);
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

    navigate(Route::STATUS);
}

void UIManager::show_radio_activity() {
    INFO("Showing Radio Activity screen");
    navigate(Route::RADIO_ACTIVITY);
}

void UIManager::set_radio_activity_source(
    std::function<RadioActivity::Snapshot()> snapshot_provider,
    const RadioActivityScreen::RadioConfig& config) {
    _radio_activity_snapshot_provider = std::move(snapshot_provider);
    _radio_activity_config = config;
}

void UIManager::on_conversation_selected(const Bytes& peer_hash) {
    show_chat(peer_hash);
}

void UIManager::on_new_message() {
    show_compose();
}

void UIManager::show_map() {
    INFO("Showing offline map");
    navigate(Route::MAP);
}

void UIManager::on_back_from_map() {
    back();
}

void UIManager::show_settings() {
    INFO("Showing settings screen");
    navigate(Route::SETTINGS);
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

    navigate(Route::PROPAGATION_NODES);
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
}

Telemetry::LocationConsentResult UIManager::start_location_sharing(
    const Bytes& peer_hash,
    const Telemetry::ShareStartOptions& options) {
    Telemetry::PeerId peer{};
    if (!peerIdFromHash(peer_hash, peer)) {
        return Telemetry::LocationConsentResult::INVALID_ARGUMENT;
    }
    if (!_location_persistence_controller) {
        return Telemetry::LocationConsentResult::NOT_READY;
    }
    const uint64_t wall_now =
        static_cast<uint64_t>(RNS::Utilities::OS::ltime());
    const uint64_t monotonic_now = monotonicMillis();
    _location_persistence_controller->service(wall_now, monotonic_now);
    return _location_persistence_controller->startSharing(
        peer, options, wall_now, monotonic_now);
}

Telemetry::LocationConsentResult UIManager::stop_location_sharing(
    const Bytes& peer_hash) {
    Telemetry::PeerId peer{};
    if (!peerIdFromHash(peer_hash, peer)) {
        return Telemetry::LocationConsentResult::INVALID_ARGUMENT;
    }
    if (!_location_persistence_controller) {
        return Telemetry::LocationConsentResult::NOT_READY;
    }
    const uint64_t wall_now =
        static_cast<uint64_t>(RNS::Utilities::OS::ltime());
    const uint64_t monotonic_now = monotonicMillis();
    _location_persistence_controller->service(wall_now, monotonic_now);
    return _location_persistence_controller->stopSharing(
        peer, wall_now, monotonic_now);
}

bool UIManager::get_location_share_session(
    const Bytes& peer_hash,
    Telemetry::ShareSession& output) const {
    Telemetry::PeerId peer{};
    return peerIdFromHash(peer_hash, peer) && _location_shares.get(peer, output);
}

void UIManager::on_back_to_conversation_list() {
    back();
}

bool UIManager::on_send_message_from_chat(const String& content) {
    return send_message(_current_peer_hash, content);
}

void UIManager::on_call_from_chat() {
    if (!_current_peer_hash) return;
    if (_current_peer_hash.size() != CallStartMailbox::PeerHash{}.size()) {
        WARNING("LXST: Call peer hash is not exactly 16 bytes");
        return;
    }

    CallStartMailbox::PeerHash peerHash{};
    for (size_t i = 0; i < peerHash.size(); ++i) {
        peerHash[i] = _current_peer_hash[i];
    }
    if (!_call_starts.request(peerHash)) {
        WARNING("LXST: Call start already pending");
    }
}

void UIManager::on_location_from_chat() {
    if (_current_peer_hash.size() != Telemetry::PEER_ID_SIZE) {
        WARNING("Location controls require an exact 16-byte current peer");
        return;
    }
    show_location_sharing(_current_peer_hash);
}

bool UIManager::on_send_message_from_compose(const Bytes& dest_hash, const String& message) {
    if (!send_message(dest_hash, message)) return false;

    // Replace Compose with Chat so Back returns to Messages instead of
    // reopening a cleared compose form.
    _current_peer_hash = dest_hash;
    replace_route(Route::CHAT);
    return true;
}

void UIManager::on_cancel_compose() {
    back();
}

void UIManager::on_announce_selected(const Bytes& dest_hash) {
    std::string hash_hex = dest_hash.toHex().substr(0, 8);
    std::string msg = "Announce selected: " + hash_hex + "...";
    INFO(msg.c_str());

    // Go directly to chat screen with this destination
    show_chat(dest_hash);
}

void UIManager::on_back_from_announces() {
    back();
}

void UIManager::on_back_from_status() {
    back();
}

void UIManager::on_back_from_radio_activity() {
    back();
}

void UIManager::on_share_from_status() {
    navigate(Route::QR);
}

void UIManager::on_back_from_qr() {
    back();
}

void UIManager::on_back_from_settings() {
    back();
}

void UIManager::on_back_from_propagation_nodes() {
    back();
}

void UIManager::on_propagation_node_selected(const Bytes& node_hash) {
    RouterLock router_lock(0);
    if (!router_lock.acquired()) {
        WARNING("Router busy; propagation selection not applied");
        return;
    }
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
    RouterLock router_lock(0);
    if (!router_lock.acquired()) {
        WARNING("Router busy; propagation mode not applied");
        return;
    }
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
    RouterLock router_lock(0);
    if (!router_lock.acquired()) {
        WARNING("Router busy; sync request not queued");
        return;
    }
    INFO("Requesting messages from propagation node");
    _router.request_messages_from_propagation_node();
}

void UIManager::set_rns_status(bool connected, const String& server_name) {
    if (_status_screen) {
        _status_screen->set_rns_status(connected, server_name);
    }
}

bool UIManager::send_message(const Bytes& dest_hash, const String& content) {
    std::string hash_hex = dest_hash.toHex().substr(0, 8);
    std::string msg = "Sending message to " + hash_hex + "...";
    INFO(msg.c_str());

    // Pre-graft: Identity::mark_persistent(dest_hash) — fork-only API for
    // the 5s fast-flush semantics. Vanilla upstream relies on microStore's
    // dirty-tracking + reticulum->should_persist_data() to decide what
    // gets written. If we observe lost contacts after crashes, revisit
    // microStore flush cadence rather than re-adding the fork API.
    // (void)Identity::mark_persistent(dest_hash);

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

    // UI messages prefer single-packet opportunistic delivery on LoRa. The
    // router automatically promotes messages that exceed the LoRa packet MDU
    // to DIRECT, so this preserves large-message support without forcing every
    // short message through the heavier link/resource path.
    ::LXMF::LXMessage message(
        destination,
        source,
        content_bytes,
        title,
        ::LXMF::Type::Message::OPPORTUNISTIC
    );

    // If destination identity was unknown, manually set the destination hash
    if (!dest_identity) {
        message.destination_hash(dest_hash);
        DEBUG("  Set destination hash manually");
    }

    // Pack the message to generate hash and signature before saving
    message.pack();

    // Reject router contention or queue exhaustion before persistence. The
    // admission guard commits the final packed/stamped message immediately
    // before queue ownership transfer while RouterLock prevents a concurrent
    // producer from consuming the checked capacity. This callback runs on
    // LVGL's 8 KiB task, so MessageStore keeps its rollback snapshot
    // object-owned rather than local to save_message().
    RouterLock router_lock(0);
    if (!router_lock.acquired()) {
        WARNING("Router busy; outgoing message retained for retry");
        return false;
    }

    OutboundPersistenceContext persistence_context{&_store, &message};
    ::LXMF::OutboundAdmissionResult admission;
    try {
        admission = _router.try_handle_outbound(
            message, persistOutgoingMessage, &persistence_context);
    } catch (const std::exception& error) {
        WARNINGF("Outgoing message preparation failed: %s", error.what());
        return false;
    }

    if (admission == ::LXMF::OutboundAdmissionResult::GUARD_REJECTED) {
        ERROR("Outgoing message persistence failed; message not queued");
        show_storage_error("Storage is unavailable. The message was not sent.");
        return false;
    }
    if (admission != ::LXMF::OutboundAdmissionResult::ACCEPTED) {
        WARNING("Outbound queue full; outgoing message retained for retry");
        return false;
    }

    if (_navigation.current() == Route::CHAT && _current_peer_hash == dest_hash) {
        _chat_screen->add_message(message, true);
    }

    INFO("  Message queued for delivery");
    return true;
}

void UIManager::on_message_received(::LXMF::LXMessage& message) {
    // Don't take LVGL_LOCK across the LittleFS write — under sustained
    // LXMF receive load (eg propagation soak), LittleFS compaction can
    // stall the save for several seconds. While the lock was held the
    // Arduino loop's `UIManager::update()` would assert at the 5s
    // LVGL_LOCK timeout (LVGLLock.h:45), tipping pyxis into a panic
    // reset. Scope the lock to ONLY the UI mutations below.
    // Drop messages whose source identity is KNOWN but whose signature
    // fails to validate (spoofed or malicious — an attacker forging a
    // legitimate peer's hash). The opportunistic (on_packet) and direct
    // (on_resource_concluded) router paths already reject these, but the
    // propagated (store-and-forward) path — process_propagated_lxmf —
    // queues them without a signature check, so this handler is the only
    // choke point that covers all three. Drop before anything else: no
    // key request, no location ingest, no persistence, no render, no
    // notification.
    //
    // SOURCE_UNKNOWN is NOT dropped here: a first-contact peer's message
    // may be legitimate and the key is still being learned (key request
    // below), so those render as today.
    if (!message.signature_validated() &&
        message.unverified_reason() ==
            ::LXMF::Type::Message::SIGNATURE_INVALID) {
        WARNING(("Dropping message with invalid signature from " +
                 message.source_hash().toHex().substr(0, 8) +
                 "...").c_str());
        return;
    }

    std::string source_hex = message.source_hash().toHex().substr(0, 8);
    std::string msg = "Message received from " + source_hex + "...";
    INFO(msg.c_str());

#ifdef PYXIS_TEST_HOOKS
    pyxis_test_hook_record_rx(message);
#endif

    // First-contact key learning: unauthenticated messages from a source
    // whose identity has not been learned yet (SOURCE_UNKNOWN) are accepted
    // by the router but skipped below for location ingest. Ask the network
    // for the source's key (Sideband "request keys" equivalent) so the NEXT
    // message from this peer can validate and be plotted. Signature-invalid
    // messages were already dropped above.
    if (!message.signature_validated()) {
        request_key_for_unknown_source(message);
    }

    // Classify authenticated location fields before any conversation write.
    // Raw field keys and values are MessagePack spans retained by LXMessage.
    Telemetry::InboundLocationMessage inbound{};
    const RNS::Bytes& source_hash = message.source_hash();
    if (message.signature_validated() &&
        source_hash.size() == Telemetry::PEER_ID_SIZE) {
        std::memcpy(inbound.authenticated_sender.bytes,
                    source_hash.data(), Telemetry::PEER_ID_SIZE);

        static const uint8_t telemetry_key_bytes[] = {
            Telemetry::FIELD_TELEMETRY};
        static const uint8_t custom_meta_key_bytes[] = {
            0xccU, Telemetry::FIELD_CUSTOM_META};
        const RNS::Bytes telemetry_key(
            telemetry_key_bytes, sizeof(telemetry_key_bytes));
        const RNS::Bytes custom_meta_key(
            custom_meta_key_bytes, sizeof(custom_meta_key_bytes));
        const RNS::Bytes* telemetry_value = message.fields_get(telemetry_key);
        const RNS::Bytes* custom_meta_value = message.fields_get(custom_meta_key);
        if (telemetry_value != nullptr) {
            inbound.telemetry.present = true;
            inbound.telemetry.raw_value = Telemetry::BinaryView{
                telemetry_value->data(), telemetry_value->size()};
        }
        if (custom_meta_value != nullptr) {
            inbound.custom_meta.present = true;
            inbound.custom_meta.raw_value = Telemetry::BinaryView{
                custom_meta_value->data(), custom_meta_value->size()};
        }
        const RNS::Bytes& title = message.title();
        const RNS::Bytes& content = message.content();
        inbound.title = Telemetry::TextField(
            true, title.data(), title.size());
        inbound.content = Telemetry::TextField(
            true, content.data(), content.size());
        inbound.received_at_millis = RNS::Utilities::OS::ltime();

        const Telemetry::LocationMessageDecision location_decision =
            Telemetry::classifyInboundLocationMessage(inbound);
        if (location_decision.log_malformed) {
            WARNING("Malformed inbound location field ignored");
        }
        if (location_decision.apply_location) {
            const uint64_t location_wall_now =
                static_cast<uint64_t>(RNS::Utilities::OS::ltime());
            const uint64_t location_monotonic_now = monotonicMillis();
            const Telemetry::LocationControllerState location_state =
                _location_persistence_controller
                    ? _location_persistence_controller->service(
                          location_wall_now, location_monotonic_now)
                    : Telemetry::LocationControllerState::BLOCKED;
            if (location_state == Telemetry::LocationControllerState::READY) {
                const Telemetry::PeerLocationResult location_result =
                    _peer_locations.apply(
                        location_decision.authenticated_sender,
                        location_decision.location,
                        location_decision.meta,
                        location_decision.received_at_millis);
                if (location_result != Telemetry::PeerLocationResult::STALE &&
                    location_result != Telemetry::PeerLocationResult::NOT_FOUND &&
                    location_result != Telemetry::PeerLocationResult::INVALID_ARGUMENT) {
                    _location_persistence_controller->service(
                        location_wall_now, location_monotonic_now);
                }
            } else {
                WARNING("Location state not restored; inbound update ignored");
            }
        }
        if (!location_decision.persist) {
            INFO("  Location telemetry processed without chat persistence");
            return;
        }
    }

    // Pre-graft: RNS::Identity::mark_persistent — fork-only. See note above.
    // (void)RNS::Identity::mark_persistent(message.source_hash());

    // Save to store — no LVGL lock; LittleFS GC is allowed to take its
    // time without freezing the UI thread. Do not present an in-memory-only
    // message as durable conversation history.
    if (!_store.save_message(message)) {
        ERROR("Incoming message persistence failed; message not added to history");
        LVGL_LOCK();
        show_storage_error("An incoming message could not be saved. Check device storage.");
        return;
    }

    // Take the LVGL lock now for the UI-touching code below.
    LVGL_LOCK();

    // Update UI if we're viewing this conversation
    bool viewing_this_chat = (_navigation.current() == Route::CHAT && _current_peer_hash == message.source_hash());
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

    // Coalesce list refreshes — if 50 propagation messages land
    // back-to-back we used to redraw the list 50 times and saturate
    // the SPI flush + serial output. Just flag the pending refresh;
    // call_update() / main loop drains it at most every COALESCE_MS
    // and only when the user is actually on the conversation list.
    _pending_conversation_refresh = true;

    INFO("  Message processed");
}

void UIManager::on_message_delivered(::LXMF::LXMessage& message) {
    std::string hash_hex = message.hash().toHex().substr(0, 8);
    std::string msg = "Message delivered: " + hash_hex + "...";
    INFO(msg.c_str());

    LVGL_LOCK();
    // Update UI if we're viewing this conversation
    if (_navigation.current() == Route::CHAT && _current_peer_hash == message.destination_hash()) {
        _chat_screen->update_message_status(message.hash(), true);
    }
}

void UIManager::on_message_failed(::LXMF::LXMessage& message) {
    std::string hash_hex = message.hash().toHex().substr(0, 8);
    std::string msg = "Message delivery failed: " + hash_hex + "...";
    WARNING(msg.c_str());

    if (!_store.update_message_state(
            message.hash(), ::LXMF::Type::Message::State::FAILED)) {
        ERROR("Failed message state persistence failed");
        LVGL_LOCK();
        show_storage_error("Delivery failure status could not be saved. Check device storage.");
        return;
    }

    LVGL_LOCK();
    // Update UI if we're viewing this conversation
    if (_navigation.current() == Route::CHAT && _current_peer_hash == message.destination_hash()) {
        _chat_screen->update_message_status(message.hash(), false);
    }
}

void UIManager::refresh_current_screen() {
    LVGL_LOCK();
    switch (_navigation.current()) {
        case Route::MESSAGES:
            _conversation_list_screen->refresh();
            break;
        case Route::CHAT:
            _chat_screen->refresh();
            break;
        case Route::COMPOSE:
            // No refresh needed
            break;
        case Route::ANNOUNCES:
            _announce_list_screen->refresh();
            break;
        case Route::STATUS:
            _status_screen->refresh();
            break;
        case Route::SETTINGS:
            _settings_screen->refresh();
            break;
        case Route::PROPAGATION_NODES:
            _propagation_nodes_screen->refresh();
            break;
        case Route::CALL:
            break;
        case Route::QR:
            break;
    }
}

// ── NomadNet browser transport ──

bool UIManager::nomad_load_library() {
    NomadNet::ExternalVector<uint8_t> bytes;
    NomadNet::Library loaded;
    if (read_nomad_library_file(NOMAD_LIBRARY_PATH, bytes) &&
        loaded.decode(bytes.data(), bytes.size())) {
        _nomad_library = std::move(loaded);
        return true;
    }
    bytes.clear();
    if (read_nomad_library_file(NOMAD_LIBRARY_BACKUP, bytes) &&
        loaded.decode(bytes.data(), bytes.size())) {
        _nomad_library = std::move(loaded);
        WARNING("Recovered NomadNet library from backup");
        return true;
    }
    bytes.clear();
    if (read_nomad_library_file(NOMAD_LIBRARY_TMP, bytes) &&
        loaded.decode(bytes.data(), bytes.size())) {
        _nomad_library = std::move(loaded);
        // Primary and backup were already rejected above. Promote the valid
        // interrupted generation now so the next save never deletes its only
        // durable copy before writing a replacement.
        bool promoted = true;
        if (LittleFS.exists(NOMAD_LIBRARY_PATH)) promoted = LittleFS.remove(NOMAD_LIBRARY_PATH);
        if (promoted) promoted = LittleFS.rename(NOMAD_LIBRARY_TMP, NOMAD_LIBRARY_PATH);
        _nomad_library_dirty = !promoted;
        WARNING(promoted ? "Recovered interrupted NomadNet library write" :
                           "Using retained NomadNet temp generation; promotion pending");
        return true;
    }
    bytes.clear();
    if (read_nomad_library_file(NOMAD_LIBRARY_STAGE, bytes) &&
        loaded.decode(bytes.data(), bytes.size())) {
        _nomad_library = std::move(loaded);
        bool promoted = true;
        if (LittleFS.exists(NOMAD_LIBRARY_PATH)) promoted = LittleFS.remove(NOMAD_LIBRARY_PATH);
        if (promoted) promoted = LittleFS.rename(NOMAD_LIBRARY_STAGE, NOMAD_LIBRARY_PATH);
        _nomad_library_dirty = !promoted;
        WARNING(promoted ? "Recovered staged NomadNet library write" :
                           "Using retained NomadNet stage generation; promotion pending");
        return true;
    }
    bytes.clear();
    if (read_nomad_library_file(NOMAD_LIBRARY_OLD, bytes) &&
        loaded.decode(bytes.data(), bytes.size())) {
        _nomad_library = std::move(loaded);
        _nomad_library_dirty = true;
        WARNING("Recovered NomadNet library from retained generation");
        return true;
    }
    // No file is the normal first-boot state. Never format or remove unrelated
    // LittleFS content when the index is absent or malformed.
    return !LittleFS.exists(NOMAD_LIBRARY_PATH) && !LittleFS.exists(NOMAD_LIBRARY_BACKUP);
}

bool UIManager::nomad_save_library() {
    const auto bytes = _nomad_library.encode();
    if (bytes.empty()) return false;
    if (!LittleFS.exists(NOMAD_LIBRARY_DIR) && !LittleFS.mkdir(NOMAD_LIBRARY_DIR)) return false;
    const auto file_valid = [](const char* path) {
        NomadNet::ExternalVector<uint8_t> stored;
        NomadNet::Library parsed;
        return read_nomad_library_file(path, stored) && parsed.decode(stored.data(), stored.size());
    };
    const bool had_primary = LittleFS.exists(NOMAD_LIBRARY_PATH);
    const bool had_backup = LittleFS.exists(NOMAD_LIBRARY_BACKUP);
    const bool primary_valid = file_valid(NOMAD_LIBRARY_PATH);
    const bool backup_valid = file_valid(NOMAD_LIBRARY_BACKUP);
    const bool old_valid = file_valid(NOMAD_LIBRARY_OLD);
    const bool committed_valid = primary_valid || backup_valid || old_valid;
    const bool temp_valid = file_valid(NOMAD_LIBRARY_TMP);
    const bool stage_valid = file_valid(NOMAD_LIBRARY_STAGE);
    // Never overwrite the sole valid recovery generation. Alternate staging
    // names when an interrupted write is the only durable copy.
    const char* stage_path = NOMAD_LIBRARY_TMP;
    if (!committed_valid && temp_valid) stage_path = NOMAD_LIBRARY_STAGE;
    else if (!committed_valid && stage_valid) stage_path = NOMAD_LIBRARY_TMP;

    if (LittleFS.exists(stage_path) && !LittleFS.remove(stage_path)) return false;
    File file = LittleFS.open(stage_path, "w");
    if (!file) return false;
    const std::size_t written = file.write(bytes.data(), bytes.size());
    file.flush();
    file.close();
    if (written != bytes.size()) { LittleFS.remove(stage_path); return false; }

    NomadNet::ExternalVector<uint8_t> verify_bytes;
    NomadNet::Library verified;
    if (!read_nomad_library_file(stage_path, verify_bytes) ||
        verify_bytes.size() != bytes.size() ||
        !std::equal(bytes.begin(), bytes.end(), verify_bytes.begin()) ||
        !verified.decode(verify_bytes.data(), verify_bytes.size())) {
        LittleFS.remove(stage_path);
        return false;
    }

    bool backup_now_valid = backup_valid;
    if (primary_valid) {
        // Retain the previous backup as a third generation until the new
        // primary passes exact read-back. Every interruption point retains at
        // least one prior generation in primary, backup, or old.
        if (LittleFS.exists(NOMAD_LIBRARY_OLD) && !LittleFS.remove(NOMAD_LIBRARY_OLD)) return false;
        if (had_backup && !LittleFS.rename(NOMAD_LIBRARY_BACKUP, NOMAD_LIBRARY_OLD)) return false;
        if (!LittleFS.rename(NOMAD_LIBRARY_PATH, NOMAD_LIBRARY_BACKUP)) {
            if (had_backup) LittleFS.rename(NOMAD_LIBRARY_OLD, NOMAD_LIBRARY_BACKUP);
            return false;
        }
        backup_now_valid = true;
    } else if (had_primary) {
        // Never rotate a corrupt primary over a potentially valid backup or
        // retained generation. The validated temp is already durable enough
        // to promote, so only the known-corrupt primary is discarded.
        if (!LittleFS.remove(NOMAD_LIBRARY_PATH)) return false;
    }
    if (!LittleFS.rename(stage_path, NOMAD_LIBRARY_PATH)) return false;
    verify_bytes.clear();
    if (!read_nomad_library_file(NOMAD_LIBRARY_PATH, verify_bytes) ||
        verify_bytes.size() != bytes.size() ||
        !std::equal(bytes.begin(), bytes.end(), verify_bytes.begin()) ||
        !verified.decode(verify_bytes.data(), verify_bytes.size())) {
        LittleFS.remove(NOMAD_LIBRARY_PATH);
        return false;
    }
    // If recovery came from OLD while backup was corrupt, retain that known
    // good generation as the backup instead of deleting the last prior copy.
    if (!backup_now_valid && old_valid && LittleFS.exists(NOMAD_LIBRARY_OLD)) {
        if (LittleFS.exists(NOMAD_LIBRARY_BACKUP)) LittleFS.remove(NOMAD_LIBRARY_BACKUP);
        backup_now_valid = LittleFS.rename(NOMAD_LIBRARY_OLD, NOMAD_LIBRARY_BACKUP);
    }
    if (backup_now_valid && LittleFS.exists(NOMAD_LIBRARY_OLD)) LittleFS.remove(NOMAD_LIBRARY_OLD);
    const char* stale_stage = stage_path == NOMAD_LIBRARY_TMP ? NOMAD_LIBRARY_STAGE : NOMAD_LIBRARY_TMP;
    if (LittleFS.exists(stale_stage)) LittleFS.remove(stale_stage);
    return true;
}

void UIManager::nomad_hear_node(const Bytes& destination_hash, const Bytes& app_data) {
    const std::string name = app_data
        ? NomadNet::sanitize_directory_name(app_data.data(), app_data.size()) : std::string{};
    if (!_nomad_library.hear_node(destination_hash.toHex(), name,
            static_cast<uint64_t>(RNS::Utilities::OS::time()),
            Transport::hops_to(destination_hash))) return;
    _nomad_library_dirty = true;
    _nomad_directory_refresh_pending.store(true, std::memory_order_release);
}

void UIManager::nomad_refresh_nodes() {
    nomad_heap_checkpoint("refresh-enter");
    bool changed = false;
    const auto& destinations = Transport::path_table();
    for (auto it = destinations.begin(); it != destinations.end(); ++it) {
        const Bytes& destination_hash = it->first;
        // The enumerable mirror in pinned microReticulum is add-only, while
        // has_path() consults the authoritative persistent routable store.
        if (!Transport::has_path(destination_hash)) continue;
        Identity identity = Identity::recall(destination_hash);
        if (!identity) continue;
        if (destination_hash != Destination::hash(identity, "nomadnetwork", "node")) continue;
        const Bytes app_data = Identity::recall_app_data(destination_hash);
        const std::string name = app_data
            ? NomadNet::sanitize_directory_name(app_data.data(), app_data.size()) : std::string{};
        const auto& entry = it->second;
        changed = _nomad_library.hear_node(destination_hash.toHex(), name,
            static_cast<uint64_t>(entry._timestamp), static_cast<uint8_t>(entry._hops)) || changed;
    }
    nomad_heap_checkpoint("refresh-scanned");
    if (!changed) return;
    _nomad_library_dirty = true;
    LVGL_LOCK();
    nomad_heap_checkpoint("refresh-before-library-copy");
    _nomadnet_screen->set_library(_nomad_library);
    nomad_heap_checkpoint("refresh-after-library-copy");
}

void UIManager::nomad_update_library() {
    // Filesystem persistence and directory scans are not required to complete
    // an active Reticulum operation. Dirty/pending flags remain set and are
    // serviced after terminal cleanup returns the state to IDLE.
    if (nomad_operation_active()) return;
    const uint32_t now = millis();
    if (_navigation.current() == Route::NOMADNET && _nomadnet_screen->directory_visible() &&
        now - _nomad_last_directory_refresh_ms >= 10000) {
        _nomad_directory_refresh_pending.store(true, std::memory_order_release);
    }
    if (_nomad_directory_refresh_pending.exchange(false, std::memory_order_acq_rel)) {
        _nomad_last_directory_refresh_ms = now;
        nomad_refresh_nodes();
    }

    if (_nomad_library_dirty && now - _nomad_last_library_save_ms >= 5000) {
        _nomad_last_library_save_ms = now;
        if (nomad_save_library()) _nomad_library_dirty = false;
        else WARNING("Could not persist NomadNet library; preserving existing file");
    }
}

void UIManager::nomad_update_user_actions() {
    class ScreenSubmissionSource final : public NomadNet::OwnerSubmissionSource {
    public:
        explicit ScreenSubmissionSource(NomadNetScreen& screen) : _screen(screen) {}
        bool prepare_submission(uint16_t item_id, uint32_t generation,
                std::string& target, NomadNet::ExternalVector<uint8_t>& bytes,
                NomadNet::FormEncodeResult& result) override {
            LVGL_LOCK();
            return _screen.prepare_submission(item_id, generation, target, bytes, result);
        }
    private:
        NomadNetScreen& _screen;
    } source(*_nomadnet_screen);
    // Back/Home is a terminal slot behind at most CAPACITY retained explicit
    // Saves. Drain that bounded batch in one owner-loop pass so the terminal
    // action cannot sit behind the synchronous library persistence below.
    // Ordinary Open and standalone Save actions remain one-per-pass.
    for (std::size_t serviced = 0; serviced < NomadNet::ActionMailbox::CAPACITY + 1; ++serviced) {
        NomadNet::UserAction action;
        if (!_nomad_actions.pop(action)) return;
        nomad_heap_checkpoint("action-popped");
        const std::string target = action.target();
        nomad_heap_checkpoint("action-target-copied");
        switch (action.kind) {
        case NomadNet::UserActionKind::OPEN:
            nomad_heap_checkpoint("action-before-open");
            nomad_open(target);
            break;
        case NomadNet::UserActionKind::RELOAD:
        case NomadNet::UserActionKind::BACK:
        case NomadNet::UserActionKind::SUBMIT: {
            int32_t current_scroll = 0;
            { LVGL_LOCK(); current_scroll = _nomadnet_screen->logical_scroll(); }
            auto command = _nomad_owner.service(action, _nomad_history, source, current_scroll);
            if (command.result != NomadNet::OwnerResult::REQUEST) {
                if (action.kind == NomadNet::UserActionKind::BACK &&
                        command.result == NomadNet::OwnerResult::BACK_EMPTY) {
                    nomad_back_empty();
                    if (_navigation.current() != Route::NOMADNET) _nomad_actions.clear();
                    break;
                }
                LVGL_LOCK();
                _nomadnet_screen->set_status(NomadNet::OwnerController::status(command.result));
                break;
            }
            NomadNet::clear_encoded_form(_nomad_submission_data);
            _nomad_submission_data.swap(command.request_data);
            _nomad_submission_ready = !_nomad_submission_data.empty();
            _nomad_pending_history = std::move(command.pending_history);
            _nomad_cache_bypass_once = command.cache_bypass;
            nomad_open(command.target, false, command.restore_scroll,
                       !_nomad_submission_data.empty(), true);
            break;
        }
        case NomadNet::UserActionKind::SAVE: {
            const bool save = !_nomad_library.page_saved(target);
            if (!_nomad_library.set_page_saved(target, save)) break;
            _nomad_library_dirty = true;
            const bool current_page = !_nomad_url.destination_hex.empty() && _nomad_url.str() == target;
            LVGL_LOCK();
            _nomadnet_screen->set_library(_nomad_library);
            if (current_page) _nomadnet_screen->set_page_saved(save);
            break;
        }
        case NomadNet::UserActionKind::IDENTIFY: {
            if (_nomad_url.destination_hex.empty() || target != _nomad_url.str()) break;
            const bool identified = action.item_id != 0;
            if (!identified && _nomad_link_identified && !nomad_stop_transport()) {
                LVGL_LOCK();
                _nomadnet_screen->set_status("Could not switch active link to anonymous browsing");
                break;
            }
            if (!_nomad_library.set_node_identified(_nomad_url.destination_hex, identified)) break;
            _nomad_library_dirty = true;
            if (identified && !_nomad_link_identified && _nomad_link &&
                       _nomad_link.status() == Type::Link::ACTIVE) {
                RouterLock router_lock;
                if (router_lock.acquired()) {
                    _nomad_link.identify(_router.identity());
                    _nomad_link_identified = true;
                }
            }
            LVGL_LOCK();
            _nomadnet_screen->set_library(_nomad_library);
            _nomadnet_screen->set_identify_enabled(identified);
            _nomadnet_screen->set_status(identified
                ? "Identity enabled for this node" : "Anonymous browsing enabled for this node");
            break;
        }

        case NomadNet::UserActionKind::HOME:
            _nomad_actions.clear();
            home();
            break;
        }
        if ((action.kind != NomadNet::UserActionKind::SAVE &&
             action.kind != NomadNet::UserActionKind::IDENTIFY) ||
            !_nomad_actions.terminal_pending()) return;
    }
}

void UIManager::service_nomad_user_action() {
    nomad_update_user_actions();
}

bool UIManager::nomad_link_pending() const {
    return _nomad_state == NomadState::LINK && _nomad_link &&
           _nomad_link.status() != RNS::Type::Link::ACTIVE &&
           _nomad_link.status() != RNS::Type::Link::CLOSED;
}

bool UIManager::nomad_operation_active() const {
    return _nomad_state.load(std::memory_order_acquire) != NomadState::IDLE;
}

#if defined(PYXIS_TEST_HOOKS) || defined(PYXIS_NOMAD_LINK_DIAGNOSTIC)
bool UIManager::test_nomad_open(const std::string& address) {
    return _nomad_actions.publish(NomadNet::UserActionKind::OPEN, address);
}

void UIManager::test_nomad_status() const {
    const char* state = "IDLE";
    switch (_nomad_state) {
        case NomadState::CACHE: state = "CACHE"; break;
        case NomadState::PATH: state = "PATH"; break;
        case NomadState::LINK: state = "LINK"; break;
        case NomadState::REQUEST: state = "REQUEST"; break;
        case NomadState::IDLE: break;
    }
    const bool path = _nomad_destination_hash && Transport::has_path(_nomad_destination_hash);
    const bool identity = _nomad_destination_hash && Identity::recall(_nomad_destination_hash);
    const int link = _nomad_link ? static_cast<int>(_nomad_link.status()) : -1;
    const int request = _nomad_request ? static_cast<int>(_nomad_request.get_status()) : -1;
    Serial.printf(
        "T:OK state=%s path=%d identity=%d link=%d request=%d response=%u "
        "free=%u largest=%u minimum=%u\n",
        state, path ? 1 : 0, identity ? 1 : 0, link, request,
        static_cast<unsigned>(_nomad_response.size()),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
}
#endif

void UIManager::nomad_release_request() {
    // request_timed_out() is the pinned dependency's only public pending-set
    // erasure path, and it synchronously invokes the failed callback. Seal the
    // callback mailbox first so successful terminal state cannot be replaced.
    _nomad_mailbox.seal();
    if (!_nomad_request) return;
    RequestReceipt receipt = _nomad_request;
    _nomad_request = RequestReceipt(Type::NONE);
    // Pinned 6ac0d32 does not erase successful packet receipts or pending
    // receipts on Link close. request_timed_out() is the only public erasure
    // path. The sealed mailbox rejects the synthetic failed callback generated
    // by this compatibility cleanup.
    if (receipt.get_status() != Type::RequestReceipt::FAILED) {
        receipt.request_timed_out(PacketReceipt(Type::NONE));
    }
}

void UIManager::nomad_finish_request_keep_link() {
    // A RESPONSE event arrives only after the dependency marks the receipt
    // READY and removes it from Link::pending_requests(). Seal callback delivery
    // before dropping our concluded receipt, but retain the ACTIVE Link so a
    // same-destination page can issue its next anonymous request directly.
    _nomad_mailbox.seal();
    _nomad_request = RequestReceipt(Type::NONE);
    _nomad_state = NomadState::IDLE;
    _nomad_deadline_ms = 0;
    nomad_heap_checkpoint("request-finished");
}

bool UIManager::nomad_stop_transport() {
    NomadNet::clear_encoded_form(_nomad_submission_data);
    _nomad_submission_ready = false;
    _nomad_pending_history.clear();
    // Link teardown can synchronously cancel an in-flight response Resource,
    // update its shared RequestReceipt and invoke callbacks. Keep that mutation
    // in the Reticulum serialization domain, and let teardown mark the receipt
    // failed before the compatibility pending-set cleanup observes it.
    RouterLock router_lock;
    if (!router_lock.acquired()) return false;
    nomad_heap_checkpoint("stop-enter");
    _nomad_state = NomadState::IDLE;
    _nomad_deadline_ms = 0;
    _nomad_pending_scroll = -1;
    _nomad_mailbox.seal();
    if (_nomad_link && _nomad_link.status() != Type::Link::CLOSED) _nomad_link.teardown();
    nomad_release_request();
    nomad_heap_checkpoint("stop-request-released");
    _nomad_link = Link(Type::NONE);
    _nomad_link_identified = false;
    nomad_heap_checkpoint("stop-link-released");
    nomad_heap_checkpoint("stop-done");
    return true;
}

bool UIManager::nomad_refresh_path_after_link_failure() {
    if (_nomad_request_policy.on_link_timeout() !=
        NomadNet::RequestPolicy::LinkTimeoutAction::REFRESH_PATH) return false;
    nomad_heap_checkpoint("link-timeout-enter");
    _nomad_mailbox.seal();
    if (_nomad_link && _nomad_link.status() != Type::Link::CLOSED) _nomad_link.teardown();
    nomad_release_request();
    _nomad_link = Link(Type::NONE);
    _nomad_link_identified = false;
    nomad_heap_checkpoint("link-timeout-released");
    Transport::expire_path(_nomad_destination_hash);
    if (!NomadNet::RequestPolicy::path_invalidation_succeeded(
            Transport::has_path(_nomad_destination_hash))) return false;
    Transport::request_path(_nomad_destination_hash);
    _nomad_state = NomadState::PATH;
    _nomad_deadline_ms = millis() + NomadNet::RequestPolicy::PATH_WAIT_MS;
    LVGL_LOCK();
    if (_nomad_partial_controller.active())
        _nomadnet_screen->set_partial_activity(true);
    else
        _nomadnet_screen->set_status("Refreshing stale path...");
    return true;
}

uint32_t UIManager::nomad_advance_navigation_generation() {
    _nomad_partial_scheduler.cancel(_nomad_navigation_generation);
    _nomad_partial_controller.cancel();
    _nomad_partial_request = NomadNet::PartialRequest{};
    {
        LVGL_LOCK();
        _nomadnet_screen->set_partial_activity(false);
    }
    ++_nomad_navigation_generation;
    if (_nomad_navigation_generation == 0) ++_nomad_navigation_generation;
    return _nomad_navigation_generation;
}

bool UIManager::nomad_supersede_transport(const std::string& destination_hex) {
    // Reconcile callback and Reticulum ownership before cache lookup. This does
    // not touch the rendered model: a cache miss clears it only when live I/O
    // actually begins, while a hit replaces it atomically at publication.
    RouterLock router_lock;
    if (!router_lock.acquired()) return false;
    _nomad_state = NomadState::IDLE;
    _nomad_deadline_ms = 0;
    _nomad_mailbox.seal();

    const bool retain_link = _nomad_link &&
        _nomad_link.status() == Type::Link::ACTIVE &&
        _nomad_destination_hash &&
        bytes_equal_lower_hex(_nomad_destination_hash, destination_hex);
    if (!retain_link && _nomad_link && _nomad_link.status() != Type::Link::CLOSED)
        _nomad_link.teardown();
    nomad_release_request();
    _nomad_request = RequestReceipt(Type::NONE);
    _nomad_response.release();
    if (!retain_link) {
        _nomad_link = Link(Type::NONE);
        _nomad_link_identified = false;
        _nomad_destination_hash = Bytes();
    }
    return true;
}

void UIManager::nomad_open(const std::string& address, bool add_history,
                           int32_t restore_logical_scroll, bool preserve_submission,
                           bool history_prepared) {
    if (address.rfind("p:", 0) == 0) {
        nomad_schedule_partial_ids(address, millis());
        return;
    }
    if (!preserve_submission) {
        NomadNet::clear_encoded_form(_nomad_submission_data);
        _nomad_submission_ready = false;
    }
    nomad_heap_checkpoint("open-enter");
    NomadNet::Url parsed;
    std::string error;
    bool parsed_ok = false;
    try {
        parsed_ok = NomadNet::Url::parse(address, parsed, error,
            _nomad_url.destination_hex, _nomad_url.path, _nomad_url.fields);
    } catch (const std::bad_alloc&) {
        NomadNet::clear_encoded_form(_nomad_submission_data);
        _nomad_submission_ready = false;
        _nomad_pending_history.clear();
        LVGL_LOCK();
        _nomadnet_screen->set_status("Address exceeds available memory");
        return;
    }
    if (!parsed_ok) {
        NomadNet::clear_encoded_form(_nomad_submission_data);
        _nomad_submission_ready = false;
        _nomad_pending_history.clear();
        LVGL_LOCK();
        _nomadnet_screen->set_status(error.c_str());
        return;
    }
    nomad_heap_checkpoint("open-parsed");

    int32_t current_scroll=0;
    bool page_loaded=false;
    {
        LVGL_LOCK();
        current_scroll=_nomadnet_screen->logical_scroll();
        page_loaded=_nomadnet_screen->page_loaded();
    }
    const bool restoring_history=restore_logical_scroll>=0;
    if(!preserve_submission&&NomadNet::should_jump_locally(_nomad_url,parsed,page_loaded,restoring_history)){
        NomadNet::LocalNavigationResult local_result =
            NomadNet::LocalNavigationResult::STAGING_FAILED;
        try {
            // Materialize every fallible canonical owner value before transport,
            // visible scroll/address, or history state is changed.
            const std::string canonical_address = parsed.str();
            NomadNet::Url next_url = parsed;
            local_result = NomadNet::apply_local_navigation_transaction(
                canonical_address, add_history, current_scroll,
                restore_logical_scroll, history_prepared,
                _nomad_history, _nomad_pending_history,
                [&]() {
                    _nomad_cache_flow.cancel();
                    return nomad_supersede_transport(parsed.destination_hex);
                },
                [&](const std::string& published_address, int32_t restore_scroll) {
                    bool resolved = true;
                    LVGL_LOCK();
                    if (!_nomadnet_screen->set_local_address(published_address))
                        return false;
                    if (restore_scroll >= 0)
                        _nomadnet_screen->restore_logical_scroll(restore_scroll);
                    else
                        resolved = _nomadnet_screen->jump_to_anchor(parsed.fragment);
                    if (!resolved) return false;
                    _nomadnet_screen->set_status("Page loaded");
                    return true;
                });
            if (local_result == NomadNet::LocalNavigationResult::APPLIED) {
                _nomad_url = std::move(next_url);
                _nomad_pending_scroll = -1;
                // A fragment jump keeps the same published page and partials.
                return;
            }
        } catch (const std::bad_alloc&) {
            _nomad_pending_history.clear();
            local_result = NomadNet::LocalNavigationResult::STAGING_FAILED;
        }
        LVGL_LOCK();
        if (local_result == NomadNet::LocalNavigationResult::PUBLICATION_FAILED &&
                !parsed.fragment.empty()) {
            const std::string status = "Unknown anchor: #" + parsed.fragment;
            _nomadnet_screen->set_status(status.c_str());
        } else if (local_result == NomadNet::LocalNavigationResult::PREPARATION_FAILED) {
            _nomadnet_screen->set_status("Navigation is busy; try again");
        } else {
            _nomadnet_screen->set_status("Form history exceeds available memory");
        }
        return;
    }

    _nomad_pending_scroll=restore_logical_scroll;
    nomad_heap_checkpoint("open-owner");
    const uint8_t* history_request = _nomad_submission_ready
        ? _nomad_submission_data.data() : nullptr;
    const std::size_t history_request_size = _nomad_submission_ready
        ? _nomad_submission_data.size() : 0;
    if (!history_prepared && !_nomad_history.prepare_open(parsed.str(), add_history,
            current_scroll, history_request, history_request_size,
            _nomad_pending_history)) {
        NomadNet::clear_encoded_form(_nomad_submission_data);
        _nomad_submission_ready = false;
        LVGL_LOCK();
        _nomadnet_screen->set_status("Form history exceeds available memory");
        return;
    }
    nomad_advance_navigation_generation();
    _nomad_cache_flow.cancel();
    if (!nomad_supersede_transport(parsed.destination_hex)) {
        NomadNet::clear_encoded_form(_nomad_submission_data);
        _nomad_submission_ready = false;
        _nomad_pending_history.clear();
        LVGL_LOCK();
        _nomadnet_screen->set_status("Navigation is busy; try again");
        return;
    }
    _nomad_url = parsed;
    _nomad_request_data_class = _nomad_submission_ready
        ? NomadNet::RequestDataClass::FORM
        : parsed.fields.empty() ? NomadNet::RequestDataClass::NIL
                                : NomadNet::RequestDataClass::FIELDS;
    const NomadNet::CacheKey cache_key{
        parsed.destination_hex, parsed.path, _nomad_request_data_class};
    const double wall_time = Utilities::OS::time();
    const uint64_t cache_now = wall_time >= 1609459200.0
        ? static_cast<uint64_t>(wall_time) : 0;
    const bool bypass = _nomad_cache_bypass_once;
    _nomad_cache_bypass_once = false;
    _nomad_cache_generation = _nomad_navigation_generation;
    {
        LVGL_LOCK();
        _nomadnet_screen->show_pending_navigation(address);
    }
    if (_nomad_cache_flow.begin(cache_key, cache_now, bypass) ==
            NomadNet::CacheFlowState::LOOKUP) {
        _nomad_state = NomadState::CACHE;
        LVGL_LOCK();
        _nomadnet_screen->set_status("Checking SD page cache...");
        return;
    }
    nomad_begin_live_transport();
}

const NomadNet::Url& UIManager::nomad_transport_url() const {
    return _nomad_partial_controller.active() ? _nomad_partial_url : _nomad_url;
}

bool UIManager::nomad_schedule_partial_ids(const std::string& address,
                                           uint32_t now_ms) {
    bool matched = false;
    std::size_t start = 2;
    while (start <= address.size()) {
        const std::size_t end = address.find(':', start);
        const std::size_t size = (end == std::string::npos ? address.size() : end) - start;
        if (size != 0 && size <= NomadNet::DocumentParser::MAX_PARTIAL_ID_BYTES) {
            LVGL_LOCK();
            for (std::size_t index = 0;
                    index < _nomad_partial_scheduler.size(); ++index) {
                if (_nomadnet_screen->partial_id_matches(
                        index, address.data() + start, size) &&
                        _nomad_partial_scheduler.request_now(
                            index, _nomad_navigation_generation, now_ms))
                    matched = true;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    LVGL_LOCK();
    if (!matched)
        _nomadnet_screen->set_status("No matching dynamic content");
    return matched;
}

void UIManager::nomad_poll_partials(uint32_t now_ms) {
    if (_nomad_state != NomadState::IDLE ||
            _nomad_partial_controller.active())
        return;
    NomadNet::PartialRequest request;
    if (!_nomad_partial_scheduler.poll(
            now_ms, _navigation.current() == Route::NOMADNET,
            true, request))
        return;

    NomadNet::FormEncodeResult encode_result;
    bool prepared = false;
    {
        LVGL_LOCK();
        prepared = _nomadnet_screen->prepare_partial_request(
            request, _nomad_partial_controller, encode_result);
    }
    if (!prepared) {
        _nomad_partial_scheduler.defer(request);
        _nomad_partial_controller.abandon_request();
        LVGL_LOCK();
        _nomadnet_screen->set_status(
            encode_result == NomadNet::FormEncodeResult::ALLOCATION_FAILED
                ? "Dynamic request exceeds available memory"
                : "Dynamic request data exceeds device limit");
        return;
    }

    NomadNet::Url target;
    std::string error;
    bool parsed = false;
    bool allocation_failed = false;
    try {
        parsed = NomadNet::Url::parse(
            std::string(_nomad_partial_controller.url_data(),
                        _nomad_partial_controller.url_size()),
            target, error, _nomad_url.destination_hex, _nomad_url.path, {});
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    if (!parsed || target.has_fragment) {
        if (allocation_failed) _nomad_partial_scheduler.defer(request);
        else _nomad_partial_scheduler.complete(request, false, now_ms);
        _nomad_partial_controller.abandon_request();
        LVGL_LOCK();
        _nomadnet_screen->set_status(
            allocation_failed ? "Dynamic address exceeds available memory" :
            parsed ? "Dynamic address cannot target a fragment" : error.c_str());
        return;
    }
    if (!nomad_supersede_transport(target.destination_hex)) {
        _nomad_partial_scheduler.defer(request);
        _nomad_partial_controller.abandon_request();
        LVGL_LOCK();
        _nomadnet_screen->set_status("Dynamic request owner is busy");
        return;
    }
    _nomad_partial_request = request;
    _nomad_partial_url = std::move(target);
    _nomad_state = NomadState::PARTIAL_PENDING;
    {
        LVGL_LOCK();
        _nomadnet_screen->set_partial_activity(true);
    }
    nomad_begin_partial_transport();
}

void UIManager::nomad_begin_partial_transport() {
    try {
    if (!_nomad_partial_controller.active()) {
        _nomad_state = NomadState::IDLE;
        return;
    }
    RouterLock router_lock;
    if (!router_lock.acquired()) {
        _nomad_state = NomadState::PARTIAL_PENDING;
        return;
    }
    const bool same_destination = _nomad_link &&
        _nomad_link.status() == Type::Link::ACTIVE &&
        _nomad_destination_hash && bytes_equal_lower_hex(
            _nomad_destination_hash, _nomad_partial_url.destination_hex);
    _nomad_mailbox.prepare(
        _nomad_navigation_generation,
        NomadNet::PartialController::MAX_RESPONSE_WIRE_BYTES);
    if (same_destination) {
        nomad_send_request();
        return;
    }
    if (_nomad_link && _nomad_link.status() != Type::Link::CLOSED)
        _nomad_link.teardown();
    nomad_release_request();
    _nomad_link = Link(Type::NONE);
    _nomad_link_identified = false;
    _nomad_request = RequestReceipt(Type::NONE);
    _nomad_response.clear();
    _nomad_request_policy.reset();
    _nomad_destination_hash = Bytes();
    _nomad_destination_hash.assignHex(_nomad_partial_url.destination_hex.c_str());
    if (Transport::has_path(_nomad_destination_hash)) nomad_start_link();
    else {
        Transport::request_path(_nomad_destination_hash);
        _nomad_state = NomadState::PATH;
        _nomad_deadline_ms = millis() + NomadNet::RequestPolicy::PATH_WAIT_MS;
    }
    } catch (const std::bad_alloc&) {
        nomad_defer_partial(
            "Dynamic transport exceeds available internal memory", false);
    }
}

void UIManager::nomad_finish_partial(bool success, const char* status) {
    nomad_release_partial(success, false, status, true);
}

void UIManager::nomad_defer_partial(const char* status, bool retain_link) {
    nomad_release_partial(false, true, status, retain_link);
}

void UIManager::nomad_release_partial(bool success, bool deferred, const char* status,
                                      bool allow_retain_link) {
    if (!_nomad_partial_controller.active()) return;
    const std::string& destination = _nomad_partial_url.destination_hex;
    if (deferred) _nomad_partial_scheduler.defer(_nomad_partial_request);
    else _nomad_partial_scheduler.complete(
            _nomad_partial_request, success, millis());
    _nomad_response.release();
    const bool retain_link = allow_retain_link && _nomad_link &&
        _nomad_link.status() == Type::Link::ACTIVE &&
        _nomad_destination_hash &&
        bytes_equal_lower_hex(_nomad_destination_hash, destination);
    if (retain_link) {
        if (success) nomad_finish_request_keep_link();
        else {
            nomad_release_request();
            _nomad_state = NomadState::IDLE;
            _nomad_deadline_ms = 0;
        }
    } else nomad_stop_transport();
    _nomad_partial_controller.abandon_request();
    _nomad_partial_request = NomadNet::PartialRequest{};
    _nomad_partial_url = NomadNet::Url{};
    LVGL_LOCK();
    _nomadnet_screen->set_partial_activity(false);
    if (success) _nomadnet_screen->clear_status();
    else if (status) _nomadnet_screen->set_status(status);
}

void UIManager::nomad_begin_live_transport() {
    RouterLock router_lock;
    if (!router_lock.acquired()) {
        // Preserve the currently published page and retry from the deterministic
        // owner loop. No mailbox or transport callback is armed in this state.
        _nomad_state = NomadState::LIVE_PENDING;
        return;
    }
    {
        // Transport admission is non-destructive. The old model remains visible
        // until the complete replacement has been published successfully.
        LVGL_LOCK();
        nomad_heap_checkpoint("action-before-navigation");
        _nomadnet_screen->set_address(_nomad_url.str());
        nomad_heap_checkpoint("action-after-navigation");
    }
    const bool same_destination = _nomad_link &&
        _nomad_link.status() == Type::Link::ACTIVE &&
        _nomad_destination_hash && bytes_equal_lower_hex(
            _nomad_destination_hash, _nomad_url.destination_hex);
    if (same_destination) {
        _nomad_response.clear();
        _nomad_request_policy.reset();
        {
            LVGL_LOCK();
            _nomadnet_screen->set_address(_nomad_url.str());
            _nomadnet_screen->set_status("Requesting page...");
        }
        _nomad_mailbox.prepare(
            _nomad_navigation_generation,
            _nomad_partial_controller.active()
                ? NomadNet::PartialController::MAX_RESPONSE_WIRE_BYTES
                : NomadNet::AsyncMailbox::MAX_WIRE_BYTES);
        nomad_send_request();
        return;
    }
    _nomad_mailbox.seal();
    if (_nomad_link && _nomad_link.status() != Type::Link::CLOSED) _nomad_link.teardown();
    nomad_release_request();
    _nomad_link = Link(Type::NONE);
    _nomad_link_identified = false;
    _nomad_request = RequestReceipt(Type::NONE);
    _nomad_response.clear();
    _nomad_request_policy.reset();
    _nomad_destination_hash = Bytes();
    _nomad_destination_hash.assignHex(_nomad_url.destination_hex.c_str());
    {
        LVGL_LOCK();
        _nomadnet_screen->set_address(_nomad_url.str());
        _nomadnet_screen->set_status("Discovering path...");
    }
    if (Transport::has_path(_nomad_destination_hash)) nomad_start_link();
    else {
        Transport::request_path(_nomad_destination_hash);
        _nomad_state = NomadState::PATH;
        _nomad_deadline_ms = millis() + NomadNet::RequestPolicy::PATH_WAIT_MS;
    }
}

void UIManager::nomad_start_link() {
    nomad_heap_checkpoint("link-enter");
    Identity identity = Identity::recall(_nomad_destination_hash);
    if (!identity) {
        if (_nomad_partial_controller.active()) {
            nomad_finish_partial(false, "Dynamic-content identity is not known");
            return;
        }
        _nomad_state = NomadState::IDLE;
        NomadNet::clear_encoded_form(_nomad_submission_data);
        _nomad_submission_ready = false;
        LVGL_LOCK();
        _nomadnet_screen->set_status("Node identity is not known");
        return;
    }
    Destination destination(identity, Type::Destination::OUT,
                            Type::Destination::SINGLE, "nomadnetwork", "node");
    if (destination.hash() != _nomad_destination_hash) {
        if (_nomad_partial_controller.active()) {
            nomad_finish_partial(false, "Dynamic-content identity does not match address");
            return;
        }
        _nomad_state = NomadState::IDLE;
        NomadNet::clear_encoded_form(_nomad_submission_data);
        _nomad_submission_ready = false;
        LVGL_LOCK();
        _nomadnet_screen->set_status("Identity does not match node address");
        return;
    }
    _nomad_mailbox.prepare(
        _nomad_navigation_generation,
        _nomad_partial_controller.active()
            ? NomadNet::PartialController::MAX_RESPONSE_WIRE_BYTES
            : NomadNet::AsyncMailbox::MAX_WIRE_BYTES);
    nomad_heap_checkpoint("link-before-construct");
    _nomad_link = Link(destination, on_nomad_link_established, on_nomad_link_closed);
    nomad_heap_checkpoint("link-after-construct");
    _nomad_mailbox.begin(token(_nomad_link.link_id()), _nomad_navigation_generation);
    _nomad_state = NomadState::LINK;
    _nomad_deadline_ms = millis() + NomadNet::RequestPolicy::LINK_WAIT_MS;
    LVGL_LOCK();
    if (_nomad_partial_controller.active())
        _nomadnet_screen->set_partial_activity(true);
    else
        _nomadnet_screen->set_status("Establishing encrypted link...");
}

void UIManager::nomad_identify_link_if_configured() {
    if (_nomad_link_identified) return;
    if (_nomad_library.node_identified(nomad_transport_url().destination_hex)) {
        _nomad_link.identify(_router.identity());
        _nomad_link_identified = true;
    }
}

void UIManager::nomad_send_request() {
    if (!_nomad_link || _nomad_link.status() != Type::Link::ACTIVE) return;
    nomad_identify_link_if_configured();
    nomad_heap_checkpoint("request-enter");
    _nomad_link.set_resource_started_callback(on_nomad_resource_started);
    RNS::Bytes packed_request_data;
    try {
        if (_nomad_partial_controller.active()) {
            packed_request_data = Bytes(_nomad_partial_controller.request_data(),
                                        _nomad_partial_controller.request_size());
        } else if (_nomad_submission_ready) {
            packed_request_data = Bytes(_nomad_submission_data.data(), _nomad_submission_data.size());
        } else {
            const auto request_data = NomadNet::request_data(_nomad_url.fields);
            if (request_data.size() > NomadNet::FormState::MAX_ENCODED_BYTES) {
                nomad_stop_transport();
                LVGL_LOCK();
                _nomadnet_screen->set_status("Request data exceeds device limit");
                return;
            }
            packed_request_data = Bytes(request_data.data(), request_data.size());
        }
        if (_nomad_partial_controller.active()) {
            _nomad_request = _nomad_link.request(
                Bytes(reinterpret_cast<const uint8_t*>(_nomad_partial_url.path.data()),
                      _nomad_partial_url.path.size()),
                packed_request_data, on_nomad_response, on_nomad_failed,
                on_nomad_progress, 30.0,
                NomadNet::PartialController::MAX_RESPONSE_WIRE_BYTES, true);
        } else {
            _nomad_request = _nomad_link.request(
                Bytes(reinterpret_cast<const uint8_t*>(_nomad_url.path.data()),
                      _nomad_url.path.size()),
                packed_request_data, on_nomad_response, on_nomad_failed,
                on_nomad_progress, 30.0, NomadNet::AsyncMailbox::MAX_WIRE_BYTES, true);
        }
    } catch (const std::bad_alloc&) {
        if (packed_request_data) {
            volatile uint8_t* bytes = packed_request_data.writable(0);
            for (std::size_t i = 0; i < packed_request_data.size(); ++i) bytes[i] = 0;
        }
        if (_nomad_partial_controller.active()) {
            nomad_defer_partial(
                "Dynamic request exceeds available internal memory",
                false);
        } else {
            NomadNet::clear_encoded_form(_nomad_submission_data);
            _nomad_submission_ready = false;
            nomad_stop_transport();
            LVGL_LOCK();
            _nomadnet_screen->set_status("Request exceeds available internal memory");
        }
        return;
    }
    if (packed_request_data) {
        volatile uint8_t* bytes = packed_request_data.writable(0);
        for (std::size_t i = 0; i < packed_request_data.size(); ++i) bytes[i] = 0;
    }
    if (!_nomad_partial_controller.active()) {
        NomadNet::clear_encoded_form(_nomad_submission_data);
        _nomad_submission_ready = false;
    }
    nomad_heap_checkpoint("request-created");
    if (!_nomad_request) {
        if (_nomad_partial_controller.active())
            nomad_finish_partial(false, "Dynamic request could not be sent");
        else {
            nomad_stop_transport();
            LVGL_LOCK();
            _nomadnet_screen->set_status("Request could not be sent");
        }
        return;
    }
    _nomad_mailbox.expect_request(token(_nomad_request.request_id()));
    _nomad_state = NomadState::REQUEST;
    _nomad_deadline_ms = millis() + 30000;
    LVGL_LOCK();
    if (_nomad_partial_controller.active())
        _nomadnet_screen->set_partial_activity(true);
    else _nomadnet_screen->set_status("Requesting page...");
}

bool UIManager::nomad_apply_page_bytes(const uint8_t* data, std::size_t size, bool cached) {
    NomadNet::Document document;
    const auto parse_status = _nomad_parser.parse_into(
        reinterpret_cast<const char*>(data), size, document);
    if (parse_status == NomadNet::ParseStatus::ALLOCATION_FAILED) {
        LVGL_LOCK();
        _nomadnet_screen->set_status("Page is too large for available memory");
        return false;
    }
    if (parse_status == NomadNet::ParseStatus::INVALID_INPUT) {
        LVGL_LOCK();
        _nomadnet_screen->set_status("Page input is invalid");
        return false;
    }
    if (document.malformed && document.blocks.empty()) {
        LVGL_LOCK();
        _nomadnet_screen->set_status("Page is not valid UTF-8/Micron");
        return false;
    }
    return nomad_apply_page_document(document, cached) ==
        NomadNet::PageApplyResult::APPLIED;
}

NomadNet::PageApplyResult UIManager::nomad_apply_page_document(
        const NomadNet::Document& document, bool cached) {
    bool library_changed = false;
    const auto result = NomadNet::apply_page_transaction_for_url(
        document, _nomad_url, static_cast<uint64_t>(Utilities::OS::time()),
        _nomad_library,
        [&](const NomadNet::PagePublication& publication) {
            bool anchor_resolved = true;
            LVGL_LOCK();
            if (!_nomadnet_screen->set_page(document)) return false;
            if (_nomad_pending_scroll >= 0)
                _nomadnet_screen->restore_logical_scroll(_nomad_pending_scroll);
            else if (_nomad_url.has_fragment)
                anchor_resolved = _nomadnet_screen->jump_to_anchor(_nomad_url.fragment);
            _nomadnet_screen->set_page_saved(publication.page_saved);
            _nomadnet_screen->set_identify_enabled(publication.identify_enabled);
            if (!anchor_resolved && !publication.unknown_anchor_status.empty())
                _nomadnet_screen->set_status(publication.unknown_anchor_status.c_str());
            else
                _nomadnet_screen->set_status(cached
                    ? "Cached page; current reachability not checked"
                    : "Page loaded (live)");
            library_changed = publication.library_changed;
            return true;
        },
        [&]() noexcept {
            if (_nomad_pending_history.ready())
                _nomad_history.commit(std::move(_nomad_pending_history));
        });
    if (result == NomadNet::PageApplyResult::ALLOCATION_FAILED) {
        LVGL_LOCK();
        _nomadnet_screen->set_status("Page is too large for available memory");
    }
    if (result != NomadNet::PageApplyResult::APPLIED) {
        _nomad_pending_history.clear();
        return result;
    }
    _nomad_partial_scheduler.configure(
        document, _nomad_navigation_generation, millis());
    _nomad_partial_controller.reset_page(document.source_bytes);
    if (library_changed) _nomad_library_dirty = true;
    _nomad_pending_scroll = -1;
    return result;
}
void UIManager::nomad_update() {
    // Filesystem chunks are owner-loop work and are always serviced before the
    // Router serialization domain. LVGL is taken only later to apply a complete,
    // validated immutable page.
    // Start deferred mutations before RouterLock. The response callback only
    // transfers immutable PSRAM ownership while the router is serialized.
    if (_nomad_cache_pending_invalidate &&
            _nomad_cache_pending_generation != _nomad_navigation_generation) {
        _nomad_cache_pending_invalidate = false;
    }
    if (!_nomad_cache_pending_body.empty() &&
            _nomad_cache_pending_generation != _nomad_navigation_generation) {
        NomadNet::ExternalVector<uint8_t>().swap(_nomad_cache_pending_body);
        _nomad_cache_pending_now = 0;
        _nomad_cache_pending_ttl = 0;
    }
    if (!_nomad_cache.busy() && _nomad_cache_pending_invalidate) {
        _nomad_cache.invalidate(_nomad_cache_pending_key);
        _nomad_cache_pending_invalidate = false;
    }
    if (!_nomad_cache.busy() && !_nomad_cache_pending_body.empty()) {
        _nomad_cache.beginCommit(_nomad_cache_pending_key,
            std::move(_nomad_cache_pending_body), _nomad_cache_pending_now,
            _nomad_cache_pending_ttl);
        _nomad_cache_pending_now = 0;
        _nomad_cache_pending_ttl = 0;
    }
    _nomad_cache_flow.service();
    if (_nomad_state == NomadState::CACHE) {
        if (_nomad_cache_generation != _nomad_navigation_generation) {
            _nomad_cache_flow.cancel();
            _nomad_state = NomadState::IDLE;
            return;
        }
        if (_nomad_cache_flow.state() == NomadNet::CacheFlowState::LOOKUP) return;
        if (_nomad_cache_flow.state() == NomadNet::CacheFlowState::READY) {
            NomadNet::ExternalVector<uint8_t> cached;
            if (_nomad_cache_flow.takePage(cached) &&
                    nomad_apply_page_bytes(cached.data(), cached.size(), true)) {
                _nomad_state = NomadState::IDLE;
                return;
            }
        }
        _nomad_state = NomadState::IDLE;
        nomad_begin_live_transport();
        return;
    }
    if (_nomad_state == NomadState::LIVE_PENDING) {
        nomad_begin_live_transport();
        return;
    }
    if (_nomad_state == NomadState::PARTIAL_PENDING) {
        nomad_begin_partial_transport();
        return;
    }
    if (_nomad_state == NomadState::IDLE) {
        nomad_poll_partials(millis());
        if (_nomad_state != NomadState::IDLE) return;
    }
    RouterLock router_lock;
    if (!router_lock.acquired()) return;
    const uint32_t now = millis();
    if (_nomad_state == NomadState::PATH && Transport::has_path(_nomad_destination_hash)) {
        nomad_start_link();
    } else if (_nomad_state != NomadState::IDLE && _nomad_deadline_ms != 0 &&
               static_cast<int32_t>(now - _nomad_deadline_ms) >= 0) {
        if (_nomad_state == NomadState::LINK && nomad_refresh_path_after_link_failure()) return;
        if (_nomad_partial_controller.active())
            nomad_finish_partial(false, "Dynamic-content request timed out");
        else {
            nomad_stop_transport();
            LVGL_LOCK();
            _nomadnet_screen->set_status("NomadNet operation timed out");
        }
        return;
    }

    NomadNet::AsyncMailbox::Event event;
    if (!_nomad_mailbox.take(event)) return;
    if (event.generation != _nomad_navigation_generation) return;
    if (event.kind == NomadNet::AsyncMailbox::Kind::RESPONSE)
        nomad_heap_checkpoint("response-taken");
    switch (event.kind) {
        case NomadNet::AsyncMailbox::Kind::LINK_ESTABLISHED:
            if (_nomad_state == NomadState::LINK) {
                nomad_send_request();
            }
            break;
        case NomadNet::AsyncMailbox::Kind::LINK_CLOSED:
            if (_nomad_state == NomadState::LINK && nomad_refresh_path_after_link_failure()) break;
            if (_nomad_partial_controller.active())
                nomad_finish_partial(false, "Dynamic-content link closed");
            else {
                nomad_stop_transport();
                { LVGL_LOCK(); _nomadnet_screen->set_status("NomadNet link closed"); }
            }
            break;
        case NomadNet::AsyncMailbox::Kind::FAILED:
            if (_nomad_partial_controller.active())
                nomad_finish_partial(false, "Dynamic-content request failed");
            else {
                nomad_stop_transport();
                { LVGL_LOCK(); _nomadnet_screen->set_status("Page request failed"); }
            }
            break;
        case NomadNet::AsyncMailbox::Kind::PROGRESS:
            if (_nomad_state == NomadState::REQUEST &&
                    !_nomad_partial_controller.active()) {
                LVGL_LOCK();
                _nomadnet_screen->set_status("Receiving page...");
            }
            break;
        case NomadNet::AsyncMailbox::Kind::OVERSIZED:
            _nomad_response.clear();
            if (_nomad_partial_controller.active())
                nomad_finish_partial(false, "Dynamic content exceeds 16 KiB limit");
            else {
                nomad_stop_transport();
                { LVGL_LOCK(); _nomadnet_screen->set_status("Page exceeds 64 KiB limit"); }
            }
            break;
        case NomadNet::AsyncMailbox::Kind::RESPONSE: {
            // The dependency has already removed this successful request from
            // Link::pending_requests(). Validate it before deciding whether the
            // Link is safe to retain for same-destination navigation.
            if (_nomad_partial_controller.active()) {
                bool lease_matches = false;
                {
                    LVGL_LOCK();
                    lease_matches = _nomadnet_screen->partial_request_matches(
                        _nomad_partial_request, _nomad_partial_controller);
                }
                if (!lease_matches) {
                    _nomad_partial_scheduler.cancel(_nomad_navigation_generation);
                    _nomad_partial_controller.cancel();
                    nomad_stop_transport();
                    LVGL_LOCK();
                    _nomadnet_screen->set_partial_activity(false);
                    _nomadnet_screen->set_status("Discarded stale dynamic content");
                    break;
                }
            }
            if (!NomadNet::normalize_response(event.data.data(), event.data.size(), _nomad_response)) {
                if (_nomad_partial_controller.active())
                    nomad_finish_partial(false, "Malformed dynamic-content response");
                else {
                    nomad_stop_transport();
                    LVGL_LOCK();
                    _nomadnet_screen->set_status("Malformed NomadNet response");
                }
                break;
            }
            nomad_heap_checkpoint("response-normalized");
            const auto& bytes = _nomad_response.bytes();
            if (_nomad_partial_controller.active()) {
                if (bytes.size() > NomadNet::PartialController::MAX_RESPONSE_BYTES ||
                        !_nomad_partial_controller.can_accept_fragment(
                            _nomad_partial_request.partial_index, bytes.size())) {
                    nomad_finish_partial(false,
                        "Dynamic content exceeds expanded-page limit");
                    break;
                }
                NomadNet::Document fragment;
                const auto parse_status = _nomad_parser.parse_into(
                    reinterpret_cast<const char*>(bytes.data()), bytes.size(), fragment);
                if (parse_status != NomadNet::ParseStatus::OK ||
                        fragment.malformed || fragment.truncated) {
                    nomad_finish_partial(false,
                        parse_status == NomadNet::ParseStatus::ALLOCATION_FAILED
                            ? "Dynamic content exceeds available memory"
                            : "Dynamic content is not valid UTF-8/Micron");
                    break;
                }
                NomadNet::PartialReplaceResult applied;
                {
                    LVGL_LOCK();
                    applied = _nomadnet_screen->apply_partial_fragment(
                        _nomad_partial_request, fragment,
                        _nomad_partial_controller);
                }
                if (applied != NomadNet::PartialReplaceResult::APPLIED ||
                        !_nomad_partial_controller.commit_fragment(
                            _nomad_partial_request.partial_index, bytes.size())) {
                    nomad_finish_partial(false,
                        applied == NomadNet::PartialReplaceResult::ALLOCATION_FAILED
                            ? "Dynamic content exceeds available memory"
                            : "Dynamic content exceeds page limits");
                    break;
                }
                nomad_finish_partial(true, nullptr);
                break;
            }
            NomadNet::Document document;
            try {
                document = _nomad_parser.parse(
                    reinterpret_cast<const char*>(bytes.data()), bytes.size());
            } catch (const std::bad_alloc&) {
                _nomad_response.release();
                nomad_stop_transport();
                LVGL_LOCK();
                _nomadnet_screen->set_status("Page is too large for available memory");
                break;
            }
            nomad_heap_checkpoint("response-parsed");
            const bool valid_document = !document.malformed && !document.truncated &&
                document.cache_directive_valid && !document.blocks.empty();
            if (document.malformed && document.blocks.empty()) {
                _nomad_response.release();
                nomad_stop_transport();
                LVGL_LOCK();
                _nomadnet_screen->set_status("Page is not valid UTF-8/Micron");
                break;
            }
            const bool page_applied = nomad_apply_page_document(document, false) ==
                NomadNet::PageApplyResult::APPLIED;
            nomad_heap_checkpoint("response-page-applied");
            if (!page_applied) {
                _nomad_response.release();
                nomad_stop_transport();
                break;
            }
            const bool ordinary_nil =
                _nomad_request_data_class == NomadNet::RequestDataClass::NIL;
            const auto directive = NomadNet::parse_cache_directive(bytes.data(), bytes.size());
            const double wall_time = Utilities::OS::time();
            const uint64_t cache_now = wall_time >= 1609459200.0
                ? static_cast<uint64_t>(wall_time) : 0;
            const NomadNet::CacheKey cache_key{
                _nomad_url.destination_hex, _nomad_url.path,
                _nomad_request_data_class};
            if (ordinary_nil && valid_document && cache_now &&
                    directive.valid && directive.ttl) {
                _nomad_cache_pending_key = cache_key;
                _nomad_cache_pending_body = _nomad_response.take();
                _nomad_cache_pending_now = cache_now;
                _nomad_cache_pending_ttl = directive.ttl;
                _nomad_cache_pending_generation = event.generation;
            } else {
                _nomad_response.release();
                if (ordinary_nil && (!directive.valid || directive.ttl == 0))
                    { _nomad_cache_pending_key = cache_key;
                      _nomad_cache_pending_generation = event.generation;
                      _nomad_cache_pending_invalidate = true; }
            }
            // The response wins a racing close callback. Retain only a currently
            // active Link whose independent owner hash still matches this URL.
            if (_nomad_link && _nomad_link.status() == Type::Link::ACTIVE &&
                    _nomad_destination_hash &&
                    bytes_equal_lower_hex(
                        _nomad_destination_hash, _nomad_url.destination_hex)) {
                nomad_finish_request_keep_link();
            } else {
                nomad_stop_transport();
            }
            break;
        }
        case NomadNet::AsyncMailbox::Kind::NONE:
            break;
    }
}

void UIManager::on_nomad_link_established(Link& link) {
    if (s_nomad_instance) s_nomad_instance->_nomad_mailbox.publish_link(token(link.link_id()), true);
}

void UIManager::on_nomad_link_closed(Link& link) {
    if (s_nomad_instance) s_nomad_instance->_nomad_mailbox.publish_link(token(link.link_id()), false);
}

void UIManager::on_nomad_response(const RequestReceipt& receipt) {
    if (!s_nomad_instance) return;
    const std::size_t transfer = receipt.response_transfer_size();
    const Bytes response = receipt.get_response();
    s_nomad_instance->_nomad_mailbox.publish_response(
        token(receipt.request_id()), response ? response.data() : nullptr,
        response ? response.size() : 0, transfer);
}

void UIManager::on_nomad_failed(const RequestReceipt& receipt) {
    if (!s_nomad_instance) return;
    s_nomad_instance->_nomad_mailbox.publish_failed(
        token(receipt.request_id()), receipt.response_size());
}

void UIManager::on_nomad_progress(const RequestReceipt& receipt) {
    if (!s_nomad_instance) return;
    const std::size_t transfer = receipt.response_transfer_size();
    s_nomad_instance->_nomad_mailbox.publish_progress(token(receipt.request_id()), transfer);
}

void UIManager::on_nomad_resource_started(const Resource& resource) {
    if (!s_nomad_instance || !resource.is_response() || !s_nomad_instance->_nomad_request) return;
    const Bytes request_id = resource.request_id();
    if (!request_id || token(request_id) != token(s_nomad_instance->_nomad_request.request_id())) return;
    const_cast<Resource&>(resource).set_progress_callback(on_nomad_resource_progress);
}

void UIManager::on_nomad_resource_progress(const Resource& resource) {
    if (!s_nomad_instance || !resource.is_response()) return;
    const std::size_t transferred = static_cast<std::size_t>(
        resource.get_progress() * static_cast<float>(resource.get_transfer_size()));
    s_nomad_instance->_nomad_mailbox.publish_progress(
        token(resource.request_id()), transferred);
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
#ifdef PYXIS_TEST_HOOKS
    _test_call_initiate_result = TestCallInitiateResult::FAILED;
#endif
    {
        std::string h = peer_hash.toHex().substr(0, 16);
        INFO(("LXST: Initiating call to " + h + "...").c_str());
    }
    lxst_breadcrumb(1, ESP.getFreeHeap());

    // Link establishment needs roughly 10 KiB for crypto. The former 40 KiB
    // threshold rejected valid calls on the complete UI build, whose steady
    // state is about 30 KiB internal free. Audio allocations are now directed
    // to PSRAM and its task stacks are bounded, so retain a 24 KiB floor.
    size_t free_heap = ESP.getFreeHeap();
    if (free_heap < 24000) {
        char buf[64];
        snprintf(buf, sizeof(buf), "LXST: Insufficient heap (%u bytes), aborting call", (unsigned)free_heap);
        WARNING(buf);
        return;
    }

    lxst_breadcrumb(2, ESP.getFreeHeap());

    // Look up peer identity
    Identity peer_identity = Identity::recall(peer_hash);
    if (!peer_identity) {
        WARNING("LXST: Peer identity not known, cannot establish link");
        return;
    }

    lxst_breadcrumb(3, ESP.getFreeHeap());

    // Create LXST destination for the peer (aspect: lxst.telephony)
    Destination peer_dest(peer_identity, Type::Destination::OUT,
                          Type::Destination::SINGLE, "lxst", "telephony");

    lxst_breadcrumb(4, ESP.getFreeHeap());

    // Reserve a generation only after the outgoing call has passed its
    // acceptance checks. update() holds the recursive LVGL mutex across this
    // entire initiation, and call-lifecycle callbacks take the same mutex even
    // when an interface task dispatches them. The atomic reservation remains
    // the definitive ownership check.
    const uint32_t generation = call_begin_generation();
    if (generation == 0) {
#ifdef PYXIS_TEST_HOOKS
        _test_call_initiate_result = TestCallInitiateResult::BUSY;
#endif
        WARNING("LXST: Another call was accepted concurrently");
        return;
    }
    _call_peer_hash = peer_hash;
    _call_muted = false;

    // Show call screen
    _call_screen->set_peer(peer_dest.hash());
    _call_screen->set_state(CallScreen::CallState::CONNECTING);
    _call_screen->set_muted(false);
    navigate(Route::CALL);

    lxst_breadcrumb(5, ESP.getFreeHeap());

    _call_dest_hash = peer_dest.hash();
    _call_answer_pending.store(false, std::memory_order_release);
    _call_audio_rx_count = 0;
    _call_audio_tx_count = 0;
    _call_signal_write = 0;
    _call_signal_read = 0;
    _call_commands.take();

    {
        std::string dh = peer_dest.hash().toHex().substr(0, 16);
        bool has_path = Transport::has_path(peer_dest.hash());
        char buf[80];
        snprintf(buf, sizeof(buf), "LXST: Dest hash=%s path=%s", dh.c_str(), has_path ? "yes" : "no");
        INFO(buf);
    }

    if (Transport::has_path(peer_dest.hash())) {
        // Register lifecycle callbacks in the constructor before the link
        // request is sent. Publish the exact ID as soon as it returns.
        INFO("LXST: Creating link...");
        _call_link = Link(peer_dest, on_call_link_established, on_call_link_closed);
        if (!call_publish_link(_call_link, generation)) {
            WARNING("LXST: Link has invalid ID, aborting call");
            call_ended();
            return;
        }
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

#ifdef PYXIS_TEST_HOOKS
    _test_call_initiate_result = TestCallInitiateResult::STARTED;
#endif

    lxst_breadcrumb(7, ESP.getFreeHeap());
}

#ifdef PYXIS_TEST_HOOKS
uint32_t UIManager::test_call_decode_ok() const {
    return _lxst_audio ? _lxst_audio->playbackDecodeOk() : 0;
}

uint32_t UIManager::test_call_decode_fail() const {
    return _lxst_audio ? _lxst_audio->playbackDecodeFail() : 0;
}

uint32_t UIManager::test_call_pcm_sample_count() const {
    return _lxst_audio ? _lxst_audio->playbackPcmSampleCount() : 0;
}

uint64_t UIManager::test_call_pcm_sum_squares() const {
    return _lxst_audio ? _lxst_audio->playbackPcmSumSquares() : 0;
}

void UIManager::test_call_set_inject_sine(bool enabled, int freq, float amp) {
    if (_lxst_audio) _lxst_audio->captureSetInjectSine(enabled, freq, amp);
}

bool UIManager::test_call_set_profile(int profile) {
    if (profile_to_codec2_mode(profile) < 0) return false;
    _preferred_profile = profile;
    return true;
}

bool UIManager::test_call_answer() {
    if (_call_state != CallState::INCOMING_RINGING) return false;
    _call_answer_pending.store(true, std::memory_order_release);
    return true;
}

std::string UIManager::test_lxst_dest_hex() const {
    if (!_lxst_destination) return std::string();
    return _lxst_destination.hash().toHex();
}

const char* UIManager::test_call_state_name() const {
    switch (_call_state) {
        case CallState::IDLE:               return "IDLE";
        case CallState::PATH_REQUESTING:    return "PATH_REQUESTING";
        case CallState::LINK_ESTABLISHING:  return "LINK_ESTABLISHING";
        case CallState::WAIT_AVAILABLE:     return "WAIT_AVAILABLE";
        case CallState::WAIT_RINGING:       return "WAIT_RINGING";
        case CallState::RINGING:            return "RINGING";
        case CallState::INCOMING_IDENTIFYING: return "INCOMING_IDENTIFYING";
        case CallState::INCOMING_RINGING:   return "INCOMING_RINGING";
        case CallState::CONNECTING:         return "CONNECTING";
        case CallState::ACTIVE:             return "ACTIVE";
    }
    return "UNKNOWN";
}
#endif

void UIManager::call_hangup() {
    INFO("LXST: Hanging up");

    const uint32_t generation =
        call_current_generation();

    // Normal call termination gets an application-level terminal signal before
    // the one-shot, unacknowledged Reticulum LINKCLOSE. Repetition and the
    // bounded drain interval reduce packet-loss races without blocking the
    // owner task indefinitely. Older peers ignore unknown status 0x07.
    call_send_terminal_burst();

    // Detach callback ownership before touching the shared Link. Keep call
    // admission reserved until all owner-side teardown completes.
    _call_link_ownership.clear(generation);

    // call_hangup() is an owner operation: production UI paths reach it only
    // through update() on loopTask, which also owns pump_call_tx(). Keep the
    // generation reserved until teardown is complete so a concurrent incoming
    // callback cannot install a newer call over the old audio pointer.
    call_teardown_audio();

    // Teardown link
    if (_call_link) {
        _call_link.teardown();
        _call_link = Link(Type::NONE);
    }

    _call_peer_hash = Bytes();
    _call_dest_hash = Bytes();
    _call_start_ms = 0;
    _call_timeout_ms = 0;
    _call_muted = false;
    _call_answer_pending.store(false, std::memory_order_release);
    _call_signal_write = 0;
    _call_signal_read = 0;
    _call_audio_rx_count = 0;
    _call_audio_tx_count = 0;
    _call_commands.take();
    _call_liveness.disarm();
    _call_state = CallState::IDLE;
    call_clear_generation(generation);

    // Return to chat screen
    if (_call_screen) {
        _call_screen->set_state(CallScreen::CallState::ENDED);
    }

    // Restore the screen that owned the call rather than pushing CALL behind a
    // new Messages route. If teardown happened before the call UI appeared,
    // leave the current route untouched.
    if (_navigation.current() == Route::CALL) back();
}

void UIManager::call_set_mute(bool muted) {
    _call_muted = muted;
    if (_lxst_audio) {
        _lxst_audio->setCaptureMute(muted);
    }
    if (_call_screen) {
        _call_screen->set_muted(muted);
    }
    INFO(muted ? "LXST: Mic muted" : "LXST: Mic unmuted");
}

void UIManager::call_request_hangup() {
    _call_commands.requestHangup(
        call_current_generation());
}

void UIManager::call_request_mute(bool muted) {
    _call_commands.requestMute(
        call_current_generation(), muted);
}

uint32_t UIManager::call_begin_generation() {
    return _call_generation_guard.tryReserve();
}

void UIManager::call_clear_generation(uint32_t expected_generation) {
    // Callback ownership was detached before Link teardown/reset. Release call
    // admission last so a new owner cannot overlap old owner-side cleanup.
    _call_generation_guard.release(expected_generation);
}

uint32_t UIManager::call_current_generation() const {
    return _call_generation_guard.current();
}

bool UIManager::call_extract_link_id(
    const Link& link, CallLinkOwnership::LinkId& id) {
    // link_id() asserts on a default/invalid Link in pinned microReticulum.
    if (!link) return false;
    const Bytes& link_id = link.link_id();
    if (link_id.size() != id.size()) return false;
    for (size_t i = 0; i < id.size(); ++i) id[i] = link_id[i];
    return true;
}

bool UIManager::call_publish_link(const Link& link, uint32_t generation) {
    CallLinkOwnership::LinkId id{};
    return call_extract_link_id(link, id) &&
           _call_link_ownership.publish(generation, id);
}

bool UIManager::call_owns_link(
    const CallLinkOwnership::LinkId& id, uint32_t generation) const {
    return generation != 0 &&
           _call_generation_guard.owns(generation) &&
           _call_link_ownership.owns(generation, id);
}

void UIManager::call_teardown_audio() {
    if (!_lxst_audio) return;
    _lxst_audio->stopCapture();
    _lxst_audio->stopPlayback();
    _lxst_audio->deinit();
    delete _lxst_audio;
    _lxst_audio = nullptr;
}

void UIManager::call_send_signal(int signal) {
    call_send_signal_on_link(_call_link, signal);
}

void UIManager::call_send_terminal_burst() {
    if (!_call_link || _call_link.status() != Type::Link::ACTIVE) return;

    for (int attempt = 0; attempt < TERMINAL_SIGNAL_SEND_COUNT; ++attempt) {
        call_send_signal(LXST_STATUS_TERMINATED);
        if (attempt + 1 < TERMINAL_SIGNAL_SEND_COUNT) {
            delay(TERMINAL_SIGNAL_DRAIN_MS);
        }
    }
}

void UIManager::call_send_signal_on_link(const Link& link, int signal) {
    if (!link || link.status() != Type::Link::ACTIVE) return;

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
        Packet packet(link, signal_data);
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
    // Loopback test mode has no link — skip the link guard and route the
    // built wire packet back through the RX parser instead of sending it.
    if (!_call_loopback && (!_call_link || _call_link.status() != Type::Link::ACTIVE)) {
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
    // Each batch = [codec_type(0x02)] + [mode_header] + [N * raw_codec2].
    // Production Pyxis sends the ULBW 400 ms quantum: 10 Codec2-700C frames.
    // batch_len is the EXACT byte length of one batch, computed by the caller from
    // the real encoded size (codec_type + mode_header + N*raw_codec2). It VARIES by
    // ULBW batch length is 42 bytes: codec type + mode header + 10*4 bytes.
    // (Previously hardcoded to 82, which shipped 40 bytes of uninitialized stack and
    // a lying bin8 length on every 700C packet -- the voice-quality root cause.)

    uint8_t packet_buf[256];
    int pos = 0;

    packet_buf[pos++] = 0x81;  // fixmap(1)
    packet_buf[pos++] = 0x01;  // key: FIELD_FRAMES

    if (batch_count == 1) {
        // Single batch: bare bin8
        packet_buf[pos++] = 0xC4;               // bin8
        packet_buf[pos++] = (uint8_t)batch_len;
        memcpy(packet_buf + pos, batch_data, batch_len);
        pos += batch_len;
    } else {
        // Multiple batches: fixarray(N) of bin8 entries, each batch_len bytes (all
        // share one Codec2 mode). NOTE: pyxis only ever sends batch_count==1 today;
        // true variable-length multi-batch would need per-batch lengths passed in.
        packet_buf[pos++] = 0x90 | (uint8_t)batch_count;  // fixarray(N), N≤15
        for (int b = 0; b < batch_count; b++) {
            packet_buf[pos++] = 0xC4;               // bin8
            packet_buf[pos++] = (uint8_t)batch_len;
            memcpy(packet_buf + pos, batch_data + b * batch_len, batch_len);
            pos += batch_len;
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

    if (_call_loopback) {
        // Loopback: feed the just-built wire packet straight back through the
        // RX parser so the real framing + parse + decode path runs locally.
        // call_on_packet() does NOT re-enter pump_call_tx(), so this is a
        // bounded synchronous chain (no infinite loop, all on core 1).
        call_on_packet(Bytes(packet_buf, pos));
        return;
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
    // Guard: packets can arrive after hangup from the network pipeline.
    // In loopback mode _call_state stays IDLE, so bypass the IDLE guard.
    if (!_lxst_audio || (!_call_loopback && _call_state == CallState::IDLE)) return;
    if (!frame || frame_len < 2) {
        WARNING("LXST: RX audio dropped (truncated frame)");
        return;
    }

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

    // The encoder and decoder deliberately share one Codec2 state object. Never
    // pass a wider-mode header into decode(), since its dynamic mode switch would
    // also mutate the transmitter away from the ULBW-only production contract.
    if (!ULBWVoiceProfilePolicy::acceptsCodec2ModeHeader(codec_data[0])) {
        char dbg[72];
        snprintf(dbg, sizeof(dbg),
                 "LXST: RX audio dropped (non-ULBW mode 0x%02X)", codec_data[0]);
        WARNING(dbg);
        return;
    }

    if (_lxst_audio->isPlaying()) {
        if (_lxst_audio->writeEncodedPacket(codec_data, codec_data_len)) {
            _call_audio_rx_count++;
            if (!_call_loopback && _call_state == CallState::ACTIVE) {
                _call_liveness.observe(millis());
            }
            if (_call_audio_rx_count <= 3) {
                char dbg[80];
                snprintf(dbg, sizeof(dbg), "LXST: RX audio #%lu mode=0x%02X len=%d",
                         (unsigned long)_call_audio_rx_count, codec_data[0], (int)codec_data_len);
                INFO(dbg);
            }
        } else {
            WARNING("LXST: RX audio dropped (invalid Codec2 frame)");
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
        // Signalling: {0x00: [signal, ...]}. LXST 0.5.1 combines the
        // preferred profile and duplex mode in one two-element array; older
        // peers send one signal per packet.
        int signals[LXSTSignalParser::MAX_SIGNALS] = {};
        const size_t signal_count = LXSTSignalParser::parse(
            buf, data.size(), signals, LXSTSignalParser::MAX_SIGNALS);
        if (signal_count == 0) {
            WARNING("LXST: Unparseable signalling packet");
            return;
        }

        for (size_t i = 0; i < signal_count; ++i) {
            const int signal = signals[i];

            // Remote sends PREFERRED_PROFILE + profile_id to request a
            // transmit profile. Pyxis is ULBW-only for LoRa: never adopt the
            // request, and respond with ULBW so the peer switches to 700C.
            if (signal >= LXST_PREFERRED_PROFILE) {
                const int remote_profile = signal - LXST_PREFERRED_PROFILE;
                char dbg[80];
                snprintf(dbg, sizeof(dbg),
                         "LXST: Remote prefers profile 0x%02X, responding 0x%02X",
                         remote_profile, _preferred_profile);
                INFO(dbg);
                call_send_signal(ULBWVoiceProfilePolicy::preferredProfileSignal());
                continue;
            }

            // Pyxis voice is always full duplex. Accept FDX and explicitly
            // counter an HDX request without treating mode as call status.
            if (signal >= LXST_PREFERRED_MODE) {
                const int remote_mode = signal - LXST_PREFERRED_MODE;
                if (remote_mode != LXST_MODE_FULL_DUPLEX) {
                    INFO("LXST: Remote requested non-FDX mode, responding FDX");
                    call_send_signal(LXST_PREFERRED_MODE + LXST_MODE_FULL_DUPLEX);
                }
                continue;
            }

            {
                char dbg[48];
                snprintf(dbg, sizeof(dbg), "LXST: Received signal 0x%02X (queued)", signal);
                INFO(dbg);
            }

            // Enqueue for processing in call_update() under LVGL lock.
            uint8_t w = _call_signal_write;
            uint8_t next_w = (w + 1) % SIGNAL_QUEUE_SIZE;
            if (next_w != _call_signal_read) {  // Not full
                _call_signal_queue[w] = static_cast<uint8_t>(signal);
                _call_signal_write = next_w;
            } else {
                WARNING("LXST: Signal queue full, dropping signal!");
            }
        }

    } else if (field == 0x01) {
        // Audio: {0x01: value} where value is either:
        //   - bin8/bin16: single frame (codec_header + frame_data)
        //   - fixarray: batched frames [bin8(...), bin8(...), ...]
        // Audio buffer writes don't touch LVGL — safe to process here

        // Loopback test mode routes audio here with _call_state == IDLE.
        if ((!_call_loopback && _call_state != CallState::ACTIVE && _call_state != CallState::CONNECTING)
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

    // Terminal is generation-scoped and valid in every owned non-idle call
    // state. call_ended() is idempotent with the subsequent Link-close callback
    // because it clears link ownership before teardown.
    if (signal == LXST_STATUS_TERMINATED) {
        if (_call_state != CallState::IDLE ||
            call_current_generation() != 0) {
            INFO("LXST: Remote terminated call");
            call_ended();
        }
        return;
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
                // Tell remote our preferred profile (default ULBW = Codec2-700C)
                call_send_signal(ULBWVoiceProfilePolicy::preferredProfileSignal());
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

                int codec_mode = profile_to_codec2_mode(_preferred_profile);
                if (codec_mode < 0) codec_mode = CODEC2_MODE_700C;
                if (!_lxst_audio) {
                    _lxst_audio = new LXSTAudio();
                }
                lxst_breadcrumb(21, ESP.getFreeHeap());
                if (!_lxst_audio->init(codec_mode)) {
                    WARNING("LXST: Audio init failed");
                    call_ended();
                    return;
                }
                INFOF("LXST: Audio initialized (internal=%u largest=%u)",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
                lxst_breadcrumb(22, ESP.getFreeHeap());
                // Start full-duplex audio (mic + speaker)
                if (!_lxst_audio->startFullDuplex()) {
                    WARNING("LXST: Full-duplex start failed");
                    call_ended();
                    return;
                }
                _lxst_audio->setCaptureMute(_call_muted);
                lxst_breadcrumb(23, ESP.getFreeHeap());

            } else if (signal == LXST_STATUS_ESTABLISHED) {
                INFO("LXST: Call established!");
                _call_state = CallState::ACTIVE;
                _call_start_ms = millis();
                _call_liveness.arm(_call_start_ms);
                _call_screen->set_state(CallScreen::CallState::ACTIVE);
                lxst_breadcrumb(24, ESP.getFreeHeap());

                int codec_mode = profile_to_codec2_mode(_preferred_profile);
                if (codec_mode < 0) codec_mode = CODEC2_MODE_700C;
                if (!_lxst_audio) {
                    _lxst_audio = new LXSTAudio();
                    if (!_lxst_audio->init(codec_mode)) {
                        WARNING("LXST: Audio init failed");
                        call_ended();
                        return;
                    }
                }
                lxst_breadcrumb(25, ESP.getFreeHeap());
                if (!_lxst_audio->isPlaying()) {
                    if (!_lxst_audio->startFullDuplex()) {
                        WARNING("LXST: Full-duplex start failed");
                        call_ended();
                        return;
                    }
                    _lxst_audio->setCaptureMute(_call_muted);
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
                _call_liveness.arm(_call_start_ms);
                _call_screen->set_state(CallScreen::CallState::ACTIVE);

                // Ensure full-duplex is running
                if (_lxst_audio && !_lxst_audio->isPlaying()) {
                    if (!_lxst_audio->startFullDuplex()) {
                        WARNING("LXST: Full-duplex start failed");
                        call_ended();
                        return;
                    }
                    _lxst_audio->setCaptureMute(_call_muted);
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

    const uint32_t generation =
        call_current_generation();

    // Reject all callbacks before shared Link teardown/reset. Keep the call
    // generation reserved until owner-side cleanup is complete.
    _call_link_ownership.clear(generation);

    call_teardown_audio();

    // Teardown link
    if (_call_link) {
        _call_link.teardown();
        _call_link = Link(Type::NONE);
    }

    _call_peer_hash = Bytes();
    _call_dest_hash = Bytes();
    _call_start_ms = 0;
    _call_timeout_ms = 0;
    _call_muted = false;
    _call_answer_pending.store(false, std::memory_order_release);
    _call_signal_write = 0;
    _call_signal_read = 0;
    _call_audio_rx_count = 0;
    _call_audio_tx_count = 0;
    _call_commands.take();
    _call_liveness.disarm();
    _call_state = CallState::IDLE;
    call_clear_generation(generation);

    _call_screen->set_state(CallScreen::CallState::ENDED);

    if (_navigation.current() == Route::CALL) back();
}

void UIManager::pump_call_tx() {
    // In loopback test mode _call_state is IDLE and there is no link, but mic
    // capture is running — drain the encoded packets and feed the local
    // loopback (see call_send_audio_batch). All bypasses gate on _call_loopback.
    if (!_call_loopback && _call_state == CallState::IDLE) return;
    if (!_lxst_audio || !_lxst_audio->isCapturing()) return;
    if (!_call_loopback && (!_call_link || _call_link.status() != Type::Link::ACTIVE)) return;

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

        // Prepend codec type byte: [0x02] + [mode_header + N raw Codec2 frames].
        uint8_t batch_data[128];
        batch_data[0] = LXST_CODEC_CODEC2;
        memcpy(batch_data + 1, encoded_buf, encoded_len);
        int batch_len = 1 + encoded_len;

        const int framesPerPacket = ULBWVoiceProfilePolicy::framesPerPacket(320);
        call_send_audio_batch(batch_data, batch_len, 1, framesPerPacket);
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

void UIManager::start_loopback() {
    // Don't stomp a live real call. (The harness never overlaps the two, but
    // be defensive: a real call owns _lxst_audio and must not be torn down.)
    if (_call_state != CallState::IDLE ||
        call_current_generation() != 0) {
        WARNING("LXST: Loopback refused — call in progress");
        return;
    }

    // Always (re)create the pipeline so it picks up the currently selected
    // profile/codec mode (driven by T:CALL_PROFILE). Mirrors call_answer().
    call_teardown_audio();
    _call_audio_rx_count = 0;
    _call_audio_tx_count = 0;

    _lxst_audio = new LXSTAudio();
    int codec_mode = profile_to_codec2_mode(_preferred_profile);
    if (codec_mode < 0) codec_mode = CODEC2_MODE_700C;
    if (!_lxst_audio->init(codec_mode)) {
        WARNING("LXST: Loopback audio init failed");
        call_teardown_audio();
        return;
    }
    // Same start path a real call uses: mic + speaker simultaneously, so
    // isCapturing() (pump_call_tx) and isPlaying() (writeEncodedPacket) hold.
    if (!_lxst_audio->startFullDuplex()) {
        WARNING("LXST: Loopback full-duplex start failed");
        call_teardown_audio();
        return;
    }

    _call_loopback = true;       // enable bypass branches BEFORE arming the dump
    pyxis_audio_dump_arm(true);  // resets the running PCM byte offset to 0
    INFO("LXST: Loopback started (codec mode set by profile)");
}

void UIManager::stop_loopback() {
    // Disarm + clear the flag FIRST so pump_call_tx()/writeEncodedPacket()
    // stop touching _lxst_audio before we tear it down.
    _call_loopback = false;
    pyxis_audio_dump_arm(false);

    call_teardown_audio();
    INFO("LXST: Loopback stopped");
}

void UIManager::call_update() {
    uint32_t now = millis();

    // Consume only the current owner's atomically generation-bound close. No
    // shared Link read is needed on this callback-to-loopTask handoff.
    if (_call_link_ownership.takeClosed() != 0) {
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
    if (_call_answer_pending.exchange(false, std::memory_order_acq_rel)) {
        call_answer();
    }

    // Show incoming call UI (deferred from link callback to LVGL-safe context)
    if (_call_state == CallState::INCOMING_RINGING && _navigation.current() != Route::CALL) {
        _call_screen->set_peer(_call_peer_hash);
        _call_screen->set_state(CallScreen::CallState::INCOMING_RINGING);
        _call_screen->set_muted(false);
        navigate(Route::CALL);

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
            const uint32_t generation = call_current_generation();
            // Register callbacks before the constructor sends the link request,
            // then publish its exact ID immediately after construction.
            _call_link = Link(peer_dest, on_call_link_established, on_call_link_closed);
            if (!call_publish_link(_call_link, generation)) {
                WARNING("LXST: Link has invalid ID, aborting call");
                call_ended();
                return;
            }
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
            case CallState::INCOMING_IDENTIFYING:
                WARNING("LXST: Incoming caller identification timed out");
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

        if (_call_state == CallState::ACTIVE &&
            _call_liveness.expired(now, CALL_MEDIA_LIVENESS_TIMEOUT_MS)) {
            WARNING("LXST: Call media liveness timed out");
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
    LVGL_LOCK();
    auto* self = s_call_instance;

    CallLinkOwnership::LinkId link_id{};
    if (!call_extract_link_id(link, link_id)) {
        WARNING("LXST: Established link has invalid ID (ignoring)");
        return;
    }
    const uint32_t generation =
        self->_call_link_ownership.generationFor(link_id);
    if (!self->call_owns_link(link_id, generation) ||
        self->_call_state != CallState::LINK_ESTABLISHING) {
        WARNING("LXST: Stale outgoing link established (ignoring)");
        return;
    }

    char buf[80];
    snprintf(buf, sizeof(buf), "LXST: Outgoing link established (status=%d)", (int)link.status());
    INFO(buf);

    // Register through the validated callback argument; never read the
    // manager's shared Link object from a transport-thread callback.
    link.set_packet_callback(on_call_link_packet);
    link.set_link_closed_callback(on_call_link_closed);
    INFO("LXST: Packet callback registered on outgoing link");

    // Transition to waiting for STATUS_AVAILABLE
    self->_call_state = CallState::WAIT_AVAILABLE;
    self->_call_timeout_ms = millis() + 10000;  // 10s timeout
    INFO("LXST: Waiting for STATUS_AVAILABLE (10s timeout)");
}

void UIManager::on_call_link_closed(Link& link) {
    if (!s_call_instance) return;
    LVGL_LOCK();
    auto* self = s_call_instance;

    CallLinkOwnership::LinkId link_id{};
    if (!call_extract_link_id(link, link_id)) {
        WARNING("LXST: Closed link has invalid ID (ignoring)");
        return;
    }
    const uint32_t generation =
        self->_call_link_ownership.generationFor(link_id);
    if (!self->call_owns_link(link_id, generation)) {
        WARNING("LXST: Stale link closed (ignoring)");
        return;
    }

    WARNING("LXST: Link closed (deferred)");

    // Bind pending and generation in one CAS. If ownership changed after the
    // validation above, this exact old state cannot overwrite the new slot.
    if (!self->_call_link_ownership.markClosed(generation, link_id)) {
        WARNING("LXST: Link owner changed before close deferral (ignoring)");
    }
}

void UIManager::on_call_link_packet(const Bytes& plaintext, const Packet& packet) {
    if (!s_call_instance) return;
    LVGL_LOCK();
    auto* self = s_call_instance;
    const Link& callback_link = packet.link();
    CallLinkOwnership::LinkId link_id{};
    if (!call_extract_link_id(callback_link, link_id)) {
        WARNING("LXST: Packet link has invalid ID (ignoring)");
        return;
    }
    const uint32_t generation =
        self->_call_link_ownership.generationFor(link_id);
    if (!self->call_owns_link(link_id, generation)) {
        WARNING("LXST: Stale link packet (ignoring)");
        return;
    }
    self->call_on_packet(plaintext);
}

// ── LXST Incoming Call Callbacks ──

void UIManager::on_lxst_link_established(Link& link) {
    if (!s_call_instance) {
        link.teardown();
        return;
    }
    LVGL_LOCK();
    auto* self = s_call_instance;

    CallLinkOwnership::LinkId link_id{};
    if (!call_extract_link_id(link, link_id)) {
        WARNING("LXST: Incoming link has invalid ID (rejecting)");
        link.teardown();
        return;
    }

    // Incoming and outgoing admissions are ordered by the recursive LVGL
    // mutex, including when an interface worker dispatches this callback. The
    // atomic generation reservation remains the definitive ownership decision.
    const uint32_t generation = self->call_begin_generation();
    lxst_breadcrumb(10, ESP.getFreeHeap());
    INFO("LXST: Incoming link established");
    if (generation == 0) {
        // Another call owns the manager. Reject using only this callback's link;
        // never touch the stored owner link or any owner-side call state.
        INFO("LXST: Busy, rejecting incoming link");
        uint8_t busy_buf[4] = { 0x81, 0x00, 0x91, LXST_STATUS_BUSY };
        Bytes busy_data(busy_buf, 4);
        Packet pkt(link, busy_data);
        pkt.send();
        link.teardown();
        return;
    }

    // Store the accepted Link before publishing its exact 128-bit ID. No
    // callbacks are installed on the stored owner until publication and call
    // state initialization are complete.
    lxst_breadcrumb(11, ESP.getFreeHeap());
    self->_call_link = link;
    if (!self->_call_link_ownership.publish(generation, link_id)) {
        WARNING("LXST: Incoming link has invalid ID (rejecting)");
        self->_call_link.teardown();
        self->_call_link = Link(Type::NONE);
        self->_call_generation_guard.release(generation);
        return;
    }
    self->_call_state = CallState::INCOMING_IDENTIFYING;
    self->_call_peer_hash = Bytes();
    self->_call_dest_hash = Bytes();
    self->_call_muted = false;
    self->_call_answer_pending.store(false, std::memory_order_release);
    self->_call_signal_write = 0;
    self->_call_signal_read = 0;
    self->_call_audio_rx_count = 0;
    self->_call_audio_tx_count = 0;
    self->_call_commands.take();
    self->_call_timeout_ms = millis() + INCOMING_IDENTIFY_TIMEOUT_MS;

    // Wait for the accepted caller to identify and observe closure. Install on
    // the validated callback argument, not the manager's shared Link handle.
    link.set_remote_identified_callback(on_lxst_caller_identified);
    link.set_link_closed_callback(on_call_link_closed);

    // Send STATUS_AVAILABLE
    lxst_breadcrumb(12, ESP.getFreeHeap());
    call_send_signal_on_link(link, LXST_STATUS_AVAILABLE);

    lxst_breadcrumb(13, ESP.getFreeHeap());
    INFO("LXST: Waiting for caller identity (15s timeout)");
    lxst_breadcrumb(14, ESP.getFreeHeap());
}

void UIManager::on_lxst_caller_identified(const Link& link, const Identity& identity) {
    if (!s_call_instance) return;
    LVGL_LOCK();
    auto* self = s_call_instance;
    lxst_breadcrumb(15, ESP.getFreeHeap());

    CallLinkOwnership::LinkId link_id{};
    if (!call_extract_link_id(link, link_id)) {
        WARNING("LXST: Identified link has invalid ID (ignoring)");
        return;
    }
    // Bind identity to the exact current owner ID and generation. Validation
    // never reads the manager's shared Link object.
    const uint32_t generation =
        self->_call_link_ownership.generationFor(link_id);
    if (!self->call_owns_link(link_id, generation) ||
        self->_call_state != CallState::INCOMING_IDENTIFYING) {
        WARNING("LXST: Stale caller identified (ignoring)");
        return;
    }

    std::string hash_hex = identity.hash().toHex().substr(0, 16);
    INFO(("LXST: Caller identified: " + hash_hex + "...").c_str());

    // Store peer info
    self->_call_peer_hash = identity.hash();

    // Set callbacks and send through a callback-local shared-object copy, not
    // through the manager's concurrently resettable Link handle.
    Link callback_link(link);
    callback_link.set_packet_callback(on_call_link_packet);

    // Identification completed for the reserved owner; ringing can now become
    // visible and actionable on the next call_update().
    self->_call_state = CallState::INCOMING_RINGING;

    // Send STATUS_RINGING
    call_send_signal_on_link(callback_link, LXST_STATUS_RINGING);

    // Incoming ringing UI will be shown in call_update().
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
    {
        int codec_mode = profile_to_codec2_mode(_preferred_profile);
        if (codec_mode < 0) codec_mode = CODEC2_MODE_700C;
        if (!_lxst_audio->init(codec_mode)) {
            WARNING("LXST: Audio init failed");
            call_ended();
            return;
        }
    }
    INFOF("LXST: Audio initialized (internal=%u largest=%u)",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    lxst_breadcrumb(32, ESP.getFreeHeap());

    // Start full-duplex audio (mic + speaker)
    if (!_lxst_audio->startFullDuplex()) {
        WARNING("LXST: Full-duplex start failed");
        call_ended();
        return;
    }
    _lxst_audio->setCaptureMute(_call_muted);
    INFOF("LXST: Full-duplex ready (internal=%u largest=%u)",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    lxst_breadcrumb(33, ESP.getFreeHeap());

    // Send profile preference (default ULBW = Codec2-700C). Answerer
    // sends last and wins.
    call_send_signal(ULBWVoiceProfilePolicy::preferredProfileSignal());

    // Send STATUS_ESTABLISHED
    call_send_signal(LXST_STATUS_ESTABLISHED);

    // Transition to active call
    _call_state = CallState::ACTIVE;
    _call_start_ms = millis();
    _call_liveness.arm(_call_start_ms);
    INFO("LXST: Call active (answerer, full-duplex)");
}

void UIManager::announce_lxst() {
    if (_lxst_destination) {
        std::string h = _lxst_destination.hash().toHex();
        INFO(("Announcing LXST telephony destination: " + h).c_str());
        _lxst_destination.announce();
        INFO("LXST announce sent");
    } else {
        WARNING("announce_lxst skipped: _lxst_destination not constructed");
    }
}

} // namespace LXMF
} // namespace UI

#endif // ARDUINO
