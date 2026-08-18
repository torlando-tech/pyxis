"""Contract tests for the merged full-flash Pyxis release image."""

import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/merge_pyxis_release.py"
WORKFLOW = ROOT / ".github/workflows/release-firmware.yml"
README = ROOT / "README.md"

FLASH_SIZE = 8 * 1024 * 1024
OFFSETS = {
    "bootloader.bin": 0x0,
    "partitions.bin": 0x8000,
    "boot_app0.bin": 0xE000,
    "firmware.bin": 0x10000,
}


def valid_bootloader() -> bytes:
    return b"\xE9\x03" + b"\xA5" * 512


def valid_partitions() -> bytes:
    return b"\xAA\x50" + b"\x00" * 511


def valid_boot_app0() -> bytes:
    image = bytearray([0xFF] * 0x2000)
    image[:4] = b"\x01\x00\x00\x00"
    return bytes(image)


def valid_firmware() -> bytes:
    image = bytearray(0x1000)
    image[0] = 0xE9
    image[12:14] = (9).to_bytes(2, "little")
    return bytes(image)


def images() -> dict[str, bytes]:
    return {
        "bootloader.bin": valid_bootloader(),
        "partitions.bin": valid_partitions(),
        "boot_app0.bin": valid_boot_app0(),
        "firmware.bin": valid_firmware(),
    }


def write_release_directory(directory: Path, version: str = "v9.9.9", images_: dict | None = None) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    images_ = images_ or images()
    for name, data in images_.items():
        (directory / name).write_bytes(data)
    metadata = {
        "schema": 1,
        "version": version,
        "source_commit": "a" * 40,
        "environment": "tdeck-release",
        "chip_family": "ESP32-S3",
        "flash_size": FLASH_SIZE,
        "persistence_safe": True,
        "images": {
            name: {
                "file": name,
                "offset": OFFSETS[name],
                "size": len(images_[name]),
                "sha256": hashlib.sha256(images_[name]).hexdigest(),
            }
            for name in OFFSETS
        },
    }
    (directory / "pyxis-release.json").write_text(json.dumps(metadata))
    return directory


def run_builder(directory: Path, output: Path, version: str = "v9.9.9") -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(SCRIPT), "--directory", str(directory), "--version", version, "--output", str(output)],
        capture_output=True,
        text=True,
    )


def test_merged_image_places_assets_at_fixed_offsets_and_pads_erased(tmp_path):
    directory = write_release_directory(tmp_path / "release")
    output = tmp_path / "merged.bin"
    result = run_builder(directory, output)
    assert result.returncode == 0, result.stderr

    merged = output.read_bytes()
    assert len(merged) == FLASH_SIZE
    images_ = images()
    for name, offset in OFFSETS.items():
        assert merged[offset : offset + len(images_[name])] == images_[name]
    # Every byte outside the four images must be erased flash.
    occupied = sorted(
        (offset, offset + len(images_[name])) for name, offset in OFFSETS.items()
    )
    for index in range(len(merged)):
        in_image = any(start <= index < end for start, end in occupied)
        if not in_image:
            assert merged[index] == 0xFF


def test_merged_image_is_byte_for_byte_deterministic(tmp_path):
    directory = write_release_directory(tmp_path / "release")
    first = tmp_path / "first.bin"
    second = tmp_path / "second.bin"
    assert run_builder(directory, first).returncode == 0
    assert run_builder(directory, second).returncode == 0
    assert first.read_bytes() == second.read_bytes()


def test_merged_image_reports_sha256_matching_file(tmp_path):
    directory = write_release_directory(tmp_path / "release")
    output = tmp_path / "merged.bin"
    result = run_builder(directory, output)
    assert result.returncode == 0, result.stderr
    assert f"merged_sha256={hashlib.sha256(output.read_bytes()).hexdigest()}" in result.stdout


def test_builder_rejects_version_mismatch(tmp_path):
    directory = write_release_directory(tmp_path / "release")
    result = run_builder(directory, tmp_path / "merged.bin", version="v9.9.10")
    assert result.returncode != 0
    assert not (tmp_path / "merged.bin").exists()


def test_builder_rejects_tampered_image(tmp_path):
    images_ = images()
    images_["firmware.bin"] = b"\xE9" + b"\x00" * 31  # hash in metadata won't match
    directory = write_release_directory(tmp_path / "release", images_=images_)
    (tmp_path / "release" / "firmware.bin").write_bytes(b"\xE9" + b"\x00" * 63)
    result = run_builder(directory, tmp_path / "merged.bin")
    assert result.returncode != 0
    assert not (tmp_path / "merged.bin").exists()


def test_workflow_builds_and_publishes_merged_binary():
    workflow = WORKFLOW.read_text()

    assert "Build merged full-flash binary" in workflow
    assert "python tools/merge_pyxis_release.py" in workflow
    assert "--directory docs/flasher/firmware" in workflow
    assert 'pyxis-${VERSION}-merged.bin' in workflow
    assert "docs/flasher/firmware/pyxis-*-merged.bin" in workflow
    # The merged build must be gated to tag builds, like the other release artifacts.
    merged_step = workflow[workflow.index("Build merged full-flash binary") :]
    assert "if: startsWith(github.ref, 'refs/tags/v')" in merged_step[: merged_step.index("name: Download existing release assets")]


def test_readme_documents_merged_binary_with_honest_semantics():
    readme = README.read_text()

    assert "merged binary" in readme.lower()
    assert "write_flash 0x0" in readme
    assert re.search(r"pyxis-<tag>-merged\.bin", readme)
    # The README must say the merged image wipes persistent data and that
    # data-preserving updates use firmware.bin.
    assert "erase_flash" in readme
    assert "NVS" in readme
    assert "firmware.bin" in readme
