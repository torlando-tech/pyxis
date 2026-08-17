#!/usr/bin/env python3
import argparse
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import time

import RNS

RNS_VERSION = "1.4.2"
RNS_TREE_SHA256 = "b5398e7bae0cdd47212e0c6bff3f3a51b21012db0c23cb20d43b5103612f6c5e"
if getattr(RNS, "__version__", None) != RNS_VERSION:
    raise SystemExit(f"wrong RNS reference version: {getattr(RNS, '__version__', None)}")
if RNS.__file__ is None:
    raise SystemExit("RNS reference has no source path")
reference_root = Path(RNS.__file__).resolve().parent
reference_hash = hashlib.sha256()
for reference_file in sorted(reference_root.rglob("*.py")):
    relative = reference_file.relative_to(reference_root).as_posix().encode()
    content = reference_file.read_bytes()
    reference_hash.update(len(relative).to_bytes(4, "big"))
    reference_hash.update(relative)
    reference_hash.update(len(content).to_bytes(8, "big"))
    reference_hash.update(content)
if reference_hash.hexdigest() != RNS_TREE_SHA256:
    raise SystemExit(f"wrong RNS reference tree: {reference_hash.hexdigest()}")
print(f"REFERENCE RNS {RNS_VERSION}")
print(f"REFERENCE RNS tree {RNS_TREE_SHA256}")

state = {
    "request_seen": False,
    "request_count": 0,
    "anonymous": False,
    "link": None,
    "link_closed": False,
    "form_valid": False,
    "form_sequence": [],
}


def micron_page(title: str, target_bytes: int) -> bytes:
    prefix = f">{title}\n\nThis page traversed a real encrypted Reticulum Link.\n".encode()
    line = b"Deterministic x86 NomadNet integration payload.\n"
    result = bytearray(prefix)
    while len(result) + len(line) <= target_bytes:
        result.extend(line)
    if len(result) < target_bytes:
        result.extend(b"x" * (target_bytes - len(result)))
    return bytes(result)


PAGES = {
    "/page/immediate.mu": micron_page("Immediate page", 220),
    "/page/resource.mu": micron_page("Resource-backed page", 12_000),
    "/page/near-limit.mu": micron_page("Resource-backed page", 40_000),
    "/page/oversized.mu": micron_page("Resource-backed page", 70_000),
    "/page/cancel.mu": micron_page("Resource-backed page", 60_000),
    "/page/reuse-first.mu": micron_page("Immediate page", 220),
    "/page/reuse-second.mu": micron_page("Resource-backed page", 12_000),
    "/page/form.mu": micron_page("Form response", 220),
    "/page/partial.mu": b">Peer-refreshed fragment\n\nReal partial response.\n",
}

EXPECTED_FORM_DATA = {
    "var_fixed": "yes",
    "field_name": "Example User",
    "field_password": "example-pass",
    "field_color": "red,blue",
}


def page_handler(path, data, request_id, link_id, remote_identity, requested_at):
    state["request_seen"] = True
    state["request_count"] += 1
    state["anonymous"] = remote_identity is None
    if path == "/page/form.mu":
        state["form_valid"] = data == EXPECTED_FORM_DATA
        state["form_sequence"].append(data)
    print(f"SERVER request count={state['request_count']} path={path} bytes={len(PAGES[path])} anonymous={state['anonymous']}", flush=True)
    return PAGES[path]


def on_link_established(link):
    state["link"] = link
    link.set_link_closed_callback(on_link_closed)
    print("SERVER link established", flush=True)


def on_link_closed(link):
    state["link_closed"] = True
    print("SERVER link closed", flush=True)


