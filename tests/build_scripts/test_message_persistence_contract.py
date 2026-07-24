"""Source-level regression checks for fail-closed LXMF persistence UI flow."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
UI_MANAGER = REPO_ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.cpp"


def function_body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def test_outgoing_message_is_committed_before_display_and_send():
    source = UI_MANAGER.read_text()
    body = function_body(
        source,
        "void UIManager::send_message(",
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
