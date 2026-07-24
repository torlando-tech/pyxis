"""Source-level regression checks for fail-closed LXMF persistence UI flow."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
UI_MANAGER = REPO_ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.cpp"
CHAT_SCREEN = REPO_ROOT / "lib/tdeck_ui/UI/LXMF/ChatScreen.cpp"
COMPOSE_SCREEN = REPO_ROOT / "lib/tdeck_ui/UI/LXMF/ComposeScreen.cpp"


def function_body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def test_outgoing_message_is_committed_before_display_and_send():
    source = UI_MANAGER.read_text()
    body = function_body(
        source,
        "bool UIManager::send_message(",
        "void UIManager::on_message_received(",
    )

    save = body.index("if (!_store.save_message(message))")
    display = body.index("_chat_screen->add_message(message, true)")
    send = body.index("_router.handle_outbound(message)")

    assert save < display < send
    assert "Outgoing message persistence failed; message not queued" in body
    assert "The message was not sent" in body


def test_incoming_message_is_committed_before_display():
    source = UI_MANAGER.read_text()
    body = function_body(
        source,
        "void UIManager::on_message_received(",
        "void UIManager::on_message_delivered(",
    )

    save = body.index("if (!_store.save_message(message))")
    display = body.index("_chat_screen->add_message(message, false)")

    assert save < display
    assert "Incoming message persistence failed; message not added to history" in body
    assert "An incoming message could not be saved" in body


def test_no_message_save_result_is_silently_ignored():
    source = UI_MANAGER.read_text()
    assert "\n    _store.save_message(message);" not in source


def test_delivery_state_is_committed_before_ui_update():
    source = UI_MANAGER.read_text()
    delivered = function_body(
        source,
        "void UIManager::on_message_delivered(",
        "void UIManager::on_message_failed(",
    )
    failed = function_body(
        source,
        "void UIManager::on_message_failed(",
        "void UIManager::refresh_current_screen(",
    )
    assert delivered.index("_store.update_message_state(") < delivered.index(
        "_chat_screen->update_message_status(message.hash(), true)"
    )
    assert failed.index("_store.update_message_state(") < failed.index(
        "_chat_screen->update_message_status(message.hash(), false)"
    )


def test_rejected_outgoing_message_keeps_retryable_input():
    chat = CHAT_SCREEN.read_text()
    compose = COMPOSE_SCREEN.read_text()
    assert "if (screen->_send_message_callback(message))" in chat
    assert "if (screen->_send_callback && screen->_send_callback(dest_hash, message))" in compose


def test_storage_error_dialogs_are_coalesced():
    source = UI_MANAGER.read_text()
    assert "if (storage_error_dialog) return;" in source
    assert "storage_error_dialog = nullptr;" in source
