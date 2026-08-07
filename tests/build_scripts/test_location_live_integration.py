from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.cpp"
HEADER = ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.h"


def test_live_location_dispatch_uses_authenticated_scheduler_and_guarded_router_before_lvgl():
    cpp = CPP.read_text()
    header = HEADER.read_text()
    update = cpp[cpp.index("void UIManager::update()") : cpp.index("void UIManager::show_conversation_list")]

    assert "Telemetry::LocationShareScheduler _location_shares;" in header
    assert "TinyGPSPlus* _gps;" in header
    assert "locationTelemetryFromGpsFix" in update
    assert "dispatchLocationShare" in update
    assert update.index("dispatchLocationShare") < update.index("LVGL_LOCK();")

    assert "class LiveLocationEnvelopeRouter" in cpp
    router_block = cpp[cpp.index("class LiveLocationEnvelopeRouter") : cpp.index("UIManager::UIManager")]
    assert "try_handle_outbound" in router_block
    assert "OutboundAdmissionResult::ACCEPTED" in router_block
    assert "exclusive_deadline_monotonic_millis" in router_block
    assert "esp_timer_get_time" in cpp
    assert "claimOwnership" in router_block
    assert "fields_set" in router_block
    assert "save_message" not in router_block


def test_live_location_control_surface_is_explicit_opt_in():
    header = HEADER.read_text()
    assert "start_location_sharing(" in header
    assert "stop_location_sharing(" in header
    assert "get_location_share_session(" in header
    assert "LocationConsentResult start_location_sharing(" in header


def test_live_persistence_is_owned_and_serviced_before_dispatch_and_lvgl():
    cpp = CPP.read_text()
    header = HEADER.read_text()
    main = (ROOT / "src/main.cpp").read_text()
    update = cpp[cpp.index("void UIManager::update()") : cpp.index("void UIManager::show_conversation_list")]
    assert "LocationPersistenceController* _location_persistence_controller;" in header
    assert "LocationStateSnapshot" not in update
    assert "TransactionalLocationPersistence persistence" not in update
    assert update.index("_location_persistence_controller->service") < update.index("dispatchLocationShare")
    assert update.index("_location_persistence_controller->service") < update.index("LVGL_LOCK();")
    assert "location_filesystem_available" in main
    assert "fs.init(false)" in main
    assert "UIManager(*reticulum, *router, *message_store, location_filesystem_available)" in main
    assert "heap_caps_calloc" in cpp


def test_inbound_location_and_consent_controls_use_controller_durability():
    cpp = CPP.read_text()
    inbound = cpp[cpp.index("void UIManager::on_message_received") : cpp.index("void UIManager::on_message_delivered")]
    start = cpp[cpp.index("UIManager::start_location_sharing") : cpp.index("UIManager::get_location_share_session")]
    assert inbound.index("_location_persistence_controller->service") < inbound.index("_peer_locations.apply")
    assert "_location_persistence_controller->startSharing" in start
    assert "_location_persistence_controller->stopSharing" in start


def test_router_pump_is_not_run_under_lvgl_and_transient_delivery_skips_store_update():
    cpp = CPP.read_text()
    main = (ROOT / "src/main.cpp").read_text()
    update = cpp[cpp.index("void UIManager::update()") : cpp.index("void UIManager::show_conversation_list")]
    assert "_router.process_outbound()" not in update
    assert "_router.process_inbound()" not in update

    callback = main[
        main.index("router->register_delivered_callback") :
        main.index("// Boot profiling complete")
    ]
    assert callback.index("load_message(msg_hash)") < callback.index("update_message_state(")
    assert "if (!full_msg.hash())" in callback


def test_live_and_chat_outbound_share_a_router_mutex():
    cpp = CPP.read_text()
    main = (ROOT / "src/main.cpp").read_text()
    router_block = cpp[
        cpp.index("class LiveLocationEnvelopeRouter") :
        cpp.index("UIManager::UIManager")
    ]
    send = cpp[cpp.index("bool UIManager::send_message") : cpp.index("void UIManager::on_message_received")]
    assert "RouterLock" in router_block
    assert "RouterLock" in send
    network_pump = main[
        main.index("// Process Reticulum") :
        main.index("LOOP_STEP(8);  // Memory monitor")
    ]
    assert "RouterLock" in network_pump
    assert "RouterLock router_lock(0)" in send


def test_ble_ingress_and_ui_router_mutators_follow_nonblocking_lock_order():
    cpp = CPP.read_text()
    main = (ROOT / "src/main.cpp").read_text()
    ble = (ROOT / "lib/ble_interface/BLEInterface.cpp").read_text()
    announce = cpp[
        cpp.index("set_send_announce_callback") :
        cpp.index("// Set up callbacks for status screen")
    ]
    propagation = cpp[
        cpp.index("void UIManager::on_propagation_node_selected") :
        cpp.index("void UIManager::set_rns_status")
    ]
    assert "RouterLock router_lock(0)" in announce
    assert propagation.count("RouterLock router_lock(0)") >= 3
    assert main.count("RouterLock router_lock(0)") >= 1
    reassembled = ble[
        ble.index("void BLEInterface::onPacketReassembled") :
        ble.index("size_t BLEInterface::drain_inbound")
    ]
    assert "handle_incoming" not in reassembled
    assert "drain_inbound" in ble
    assert "ble_interface_impl->drain_inbound" in main
    stop = ble[ble.index("void BLEInterface::stop()") : ble.index("void BLEInterface::loop()")]
    assert "_pending_packet_count = 0" in stop