def write_config(config_dir: str):
    config = """
[reticulum]
  enable_transport = No
  share_instance = No
  panic_on_interface_error = No

[logging]
  loglevel = 3

[interfaces]
  [[PyxisNomadNetX86]]
    type = UDPInterface
    interface_enabled = True
    listen_ip = 127.0.0.1
    listen_port = 14356
    forward_ip = 127.0.0.1
    forward_port = 14357
"""
    with open(os.path.join(config_dir, "config"), "w") as handle:
        handle.write(config)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("scenario", choices=("immediate", "resource", "near-limit", "oversized", "timeout", "cancel", "reuse", "form-anonymous", "form-identified", "owner-form-history", "partial"))
    parser.add_argument("--timeout", type=float, default=20.0)
    args = parser.parse_args()

    config_dir = tempfile.mkdtemp(prefix=f"pyxis-nomadnet-{args.scenario}-")
    os.makedirs(os.path.join(config_dir, "storage", "resources"), exist_ok=True)
    write_config(config_dir)
    RNS.Reticulum(config_dir)
    identity = RNS.Identity()
    destination = RNS.Destination(identity, RNS.Destination.IN,
                                  RNS.Destination.SINGLE, "nomadnetwork", "node")
    destination.set_proof_strategy(RNS.Destination.PROVE_ALL)
    destination.set_link_established_callback(on_link_established)
    if args.scenario == "reuse":
        for path in ("/page/reuse-first.mu", "/page/reuse-second.mu"):
            destination.register_request_handler(path, page_handler,
                                                 allow=RNS.Destination.ALLOW_ALL,
                                                 auto_compress=False)
    elif args.scenario != "timeout":
        path = ("/page/form.mu" if args.scenario.startswith("form-") or
                args.scenario == "owner-form-history" else f"/page/{args.scenario}.mu")
        destination.register_request_handler(path, page_handler,
                                             allow=RNS.Destination.ALLOW_ALL,
                                             auto_compress=False)
    destination.announce(app_data=b"Pyxis x86 NomadNet peer")
    print(f"SERVER announced {destination.hash.hex()}", flush=True)

    started = time.time()
    last_announce = started
    owner_response_deadline = None
    while time.time() - started < args.timeout:
        now = time.time()
        if now - last_announce >= 2.0 and not state["request_seen"]:
            destination.announce(app_data=b"Pyxis x86 NomadNet peer")
            last_announce = now
        if state["request_seen"] and not state["anonymous"] and args.scenario != "form-identified":
            print("SERVER FAIL client identified unexpectedly", flush=True)
            return 1
        if args.scenario == "form-anonymous" and state["request_seen"]:
            if state["anonymous"] and state["form_valid"]:
                print("SERVER PASS exact form request data anonymous=True", flush=True)
                return 0
            print("SERVER FAIL form data or anonymous identity mismatch", flush=True)
            return 1
        if args.scenario == "form-identified" and state["request_seen"]:
            if not state["anonymous"] and state["form_valid"]:
                print("SERVER PASS exact form request data anonymous=False", flush=True)
                return 0
            print("SERVER FAIL form data or identified identity mismatch", flush=True)
            return 1
        if args.scenario == "owner-form-history" and state["request_count"] == 3:
            expected_changed = dict(EXPECTED_FORM_DATA)
            expected_changed["field_name"] = "Changed User"
            if (state["anonymous"] and state["form_sequence"] ==
                    [EXPECTED_FORM_DATA, expected_changed, EXPECTED_FORM_DATA]):
                if owner_response_deadline is None:
                    owner_response_deadline = now + 2.0
                    print("SERVER owner response 3 queued; awaiting client receipt", flush=True)
                if state["link_closed"]:
                    print("SERVER PASS exact form request data owner-history=True delivery-settled=True", flush=True)
                    return 0
                if now >= owner_response_deadline:
                    print("SERVER FAIL owner response delivery did not settle", flush=True)
                    return 1
                time.sleep(0.02)
                continue
            print("SERVER FAIL owner form/history request sequence", flush=True)
            return 1
        if args.scenario in ("immediate", "resource", "near-limit", "oversized", "partial") and state["request_seen"]:
            time.sleep(1.0)
            print("SERVER PASS", flush=True)
            return 0
        if args.scenario == "reuse" and state["request_count"] == 2:
            time.sleep(1.0)
            print("SERVER PASS reused one Link for two anonymous requests", flush=True)
            return 0
        if args.scenario == "cancel" and state["request_seen"] and state["link_closed"]:
            print("SERVER PASS cancellation observed", flush=True)
            return 0
        if args.scenario == "timeout" and state["link"] is not None:
            # The missing handler is intentional; the client receipt must fail.
            time.sleep(3.0)
            print("SERVER PASS missing-handler timeout exercised", flush=True)
            return 0
        time.sleep(0.02)

    print(f"SERVER FAIL deadline request={state['request_seen']} closed={state['link_closed']}", flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
