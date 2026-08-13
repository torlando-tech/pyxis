#!/usr/bin/env python3
import argparse
import os
import sys
import tempfile
import time

import RNS

state = {
    "request_seen": False,
    "request_count": 0,
    "anonymous": False,
    "link": None,
    "link_closed": False,
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
}


def page_handler(path, data, request_id, link_id, remote_identity, requested_at):
    state["request_seen"] = True
    state["request_count"] += 1
    state["anonymous"] = remote_identity is None
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
    parser.add_argument("scenario", choices=("immediate", "resource", "near-limit", "oversized", "timeout", "cancel", "reuse"))
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
        path = f"/page/{args.scenario}.mu"
        destination.register_request_handler(path, page_handler,
                                             allow=RNS.Destination.ALLOW_ALL,
                                             auto_compress=False)
    destination.announce(app_data=b"Pyxis x86 NomadNet peer")
    print(f"SERVER announced {destination.hash.hex()}", flush=True)

    started = time.time()
    last_announce = started
    while time.time() - started < args.timeout:
        now = time.time()
        if now - last_announce >= 2.0 and not state["request_seen"]:
            destination.announce(app_data=b"Pyxis x86 NomadNet peer")
            last_announce = now
        if state["request_seen"] and not state["anonymous"]:
            print("SERVER FAIL client identified unexpectedly", flush=True)
            return 1
        if args.scenario in ("immediate", "resource", "near-limit", "oversized") and state["request_seen"]:
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
