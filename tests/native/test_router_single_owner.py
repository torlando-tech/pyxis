from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "src/main.cpp").read_text()
UI_MANAGER = (ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.cpp").read_text()


def test_lxmf_router_pumps_have_one_main_loop_owner():
    """Router processing must not be duplicated inside the render-lock owner."""
    assert MAIN.count("router->process_outbound();") == 1
    assert MAIN.count("router->process_inbound();") == 1
    assert MAIN.count("router->process_sync();") == 1
    assert "_router.process_outbound();" not in UI_MANAGER
    assert "_router.process_inbound();" not in UI_MANAGER
