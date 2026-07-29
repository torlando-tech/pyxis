from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
UIH = ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.h"
UIC = ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.cpp"
CHAT = ROOT / "lib/tdeck_ui/UI/LXMF/ChatScreen.cpp"
SCREEN = ROOT / "lib/tdeck_ui/UI/LXMF/LocationShareScreen.cpp"

def test_location_controls_are_peer_scoped_and_reachable_only_from_chat():
    h, cpp, chat = UIH.read_text(), UIC.read_text(), CHAT.read_text()
    assert "SCREEN_LOCATION_SHARING" in h
    assert "set_location_callback" in cpp
    assert "LV_SYMBOL_GPS" in chat
    show = cpp[cpp.index("void UIManager::show_location_sharing") : cpp.index("void UIManager::on_back_from_location_sharing")]
    assert "peer_hash.size() != Telemetry::PEER_ID_SIZE" in show
    assert "get_location_share_session" not in show
    assert "requestQuery" in show
    assert "_location_share_screen->open_for_peer" in show
    assert "start_location_sharing" not in show

def test_callbacks_publish_mailbox_and_update_services_before_lvgl():
    cpp, screen = UIC.read_text(), SCREEN.read_text()
    assert "start_callback_(" in screen and "stop_callback_(" in screen
    assert "requestStart" in cpp and "requestStop" in cpp
    assert "start_location_sharing" not in screen
    assert "stop_location_sharing" not in screen
    update = cpp[cpp.index("void UIManager::update()") : cpp.index("void UIManager::show_conversation_list")]
    assert "_location_share_commands.take()" in cpp
    assert "requestQuery" in cpp
    show_body = cpp.split("void UIManager::show_location_sharing", 1)[1].split("void UIManager::on_back_from_location_sharing", 1)[0]
    assert "get_location_share_session" not in show_body
    assert update.index("_location_share_commands.take()") < update.index("_location_persistence_controller->service")
    assert update.index("_location_share_commands.take()") < update.index("LVGL_LOCK();")
    assert update.index("start_location_sharing") < update.index("LVGL_LOCK();")
    assert update.index("stop_location_sharing") < update.index("LVGL_LOCK();")

def test_screen_has_bounded_choices_confirmation_status_and_errors():
    text = SCREEN.read_text()
    for token in ["15 min", "1 hour", "4 hours", "1 min", "5 min", "15 min", "Exact", "100 m", "1 km", "10 km", "Confirm location sharing", "STOPPING"]:
        assert token in text
    assert "indefinite" not in text.lower()
    assert "lv_group_add_obj(group, duration_dropdown_)" in text
    assert "lv_msgbox_get_btns" in text

def test_ui_layer_has_no_direct_scheduler_mutation_contract():
    cpp = UIC.read_text()
    screen = SCREEN.read_text()
    assert "_location_shares.start(" not in cpp
    assert "_location_shares.stop(" not in cpp
    assert "#include \"Telemetry/LocationShareScheduler.h\"" not in screen
    assert ".start(" not in screen and ".stop(" not in screen
    assert "SERVICE ORDER CONTRACT" in cpp
    assert "NO DIRECT SCHEDULER ACCESS CONTRACT" in screen
