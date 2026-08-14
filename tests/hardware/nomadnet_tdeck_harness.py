#!/usr/bin/env python3
"""Automated physical T-Deck NomadNet Link/page diagnostic over serial."""

import argparse
import queue
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

import serial

FAULTS = (
    "Guru Meditation",
    "assert failed",
    "abort() was called",
    "Task watchdog got triggered",
    "watchdog timeout",
    "Backtrace:",
)
TERMINAL_STATES = {"IDLE"}


class SerialCapture:
    def __init__(self, port: str, log_path: Path):
        self.serial = serial.Serial(port, 115200, timeout=0.1)
        self.log = log_path.open("wb")
        self.lines = queue.Queue()
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        buffered = b""
        while not self.stop.is_set():
            data = self.serial.read(4096)
            if not data:
                continue
            self.log.write(data)
            self.log.flush()
            buffered += data
            while b"\n" in buffered:
                raw, buffered = buffered.split(b"\n", 1)
                self.lines.put(raw.rstrip(b"\r").decode("utf-8", errors="replace"))

    def reset(self):
        self.serial.dtr = False
        self.serial.rts = True
        time.sleep(0.1)
        self.serial.dtr = True
        self.serial.rts = False
        time.sleep(0.1)
        self.serial.dtr = False
        self.serial.rts = False

    def command(self, text: str, timeout: float = 8.0, prefixes=("T:OK", "T:ERR")):
        self.serial.write((text + "\n").encode())
        self.serial.flush()
        deadline = time.time() + timeout
        observed = []
        while time.time() < deadline:
            try:
                line = self.lines.get(timeout=min(0.5, deadline - time.time()))
            except queue.Empty:
                continue
            observed.append(line)
            if line.startswith(prefixes):
                return line, observed
            for prefix in prefixes:
                marker = line.find(prefix)
                if marker >= 0:
                    return line[marker:], observed
        return None, observed

    def wait_for(self, predicate, timeout: float):
        deadline = time.time() + timeout
        observed = []
        while time.time() < deadline:
            try:
                line = self.lines.get(timeout=min(0.5, deadline - time.time()))
            except queue.Empty:
                continue
            observed.append(line)
            if predicate(line):
                return line, observed
        return None, observed

    def close(self):
        self.stop.set()
        self.thread.join(timeout=1)
        self.serial.close()
        self.log.close()


def parse_fields(line: str):
    return dict(re.findall(r"([a-z]+)=([^ ]+)", line or ""))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", required=True)
    parser.add_argument("--url", required=True)
    parser.add_argument("--log", default="/tmp/pyxis-nomadnet-tdeck.log")
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--mode", choices=("NORMAL", "SUSPEND"), default="NORMAL")
    args = parser.parse_args()

    capture = SerialCapture(args.serial, Path(args.log))
    all_lines = []
    try:
        capture.reset()
        boot, lines = capture.wait_for(
            lambda line: "BOOT" in line and "ui_manager" in line and "END" in line,
            120,
        )
        all_lines.extend(lines)
        if not boot:
            print("HARNESS FAIL boot-timeout")
            return 1

        vector, lines = capture.command(
            "T:CRYPTO_VECTOR", timeout=20, prefixes=("T:CRYPTO_VECTOR",)
        )
        all_lines.extend(lines)
        print(vector or "T:CRYPTO_VECTOR missing", flush=True)
        if not vector or not vector.startswith("T:CRYPTO_VECTOR PASS"):
            print("HARNESS FAIL crypto-vector")
            return 1

        mode, lines = capture.command("T:NOMAD_MODE " + args.mode)
        all_lines.extend(lines)
        if mode != f"T:OK mode={args.mode}":
            print(f"HARNESS FAIL mode-response={mode}")
            return 1

        def is_tcp_connected(line):
            return (
                "TCP interface reconnected" in line
                or ("TCPClientInterface" in line and "connected to" in line.lower())
            )

        tcp = next((line for line in all_lines if is_tcp_connected(line)), None)
        if not tcp:
            tcp, lines = capture.wait_for(is_tcp_connected, 60)
            all_lines.extend(lines)
        if not tcp:
            print("HARNESS FAIL tcp-timeout")
            return 1

        response, lines = capture.command("T:NOMAD " + args.url)
        all_lines.extend(lines)
        if response != "T:OK queued":
            print(f"HARNESS FAIL open-response={response}")
            return 1

        deadline = time.time() + args.timeout
        statuses = []
        while time.time() < deadline:
            response, lines = capture.command("T:NOMAD_STATUS", timeout=5)
            all_lines.extend(lines)
            if response:
                statuses.append(response)
                print(response, flush=True)
                fields = parse_fields(response)
                if fields.get("response", "0") != "0" and fields.get("state") in TERMINAL_STATES:
                    print("HARNESS PASS page-loaded")
                    return 0
            if any(any(marker in line for marker in FAULTS) for line in all_lines):
                print("HARNESS FAIL fault-signature")
                return 1
            time.sleep(1)

        last = statuses[-1] if statuses else "none"
        print(f"HARNESS FAIL operation-timeout last={last}")
        return 1
    finally:
        capture.close()


if __name__ == "__main__":
    sys.exit(main())
