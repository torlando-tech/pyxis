#!/usr/bin/env python3
"""Validate a same-origin Pyxis web-flasher release directory."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re


EXPECTED_OFFSETS = {
    "bootloader.bin": 0,
    "partitions.bin": 0x8000,
    "boot_app0.bin": 0xE000,
    "firmware.bin": 0x10000,
}
MAX_IMAGE_SIZES = {
    "bootloader.bin": 0x8000,
    "partitions.bin": 0x1000,
    "boot_app0.bin": 0x2000,
    "firmware.bin": 0x300000,
}
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def validate_release(directory: Path, expected_version: str) -> None:
    metadata = json.loads((directory / "pyxis-release.json").read_text())
    require(metadata.get("schema") == 1, "unsupported metadata schema")
    require(metadata.get("version") == expected_version, "release version mismatch")
    require(bool(COMMIT_PATTERN.fullmatch(metadata.get("source_commit", ""))), "invalid source commit")
    require(metadata.get("environment") == "tdeck-release", "release environment mismatch")
    require(metadata.get("chip_family") == "ESP32-S3", "chip family mismatch")
    require(metadata.get("flash_size") == 8 * 1024 * 1024, "flash size mismatch")
    require(metadata.get("persistence_safe") is True, "release is not persistence-safe")

    descriptors = metadata.get("images")
    require(isinstance(descriptors, dict), "missing image descriptors")
    require(set(descriptors) == set(EXPECTED_OFFSETS), "image descriptor set mismatch")

    for name, offset in EXPECTED_OFFSETS.items():
        descriptor = descriptors[name]
        require(isinstance(descriptor, dict), f"invalid descriptor for {name}")
        require(descriptor.get("file") == name, f"filename mismatch for {name}")
        require(descriptor.get("offset") == offset, f"offset mismatch for {name}")
        image = (directory / name).read_bytes()
        require(descriptor.get("size") == len(image) and len(image) > 0, f"size mismatch for {name}")
        require(len(image) <= MAX_IMAGE_SIZES[name], f"{name} exceeds its flash region")
        declared_hash = descriptor.get("sha256", "")
        require(bool(SHA256_PATTERN.fullmatch(declared_hash)), f"invalid SHA-256 for {name}")
        require(hashlib.sha256(image).hexdigest() == declared_hash, f"SHA-256 mismatch for {name}")

    firmware = (directory / "firmware.bin").read_bytes()
    require(len(firmware) <= 0x300000, "firmware exceeds app0 partition")
    require(len(firmware) >= 24 and firmware[0] == 0xE9, "invalid ESP firmware image")
    require(int.from_bytes(firmware[12:14], "little") == 9, "firmware is not ESP32-S3")
    boot_app0 = (directory / "boot_app0.bin").read_bytes()
    require(len(boot_app0) == 0x2000, "boot_app0 size mismatch")
    require(boot_app0[:4] == b"\x01\x00\x00\x00", "invalid boot_app0 selector")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    validate_release(args.directory, args.version)
    print(f"validated_web_release={args.version}")


if __name__ == "__main__":
    main()
