"""Source-level contract for inbound SIGNATURE_INVALID drop in on_message_received.

The microLXMF router rejects SIGNATURE_INVALID on the opportunistic and direct
paths, but the propagated (store-and-forward) path queues them unchecked, so
UIManager::on_message_received is the single choke point that must drop them
before ANY side effect. These checks lock in that ordering and the enum
specificity (drop SIGNATURE_INVALID only, never SOURCE_UNKNOWN or validated).
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.cpp"


def handler_body(source: str) -> str:
    start = source.index("void UIManager::on_message_received(")
    end = source.index("void UIManager::on_message_delivered(", start)
    return source[start:end]


def test_drop_gate_precedes_all_side_effects():
    source = CPP.read_text()
    handler = handler_body(source)

    # The drop gate is keyed on the SIGNATURE_INVALID enum inside an "if"
    # condition. Locate that condition (the only place the handler checks
    # the enum); its position is the gate's entry point.
    drop = handler.index("::LXMF::Type::Message::SIGNATURE_INVALID")
    gate_if = handler.rfind("if (!message.signature_validated() &&", 0, drop)
    assert gate_if >= 0, "drop gate must be an if(!validated && reason==SIGNATURE_INVALID)"

    # Every side effect of a received message must come AFTER the drop gate,
    # so a spoofed message triggers none of them.
    for label, marker in [
        ("initial log", '"Message received from "'),
        ("test-hook record", "pyxis_test_hook_record_rx(message)"),
        ("key request", "request_key_for_unknown_source(message)"),
        ("location ingest", "_peer_locations.apply"),
        ("persistence", "if (!_store.save_message(message))"),
        ("chat render", "_chat_screen->add_message(message, false)"),
        ("notification", "tone_play("),
    ]:
        pos = handler.index(marker)
        assert drop < pos, f"drop gate must precede {label}"


def test_drop_gate_is_specific_to_signature_invalid():
    source = CPP.read_text()
    handler = handler_body(source)

    drop = handler.index("::LXMF::Type::Message::SIGNATURE_INVALID")

    # The gate must be a single if(!validated && reason==SIGNATURE_INVALID)
    # block that logs and returns. A validated message short-circuits on the
    # first term; a SOURCE_UNKNOWN message short-circuits on the second.
    gate_start = handler.rfind("if (!message.signature_validated() &&", 0, drop)
    assert gate_start >= 0
    gate_end = handler.index("return;", drop)
    gate = handler[gate_start:gate_end]
    assert "!message.signature_validated() &&" in gate
    assert "message.unverified_reason() ==\n            ::LXMF::Type::Message::SIGNATURE_INVALID" in gate
    # It must log before the return (so a dropped spoof is observable in the
    # serial log without leaking message content).
    assert "Dropping message with invalid signature from" in gate
    # The return immediately follows the log (the gate returns, it does not
    # fall through to the side effects).
    assert handler[gate_end:gate_end + len("return;")] == "return;"
    assert gate.index("Dropping message with invalid signature from") > gate.index(
        "SIGNATURE_INVALID"
    )


def test_source_unknown_still_reaches_key_request():
    source = CPP.read_text()
    handler = handler_body(source)

    # A SOURCE_UNKNOWN message passes the drop gate (its reason is not
    # SIGNATURE_INVALID) and must still reach the key request, which fires a
    # path request so the peer's announce can be learned. The key-request
    # call sits AFTER the drop gate and is gated only on
    # !signature_validated(), so first-contact traffic is unaffected.
    drop = handler.index("::LXMF::Type::Message::SIGNATURE_INVALID)")
    key = handler.index("request_key_for_unknown_source(message)")
    assert drop < key
    # The key-request call is guarded by !signature_validated() (the
    # first-contact case), not by SIGNATURE_INVALID.
    guard = handler[handler.rfind("if (", 0, key):key]
    assert "!message.signature_validated()" in guard
    assert "SIGNATURE_INVALID" not in guard


def test_validated_messages_are_not_affected():
    source = CPP.read_text()
    handler = handler_body(source)

    # The drop gate's first term is !signature_validated(), so a message that
    # validated short-circuits and skips the drop entirely — it proceeds to
    # the (unchanged) location ingest and persistence.
    drop = handler.index("::LXMF::Type::Message::SIGNATURE_INVALID)")
    gate = handler[handler.rfind("if (", 0, drop):drop]
    assert gate.index("!message.signature_validated()") >= 0
    # Persistence and render remain present for the happy path.
    assert "if (!_store.save_message(message))" in handler
    assert "_chat_screen->add_message(message, false)" in handler
