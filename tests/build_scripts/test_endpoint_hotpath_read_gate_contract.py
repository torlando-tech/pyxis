"""Source-level contract for the endpoint-node hot-path read gate.

In the CBA microStore fork, Transport::inbound() and Transport::path_request()
each perform a full _new_path_table.get() (flush_buffer flash write + index
scan + segment open) for every non-announce inbound packet / path request,
BEFORE consulting whether the node can act on the result. On an endpoint-only
node (transport disabled, no attached local clients) that result is never
used, so each air packet costs a multi-second flash operation on the 2MB
partition that also holds message storage. Measured on a T-Deck idle: 792
such reads in 600s, all from these two sites, driving a 840s display stall.

The fix gates each read on the exact condition where its destination_entry is
actually consumed:
  - inbound():   for_local_client is only actionable when relaying, so gate on
                 (transport_enabled || any local client attached).
  - path_request(): destination_entry is only read in the
                 (transport_enabled || is_from_local_client) answer branch; the
                 local-destination answer for THIS node uses the in-memory
                 _destinations table and must remain reachable.

These checks lock the gates in place and prove the local-destination answer
path is preserved (an endpoint must still be discoverable).
"""

from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
TRANSPORT_CPP = ROOT / ".pio/libdeps/tdeck/microReticulum/src/microReticulum/Transport.cpp"


def require_source() -> str:
    if not TRANSPORT_CPP.is_file():
        pytest.skip("pinned microReticulum libdeps not populated (run: pio run -e tdeck)")
    return TRANSPORT_CPP.read_text()


def function_body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    nxt = source.index(next_signature, start + len(signature))
    return source[start:nxt]


def test_inbound_for_local_client_read_gated():
    source = require_source()
    body = function_body(
        source,
        "void Transport::inbound(const Bytes& raw",
        "void Transport::synthesize_tunnel(",
    )
    # The for_local_client determination is the hot read we gate.
    marker = "If packet is anything besides ANNOUNCE then determine"
    assert marker in body, "inbound() for_local_client block not found"
    block = body[body.index(marker):]

    # The get() must now be guarded by the endpoint condition.
    gate = block.index("Reticulum::transport_enabled() || _local_client_interfaces.size() > 0")
    get = block.index("_new_path_table.get(packet.destination_hash()", gate)
    assert gate < get, (
        "inbound() _new_path_table.get() for for_local_client must be gated "
        "behind (transport_enabled || local clients attached); an endpoint-only "
        "node must not pay a flash read for a value it never uses"
    )
    # The hops==0 for_local_client assignment must still follow the (skippable) read.
    assert "for_local_client = true" in block[get:], (
        "inbound() must still set for_local_client on a hops==0 hit"
    )


def test_path_request_read_gated_but_local_answer_preserved():
    source = require_source()
    body = function_body(
        source,
        "void Transport::path_request(const Bytes& destination_hash",
        "bool Transport::from_local_client(",
    )
    # destination_entry is only consumed in the answer branch; the read must be
    # gated on the same condition.
    gate = body.index("Reticulum::transport_enabled() || is_from_local_client")
    get = body.index("_new_path_table.get(destination_hash", gate)
    assert gate < get, (
        "path_request() _new_path_table.get() must be gated behind "
        "(transport_enabled || is_from_local_client); an endpoint-only node "
        "must not pay a flash read to answer a path request"
    )
    # The local-destination answer (in-memory _destinations) must remain and be
    # reachable AFTER the (skippable) read so an endpoint stays discoverable.
    local_lookup = body.index("_destinations.find(destination_hash)")
    assert local_lookup > get, (
        "path_request() must still consult the in-memory _destinations table "
        "to answer path requests for THIS node (local-destination answer)"
    )


def test_fork_pin_matches_audit_tool():
    # The gate lives in the pinned microReticulum fork; the pin in
    # platformio.ini must agree with tools/audit_release_build.py so a bump is
    # atomic (all 5 sites moved together).
    ini = (ROOT / "platformio.ini").read_text()
    audit = (ROOT / "tools/audit_release_build.py").read_text()
    ini_pins = {
        line.split("microReticulum.git#")[1].strip()
        for line in ini.splitlines()
        if "microReticulum.git#" in line
    }
    audit_pins = {
        line.split('"')[3]
        for line in audit.splitlines()
        if line.strip().startswith('"microReticulum"')
    }
    assert ini_pins and ini_pins == audit_pins, (
        f"platformio.ini microReticulum pin {ini_pins} != audit tool pin {audit_pins}"
    )
