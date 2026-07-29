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
        main.index("// Update UI manager")
    ]
    assert "RouterLock" in network_pump
