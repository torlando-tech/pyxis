from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.h"
SOURCE = ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.cpp"


def _function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : index]
    raise AssertionError(f"unterminated function: {signature}")


def test_terminal_status_is_backward_compatible_extension():
    header = HEADER.read_text()
    assert re.search(r"LXST_STATUS_TERMINATED\s*=\s*0x07", header)


def test_local_hangup_sends_bounded_terminal_burst_before_link_teardown():
    source = SOURCE.read_text()
    hangup = _function_body(source, "void UIManager::call_hangup()")
    burst = _function_body(source, "void UIManager::call_send_terminal_burst()")

    assert hangup.index("call_send_terminal_burst();") < hangup.index("_call_link_ownership.clear")
    assert hangup.index("call_send_terminal_burst();") < hangup.index("_call_link.teardown()")
    assert "TERMINAL_SIGNAL_SEND_COUNT = 3" in HEADER.read_text()
    assert "TERMINAL_SIGNAL_DRAIN_MS = 20" in HEADER.read_text()
    assert "call_send_signal(LXST_STATUS_TERMINATED)" in burst
    assert "delay(TERMINAL_SIGNAL_DRAIN_MS)" in burst


def test_terminal_signal_ends_every_owned_non_idle_call_before_state_switch():
    source = SOURCE.read_text()
    handler = _function_body(source, "void UIManager::call_process_signal(uint8_t signal)")

    terminal = handler.index("signal == LXST_STATUS_TERMINATED")
    state_switch = handler.index("switch (_call_state)")
    assert terminal < state_switch
    terminal_block = handler[terminal:state_switch]
    assert "call_current_generation() != 0" in terminal_block
    assert "call_ended();" in terminal_block
    assert "return;" in terminal_block
