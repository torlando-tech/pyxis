import hashlib
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = ROOT / "tools/validate_pyxis_web_release.py"
OFFSETS = {
    "bootloader.bin": 0,
    "partitions.bin": 0x8000,
    "boot_app0.bin": 0xE000,
    "firmware.bin": 0x10000,
}


def make_release(directory: Path) -> None:
    directory.mkdir()
    images = {
        "bootloader.bin": b"bootloader",
        "partitions.bin": b"partitions",
        "boot_app0.bin": b"\x01\x00\x00\x00" + b"\xff" * (0x2000 - 4),
        "firmware.bin": b"\xe9" + b"\x00" * 11 + b"\x09\x00" + b"\x00" * 114,
    }
    descriptors = {}
    for name, data in images.items():
        (directory / name).write_bytes(data)
        descriptors[name] = {
            "file": name,
            "offset": OFFSETS[name],
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        }
    metadata = {
        "schema": 1,
        "version": "v0.3.0",
        "source_commit": "a" * 40,
        "environment": "tdeck-release",
        "chip_family": "ESP32-S3",
        "flash_size": 8 * 1024 * 1024,
        "persistence_safe": True,
        "images": descriptors,
    }
    (directory / "pyxis-release.json").write_text(json.dumps(metadata))


def test_validator_accepts_complete_matching_release(tmp_path):
    release = tmp_path / "release"
    make_release(release)

    subprocess.run(
        [sys.executable, str(VALIDATOR), "--directory", str(release), "--version", "v0.3.0"],
        check=True,
    )


def test_validator_rejects_digest_mismatch(tmp_path):
    release = tmp_path / "release"
    make_release(release)
    (release / "firmware.bin").write_bytes(b"corrupt")

    result = subprocess.run(
        [sys.executable, str(VALIDATOR), "--directory", str(release), "--version", "v0.3.0"],
    )
    assert result.returncode != 0


def test_validator_rejects_image_that_overlaps_next_persistent_region(tmp_path):
    release = tmp_path / "release"
    make_release(release)
    oversized = b"\xff" * 0x1001
    (release / "partitions.bin").write_bytes(oversized)
    metadata_path = release / "pyxis-release.json"
    metadata = json.loads(metadata_path.read_text())
    metadata["images"]["partitions.bin"]["size"] = len(oversized)
    metadata["images"]["partitions.bin"]["sha256"] = hashlib.sha256(oversized).hexdigest()
    metadata_path.write_text(json.dumps(metadata))

    result = subprocess.run(
        [sys.executable, str(VALIDATOR), "--directory", str(release), "--version", "v0.3.0"],
    )
    assert result.returncode != 0