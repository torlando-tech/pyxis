#!/usr/bin/env python3
"""Build a single merged flash image from a validated Pyxis web release.

The merged image places the four release assets at their fixed flash offsets
inside an erased (0xFF) 8 MiB canvas, so the whole T-Deck Plus flash can be
provisioned with one write:

    esptool.py --chip esp32s3 erase_flash write_flash 0x0 pyxis-<tag>-merged.bin

This is a first-install/provisioning image: flashing it overwrites every
region including NVS, OTA data, and the LittleFS ("spiffs") partition.
Updating an existing device must keep using firmware.bin only (or the
Columba .pyxis package), which preserves persistent data.

Inputs must already pass tools/validate_pyxis_web_release.py; the validator
runs again here so a merged image can never be built from unvalidated or
stale assets.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from validate_pyxis_web_release import EXPECTED_OFFSETS, validate_release


FLASH_SIZE = 8 * 1024 * 1024
ERASED = 0xFF


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def build_merged_image(directory: Path) -> bytes:
    spans = {}
    for name, offset in EXPECTED_OFFSETS.items():
        image = (directory / name).read_bytes()
        spans[name] = (offset, offset + len(image))
    for name, (start, end) in spans.items():
        if end > FLASH_SIZE:
            raise ValueError(f"{name} exceeds the 8 MiB flash at offset 0x{start:x}")
        for other, (other_start, other_end) in spans.items():
            if other != name and start < other_end and other_start < end:
                raise ValueError(f"{name} overlaps {other}")

    canvas = bytearray([ERASED]) * FLASH_SIZE
    for name, (start, end) in spans.items():
        canvas[start:end] = (directory / name).read_bytes()
    return bytes(canvas)


def verify_merged_image(merged: bytes, directory: Path) -> None:
    if len(merged) != FLASH_SIZE:
        raise ValueError(f"merged image is {len(merged)} bytes, expected {FLASH_SIZE}")
    for name, offset in EXPECTED_OFFSETS.items():
        image = (directory / name).read_bytes()
        if merged[offset : offset + len(image)] != image:
            raise ValueError(f"read-back mismatch for {name} at 0x{offset:x}")

    # Regions outside the four images must remain erased flash.
    occupied = sorted(
        (offset, offset + len((directory / name).read_bytes()))
        for name, offset in EXPECTED_OFFSETS.items()
    )
    cursor = 0
    for start, end in occupied:
        if any(byte != ERASED for byte in merged[cursor:start]):
            raise ValueError(f"unexpected non-erased bytes in gap before 0x{start:x}")
        cursor = end
    if any(byte != ERASED for byte in merged[cursor:]):
        raise ValueError("unexpected non-erased bytes after the last image")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    validate_release(args.directory, args.version)

    merged = build_merged_image(args.directory)
    verify_merged_image(merged, args.directory)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(merged)

    print(f"merged_image={args.output}")
    print(f"merged_size={len(merged)}")
    print(f"merged_sha256={sha256(merged)}")
    for name, offset in EXPECTED_OFFSETS.items():
        image = (args.directory / name).read_bytes()
        print(f"{name} offset=0x{offset:x} size={len(image)} sha256={sha256(image)}")


if __name__ == "__main__":
    main()
