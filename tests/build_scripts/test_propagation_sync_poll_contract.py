"""Source-level contract for the bounded PR_PATH_REQUESTED path re-poll.

Transport::has_path() is a FileStore index lookup (a LittleFS read in the
microStore-backed microReticulum fork). LXMRouter::process_sync() is called
every main loop iteration (~2s); before the fix, the PR_PATH_REQUESTED state
called has_path() on every pass — one path-table read per pass for the whole
60s sync window whenever the propagation node's path is missing. Observed on
a T-Deck: one destination fetched ~293x in 600s, each a read on the same
2MB partition as message storage.

The fix re-polls at most once per PATH_REQUEST_WAIT. These checks lock in:
  1. the re-poll gate precedes the has_path() call in process_sync,
  2. the gate is armed with PATH_REQUEST_WAIT (not a fresh constant),
  3. the one-shot has_path in request_messages_from_propagation_node is
     untouched (a sync request must still check immediately).
"""

from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
LXMROUTER_CPP = ROOT / ".pio/libdeps/tdeck/microLXMF/src/LXMF/LXMRouter.cpp"


def require_source() -> str:
    if not LXMROUTER_CPP.is_file():
        pytest.skip("pinned microLXMF libdeps not populated (run: pio run -e tdeck)")
    return LXMROUTER_CPP.read_text()


def process_sync_body(source: str) -> str:
    start = source.index("void LXMRouter::process_sync()")
    next_fn = source.index("void LXMRouter::", start + 10)
    return source[start:next_fn]


def test_process_sync_bounded_path_poll():
    source = require_source()
    body = process_sync_body(source)

    block_start = body.index("if (_sync_state == PR_PATH_REQUESTED)")
    block_end = body.index("Identity node_identity", block_start)
    block = body[block_start:block_end]

    assert "_next_sync_path_poll_time" in block, (
        "PR_PATH_REQUESTED must gate the has_path() re-poll behind "
        "_next_sync_path_poll_time"
    )
    gate = block.index("_next_sync_path_poll_time")
    has_path = block.index("Transport::has_path(")
    assert gate < has_path, "the re-poll gate must precede the has_path() read"
    assert "PATH_REQUEST_WAIT" in block, (
        "the re-poll window must be PATH_REQUEST_WAIT (matching the outbound "
        "path-request cadence), not a fresh constant"
    )


def test_request_path_check_untouched():
    source = require_source()
    start = source.index("void LXMRouter::request_messages_from_propagation_node()")
    end = source.index("void LXMRouter::process_sync()", start)
    body = source[start:end]
    assert "Transport::has_path(prop_node)" in body, (
        "the one-shot check in request_messages_from_propagation_node must "
        "remain: a sync request must check for a path immediately"
    )
    assert "_next_sync_path_poll_time" not in body, (
        "request_messages_from_propagation_node must not consume the poll window"
    )


def test_fork_pin_matches_audit_tool():
    # The backoff lives in the pinned fork; the pin in platformio.ini must
    # agree with tools/audit_release_build.py so a bump is atomic.
    ini = (ROOT / "platformio.ini").read_text()
    audit = (ROOT / "tools/audit_release_build.py").read_text()
    pins = set()
    for line in ini.splitlines():
        if "microLXMF.git#" in line:
            pins.add(line.split("microLXMF.git#")[1].strip())
    audit_pins = {
        line.split('"')[3]
        for line in audit.splitlines()
        if line.strip().startswith('"microLXMF"')
    }
    assert pins and pins == audit_pins, (
        f"platformio.ini microLXMF pin {pins} != audit tool pin {audit_pins}"
    )
