#!/usr/bin/env python3
"""pyxis screenshot utility.

Sends `T:SCREENSHOT` over USB-CDC to a running pyxis device, reads the
base64-encoded RGB565 frame back, decodes, and writes a PNG file.

Usage:
    python3 screenshot.py [--port /dev/cu.usbmodem...] [--out screenshot.png]

If --port is omitted the script tries the common macOS/Linux candidates
in order and uses the first one that responds to T:ID within 2 seconds.

Requires Pillow (`pip install pillow pyserial`). Both are typically
already on a host that's been doing pyxis development.

Wire format (see main.cpp T:SCREENSHOT handler):
    T:SCREENSHOT BEGIN W=<w> H=<h> FMT=rgb565<be|le> BYTES=<n>
    <base64 line>
    ...
    T:SCREENSHOT END
"""

import argparse
import base64
import glob
import struct
import sys
import time

import serial  # type: ignore
from PIL import Image  # type: ignore


BAUD = 115200
HANDSHAKE_TIMEOUT_S = 2.0
SCREENSHOT_TIMEOUT_S = 15.0


def _candidate_ports():
    # macOS first, then Linux. Filter out the obvious non-pyxis ones.
    pats = ("/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*")
    out = []
    for p in pats:
        out += glob.glob(p)
    return [p for p in out if "Bluetooth" not in p and "debug-console" not in p]


def _is_pyxis(port):
    """Probe a port with T:ID — pyxis replies `T:OK <hex>`."""
    try:
        with serial.Serial(port, BAUD, timeout=0.2) as s:
            s.reset_input_buffer()
            s.write(b"T:ID\n")
            deadline = time.time() + HANDSHAKE_TIMEOUT_S
            while time.time() < deadline:
                line = s.readline().decode(errors="replace").strip()
                if line.startswith("T:OK ") and len(line.split()[1]) >= 16:
                    return True
                if line.startswith("T:ERR"):
                    return True
    except (OSError, serial.SerialException):
        return False
    return False


def _resolve_port(explicit):
    if explicit:
        return explicit
    for cand in _candidate_ports():
        if _is_pyxis(cand):
            return cand
    raise SystemExit(
        "no pyxis serial port found. Pass --port explicitly. Candidates "
        f"tried: {_candidate_ports()}"
    )


def _capture(port):
    """Send T:SCREENSHOT, return (width, height, fmt, raw_bytes)."""
    with serial.Serial(port, BAUD, timeout=0.5) as s:
        s.reset_input_buffer()
        s.write(b"T:SCREENSHOT\n")

        # Skip lines until BEGIN marker. The pyxis log channel is
        # noisy with [HEAP], [PSRAM], BLE heartbeats, etc — they all
        # need to be discarded before the screenshot stream starts.
        deadline = time.time() + SCREENSHOT_TIMEOUT_S
        header = None
        while time.time() < deadline:
            line = s.readline().decode(errors="replace").strip()
            if line.startswith("T:SCREENSHOT BEGIN"):
                header = line
                break
            if line.startswith("T:ERR"):
                raise RuntimeError(f"device returned: {line}")
        if header is None:
            raise RuntimeError("timed out waiting for T:SCREENSHOT BEGIN")

        # Parse header fields: `K=V` tokens after BEGIN.
        meta = {}
        for tok in header.split()[2:]:
            if "=" in tok:
                k, v = tok.split("=", 1)
                meta[k] = v
        try:
            width = int(meta["W"])
            height = int(meta["H"])
            fmt = meta["FMT"]
            expected_bytes = int(meta["BYTES"])
        except KeyError as e:
            raise RuntimeError(f"missing field in header: {header} ({e})")

        # Read base64 lines until END marker. Don't strip the lines —
        # base64 alphabet excludes whitespace anyway, and trailing
        # padding chars matter for the last line.
        b64_chunks = []
        deadline = time.time() + SCREENSHOT_TIMEOUT_S
        while time.time() < deadline:
            line = s.readline().decode(errors="replace").rstrip("\r\n")
            if line == "T:SCREENSHOT END":
                break
            # Filter out interleaved log lines (anything that's not
            # base64 alphabet). The CDC channel can have a heap log
            # line spliced between two screenshot lines if a hook
            # fires mid-dump.
            if line and all(c in _B64_ALPHABET for c in line):
                b64_chunks.append(line)
        else:
            raise RuntimeError("timed out waiting for T:SCREENSHOT END")

        raw = base64.b64decode("".join(b64_chunks))
        if len(raw) != expected_bytes:
            raise RuntimeError(
                f"size mismatch: header says {expected_bytes}B, decoded "
                f"{len(raw)}B. Likely lost lines on CDC."
            )
        return width, height, fmt, raw


_B64_ALPHABET = set(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/="
)


def _rgb565_to_image(width, height, fmt, raw):
    """Decode an RGB565 buffer into a PIL Image (RGB mode)."""
    if len(raw) != width * height * 2:
        raise RuntimeError(
            f"buffer size {len(raw)} != {width}*{height}*2 = "
            f"{width*height*2}"
        )
    # Unpack as 16-bit ints in the right byte order.
    if fmt == "rgb565be":
        words = struct.unpack(f">{width*height}H", raw)
    elif fmt == "rgb565le":
        words = struct.unpack(f"<{width*height}H", raw)
    else:
        raise RuntimeError(f"unknown FMT={fmt!r}")
    # RGB565: rrrrrggggggbbbbb. Expand to 8-bit channels by replicating
    # the high bits into the low bits (matches what the panel actually
    # displays after the truncation, vs naive `<< 3` which biases dark).
    pixels = bytearray(width * height * 3)
    for i, w in enumerate(words):
        r = (w >> 11) & 0x1f
        g = (w >> 5) & 0x3f
        b = w & 0x1f
        pixels[3 * i + 0] = (r << 3) | (r >> 2)
        pixels[3 * i + 1] = (g << 2) | (g >> 4)
        pixels[3 * i + 2] = (b << 3) | (b >> 2)
    return Image.frombytes("RGB", (width, height), bytes(pixels))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="serial port (default: auto-detect)")
    ap.add_argument(
        "--out",
        default="screenshot.png",
        help="output path (default: screenshot.png)",
    )
    args = ap.parse_args()

    port = _resolve_port(args.port)
    print(f"capturing from {port}…", file=sys.stderr)
    width, height, fmt, raw = _capture(port)
    img = _rgb565_to_image(width, height, fmt, raw)
    img.save(args.out)
    print(f"wrote {args.out} ({width}x{height} {fmt})", file=sys.stderr)


if __name__ == "__main__":
    main()
