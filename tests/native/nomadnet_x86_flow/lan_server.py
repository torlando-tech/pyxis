#!/usr/bin/env python3
import argparse
import os
import sys
import tempfile
import time

import RNS

PAGE = b""">Pyxis LAN NomadNet Test

This default page crossed a real TCP Reticulum interface, established an encrypted Link, and was served by an independent Python RNS peer.

`[Open this page again`:/page/index.mu]
"""

state = {
    "request_seen": False,
    "anonymous": False,
    "link_established": False,
    "link_closed": False,
}


def packet_kind(raw):
    if len(raw) < 19:
        return None
    if raw[0] & 0x03 == RNS.Packet.LINKREQUEST:
        return "LINKREQUEST"
    context_offset = 34 if raw[0] & 0x40 else 18
    if len(raw) > context_offset and raw[context_offset] == RNS.Packet.LRPROOF:
        return "LRPROOF"
    return None


def install_wire_capture():
    original_inbound = RNS.Transport.inbound
    original_transmit = RNS.Transport.transmit

    def captured_inbound(raw, interface=None):
        kind = packet_kind(raw)
        if kind == "LINKREQUEST":
            print(f"LAN WIRE RX kind={kind} raw={len(raw)} hex={raw.hex()}", flush=True)
        return original_inbound(raw, interface)

    def captured_transmit(interface, raw):
        kind = packet_kind(raw)
        if kind == "LRPROOF":
            print(f"LAN WIRE TX kind={kind} raw={len(raw)} hex={raw.hex()}", flush=True)
        return original_transmit(interface, raw)

    RNS.Transport.inbound = staticmethod(captured_inbound)
    RNS.Transport.transmit = staticmethod(captured_transmit)


def page_handler(path, data, request_id, link_id, remote_identity, requested_at):
    state["request_seen"] = True
    state["anonymous"] = remote_identity is None
    print(
        f"LAN SERVER request path={path} bytes={len(PAGE)} anonymous={state['anonymous']}",
        flush=True,
    )
    return PAGE


def on_link_established(link):
    state["link_established"] = True
    link.set_link_closed_callback(on_link_closed)
    print("LAN SERVER link established", flush=True)


def on_link_closed(link):
    state["link_closed"] = True
    print("LAN SERVER link closed", flush=True)


def write_config(config_dir: str, listen_ip: str, listen_port: int):
    config = f"""
[reticulum]
  enable_transport = No
  share_instance = No
  panic_on_interface_error = No

[logging]
  loglevel = 5

[interfaces]
  [[Pyxis LAN TCP Server]]
    type = TCPServerInterface
    enabled = Yes
    listen_ip = {listen_ip}
    listen_port = {listen_port}
    discoverable = No
"""
    with open(os.path.join(config_dir, "config"), "w") as handle:
        handle.write(config)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen-ip", default="0.0.0.0")
    parser.add_argument("--listen-port", type=int, default=42420)
    parser.add_argument("--timeout", type=float, default=90.0)
    args = parser.parse_args()

    config_dir = tempfile.mkdtemp(prefix="pyxis-nomadnet-lan-server-")
    write_config(config_dir, args.listen_ip, args.listen_port)
    RNS.Reticulum(config_dir)
    install_wire_capture()
    identity = RNS.Identity()
    destination = RNS.Destination(
        identity,
        RNS.Destination.IN,
        RNS.Destination.SINGLE,
        "nomadnetwork",
        "node",
    )
    destination.set_proof_strategy(RNS.Destination.PROVE_ALL)
    destination.set_link_established_callback(on_link_established)
    destination.register_request_handler(
        "/page/index.mu",
        page_handler,
        allow=RNS.Destination.ALLOW_ALL,
        auto_compress=False,
    )
    destination.announce(app_data=b"Pyxis LAN NomadNet Test")
    print(
        f"LAN SERVER READY destination={destination.hash.hex()} port={args.listen_port}",
        flush=True,
    )

    started = time.time()
    last_announce = started
    while time.time() - started < args.timeout:
        now = time.time()
        if now - last_announce >= 2.0 and not state["request_seen"]:
            destination.announce(app_data=b"Pyxis LAN NomadNet Test")
            last_announce = now
        if state["request_seen"]:
            time.sleep(1.0)
            passed = state["link_established"] and state["anonymous"]
            print(
                "LAN SERVER RESULT "
                f"link={int(state['link_established'])} request=1 "
                f"anonymous={int(state['anonymous'])} closed={int(state['link_closed'])} "
                f"passed={int(passed)}",
                flush=True,
            )
            return 0 if passed else 1
        time.sleep(0.02)

    print(
        "LAN SERVER FAIL deadline "
        f"link={int(state['link_established'])} request={int(state['request_seen'])}",
        flush=True,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
