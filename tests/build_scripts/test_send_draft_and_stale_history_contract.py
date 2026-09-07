"""Source-level contracts for the Greptile P1 remediation on PR #96.

Two regressions introduced by the off-lock threading fixes (1c68860 /
ce92e80) were flagged by Greptile on the exact head and fixed in place:

1. Send Completion Erases Draft — the async send deferral leaves the
   composer un-cleared between the send click and the main-loop's
   ADDED commit. The completion path must therefore only clear the
   composer when it still holds exactly the submitted text, so input
   typed while persistence/admission is in flight survives.

2. Same-Peer History Stays Stale — the same-peer early-return in
   ChatScreen::load_conversation() skips the store re-read, so a
   message that arrived for that peer while the chat was hidden never
   surfaces on re-open. The early-return must now compare the store's
   in-memory conversation count against the count at the last prepare
   commit and re-arm prepare on a mismatch.
"""

from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
CHAT_CPP = ROOT / "lib/tdeck_ui/UI/LXMF/ChatScreen.cpp"
CHAT_H = ROOT / "lib/tdeck_ui/UI/LXMF/ChatScreen.h"


def chat_source() -> str:
    if not CHAT_CPP.is_file():
        pytest.skip("ChatScreen.cpp not found")
    return CHAT_CPP.read_text()


def chat_header() -> str:
    if not CHAT_H.is_file():
        pytest.skip("ChatScreen.h not found")
    return CHAT_H.read_text()


def function_body(source: str, signature: str, end_marker: str) -> str:
    start = source.index(signature)
    end = source.index(end_marker, start)
    return source[start:end]


def test_clear_composer_is_generation_checked():
    source = chat_source()
    body = function_body(
        source,
        "void ChatScreen::clear_composer()",
        "lv_group_focus_obj(_text_area);",
    )
    # The clear must bail when no submission is pending (empty marker) and
    # compare the current composer text against the captured submitted text.
    assert "_pending_submitted_text.empty()" in body, (
        "clear_composer() must no-op when no submission is pending"
    )
    assert "_pending_submitted_text" in body
    assert "lv_textarea_get_text(_text_area)" in body
    assert "return;" in body, (
        "clear_composer() must skip the clear when the composer was edited"
    )


def test_send_path_records_submitted_text_in_lock_section():
    source = chat_source()
    ui_source = (ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.cpp").read_text()
    # The ChatScreen click handler must not assign the marker itself — the
    # marker has to be set inside the callback (UIManager), in the same LVGL
    # lock section as the mailbox publish, or the main loop can observe the
    # mailbox entry before the marker exists (Greptile P1: completion race).
    body = function_body(
        source,
        "void ChatScreen::on_send_clicked(",
        "void ChatScreen::set_pending_submitted_text(",
    )
    assert "_pending_submitted_text" not in body, (
        "on_send_clicked() must not assign the marker outside the callback "
        "lock section (race with the main-loop completion)"
    )
    assert "_send_message_callback(message)" in body
    # The callback-side handler sets the marker right after acceptance,
    # under the same LVGL lock the click handler already holds.
    ui_body = function_body(
        ui_source,
        "bool UIManager::on_send_message_from_chat(const String& content)",
        "void UIManager::on_call_from_chat()",
    )
    assert "set_pending_submitted_text" in ui_body, (
        "on_send_message_from_chat must record the submitted text in the "
        "same LVGL lock section as the mailbox publish"
    )
    assert "send_message(_current_peer_hash, content)" in ui_body


def test_same_peer_reopen_checks_live_message_count():
    source = chat_source()
    body = function_body(
        source,
        "void ChatScreen::load_conversation(",
        "void ChatScreen::prepare_conversation()",
    )
    assert "_prepared_message_count" in body, (
        "load_conversation() same-peer early-return must compare the store's "
        "live conversation count against the prepared count"
    )
    assert "get_messages_for_conversation(peer_hash).size()" in body, (
        "the staleness check must use the in-memory index (no LittleFS)"
    )


def test_prepared_count_recorded_and_reset():
    source = chat_source()
    # Recorded at prepare commit time.
    prepare_body = function_body(
        source,
        "void ChatScreen::prepare_conversation()",
        "void ChatScreen::refresh()",
    )
    assert "_prepared_message_count = _all_message_hashes.size()" in prepare_body
    # Reset by the peer-change path so a fresh prepare always re-records.
    load_body = function_body(
        source,
        "void ChatScreen::load_conversation(",
        "void ChatScreen::prepare_conversation()",
    )
    assert "_prepared_message_count = 0" in load_body
    refresh_body = function_body(
        source,
        "void ChatScreen::refresh()",
        "void ChatScreen::add_message(",
    )
    assert "_prepared_message_count = 0" in refresh_body


def test_pending_submitted_text_field_declared():
    header = chat_header()
    assert "_pending_submitted_text" in header, (
        "ChatScreen must declare the submitted-text capture field"
    )
    assert "_prepared_message_count" in header
