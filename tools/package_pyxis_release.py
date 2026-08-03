#!/usr/bin/env python3
"""Build a deterministic update package accepted by Columba's Pyxis parser."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import zipfile


MAX_ARCHIVE_BYTES = 4 * 1024 * 1024
MAX_FIRMWARE_BYTES = 0x300000
BOOT_APP0_BYTES = 0x2000
ESP32_S3_CHIP_ID = 9
DETERMINISTIC_TIMESTAMP = (1980, 1, 1, 0, 0, 0)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def validate_images(firmware: bytes, boot_app0: bytes) -> None:
    if not 1 <= len(firmware) <= MAX_FIRMWARE_BYTES:
        raise ValueError("firmware.bin must fit the 0x300000-byte application partition")
    if len(firmware) < 24 or firmware[0] != 0xE9:
        raise ValueError("firmware.bin is not an ESP application image")
    chip_id = int.from_bytes(firmware[12:14], "little")
    if chip_id != ESP32_S3_CHIP_ID:
        raise ValueError(f"firmware.bin targets chip ID {chip_id}, expected ESP32-S3 chip ID 9")
    if len(boot_app0) != BOOT_APP0_BYTES:
        raise ValueError("boot_app0.bin must be exactly 0x2000 bytes")
    if boot_app0[:4] != b"\x01\x00\x00\x00":
        raise ValueError("boot_app0.bin does not contain the expected OTA selector")


def zip_info(name: str) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, DETERMINISTIC_TIMESTAMP)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 3
    info.external_attr = 0o100644 << 16
    return info


def build_package(firmware_path: Path, boot_app0_path: Path, version: str, output: Path) -> None:
    if not version.strip():
        raise ValueError("version must not be blank")
    if not (output.name.endswith(".pyxis") or output.name.endswith(".pyxis.zip")):
        raise ValueError("output filename must end in .pyxis or .pyxis.zip")

    firmware = firmware_path.read_bytes()
    boot_app0 = boot_app0_path.read_bytes()
    validate_images(firmware, boot_app0)

    manifest = {
        "schemaVersion": 1,
        "product": "pyxis",
        "board": "t-deck-plus",
        "chip": "esp32-s3",
        "version": version,
        "firmware": {
            "name": "firmware.bin",
            "offset": 0x10000,
            "size": len(firmware),
            "sha256": sha256(firmware),
        },
        "bootApp0": {
            "name": "boot_app0.bin",
            "offset": 0xE000,
            "size": len(boot_app0),
            "sha256": sha256(boot_app0),
        },
    }
    manifest_bytes = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode() + b"\n"

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output.parent, prefix=f".{output.name}.", delete=False) as handle:
        temporary = Path(handle.name)
    try:
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name, data in (
                ("manifest.json", manifest_bytes),
                ("firmware.bin", firmware),
                ("boot_app0.bin", boot_app0),
            ):
                archive.writestr(zip_info(name), data, compresslevel=9)
        if temporary.stat().st_size > MAX_ARCHIVE_BYTES:
            raise ValueError("Pyxis package exceeds Columba's 4 MiB archive limit")
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)

    print(f"package={output}")
    print(f"package_size={output.stat().st_size}")
    print(f"package_sha256={sha256(output.read_bytes())}")
    print(f"firmware_size={len(firmware)}")
    print(f"firmware_sha256={sha256(firmware)}")
    print(f"boot_app0_size={len(boot_app0)}")
    print(f"boot_app0_sha256={sha256(boot_app0)}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--boot-app0", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    build_package(args.firmware, args.boot_app0, args.version, args.output)


if __name__ == "__main__":
    main()
