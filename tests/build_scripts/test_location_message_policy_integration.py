from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.cpp"
HEADER = ROOT / "lib/tdeck_ui/UI/LXMF/UIManager.h"


def guarded_block(source: str, condition: str) -> tuple[int, int]:
    condition_start = source.index(condition)
    open_brace = source.index("{", condition_start)
    depth = 0
    for index in range(open_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return open_brace, index
    raise AssertionError("unterminated guarded block")


def test_inbound_location_policy_precedes_chat_persistence():
    source = CPP.read_text()
    handler = source[source.index("void UIManager::on_message_received") :]
    authenticated = handler.index("message.signature_validated()")
    receive_millis = handler.index("RNS::Utilities::OS::ltime()")
    classify = handler.index("classifyInboundLocationMessage")
    apply_location = handler.index("_peer_locations.apply")
    silent_return = handler.index("if (!location_decision.persist)")
    save = handler.index("_store.save_message(message)")
    lock = handler.index("LVGL_LOCK();")
    assert authenticated < receive_millis < classify < apply_location < silent_return < save < lock
    assert "message.source_hash()" in handler[:save]
    assert "location_decision.authenticated_sender" in handler[:save]

    guard_start, guard_end = guarded_block(
        handler,
        "if (message.signature_validated() &&\n"
        "        source_hash.size() == Telemetry::PEER_ID_SIZE)",
    )
    guarded = handler[guard_start:guard_end]
    assert "RNS::Utilities::OS::ltime()" in guarded
    assert "classifyInboundLocationMessage" in guarded
    assert "_peer_locations.apply" in guarded
    assert "location_decision.authenticated_sender" in guarded
    assert handler.count("classifyInboundLocationMessage") == 1
    assert handler.count("_peer_locations.apply") == 1


def test_peer_location_store_is_durable_ui_manager_state():
    header = HEADER.read_text()
    assert "Telemetry::PeerLocationStore _peer_locations;" in header
