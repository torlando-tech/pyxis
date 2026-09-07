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
    # The clear must compare the current composer text against the
    # captured submitted text and bail when they differ.
    assert "_pending_submitted_text" in body, (
        "clear_composer() no longer consults the submitted-text generation"
    )
    assert "lv_textarea_get_text(_text_area)" in body
    assert "return;" in body, (
        "clear_composer() must skip the clear when the composer was edited"
    )


def test_send_click_records_submitted_text():
    source = chat_source()
    body = function_body(
        source,
        "void ChatScreen::on_send_clicked(",
        "void ChatScreen::clear_composer()",
    )
    assert "_pending_submitted_text" in body, (
        "on_send_clicked() must capture the submitted composer text on "
        "acceptance so the later completion can identify it"
    )
    assert "_send_message_callback(message)" in body


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
