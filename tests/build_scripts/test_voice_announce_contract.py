from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "src" / "main.cpp").read_text()
UI_MANAGER = (ROOT / "lib" / "tdeck_ui" / "UI" / "LXMF" / "UIManager.cpp").read_text()


def test_all_main_loop_announces_use_the_paired_destination_helper():
    helper_start = MAIN.index("void announce_reachable_destinations()")
    helper_end = MAIN.index("\n}\n", helper_start) + 2
    helper = MAIN[helper_start:helper_end]

    assert "router->announce();" in helper
    assert "ui_manager->announce_lxst();" in helper
    assert MAIN.count("router->announce();") == 1


def test_tcp_online_edge_reannounces_both_destinations():
    status_block = MAIN[MAIN.index("bool tcp_online ="): MAIN.index("// Update BLE peer info")]

    assert "tcp_online && !last_tcp_online" in status_block
    assert "announce_reachable_destinations();" in status_block


def test_manual_announce_action_pairs_lxmf_and_lxst():
    callback_start = UI_MANAGER.index("_announce_list_screen->set_send_announce_callback")
    callback_end = UI_MANAGER.index("// Set up callbacks for status screen", callback_start)
    callback = UI_MANAGER[callback_start:callback_end]

    assert "_router.announce();" in callback
    assert "announce_lxst();" in callback
