"""Contract tests for deterministic Columba-compatible Pyxis update packages."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import zipfile


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/package_pyxis_release.py"


def valid_firmware() -> bytes:
    image = bytearray(128)
    image[0] = 0xE9
    image[12:14] = (9).to_bytes(2, "little")
    return bytes(image)


def valid_boot_app0() -> bytes:
    image = bytearray([0xFF] * 0x2000)
    image[:4] = b"\x01\x00\x00\x00"
    return bytes(image)


def run_builder(tmp_path: Path, version: str = "v0.3.0") -> Path:
    tmp_path.mkdir(parents=True, exist_ok=True)
    firmware = tmp_path / "firmware.bin"
    boot = tmp_path / "boot_app0.bin"
    output = tmp_path / f"pyxis-{version}.pyxis.zip"
    firmware.write_bytes(valid_firmware())
    boot.write_bytes(valid_boot_app0())
    subprocess.run(
        [sys.executable, str(SCRIPT), "--firmware", str(firmware), "--boot-app0", str(boot), "--version", version, "--output", str(output)],
        check=True,
    )
    return output


def test_builder_emits_exact_columba_archive_and_manifest(tmp_path):
    package = run_builder(tmp_path)

    with zipfile.ZipFile(package) as archive:
        assert archive.namelist() == ["manifest.json", "firmware.bin", "boot_app0.bin"]
        manifest = json.loads(archive.read("manifest.json"))
        firmware = archive.read("firmware.bin")
        boot = archive.read("boot_app0.bin")

    assert manifest == {
        "schemaVersion": 1,
        "product": "pyxis",
        "board": "t-deck-plus",
        "chip": "esp32-s3",
        "version": "v0.3.0",
        "firmware": {
            "name": "firmware.bin",
            "offset": 0x10000,
            "size": len(firmware),
            "sha256": hashlib.sha256(firmware).hexdigest(),
        },
        "bootApp0": {
            "name": "boot_app0.bin",
            "offset": 0xE000,
            "size": len(boot),
            "sha256": hashlib.sha256(boot).hexdigest(),
        },
    }
    assert package.stat().st_size <= 4 * 1024 * 1024


def test_builder_is_byte_for_byte_deterministic(tmp_path):
    first = run_builder(tmp_path / "first")
    second = run_builder(tmp_path / "second")

    assert first.read_bytes() == second.read_bytes()


def test_release_workflow_builds_and_uploads_columba_package():
    workflow = (ROOT / ".github/workflows/release-firmware.yml").read_text()

    assert "Build Columba-compatible Pyxis package" in workflow
    assert "python tools/package_pyxis_release.py" in workflow
    assert 'PACKAGE_VERSION="${VERSION#v}"' in workflow
    assert '--version "${PACKAGE_VERSION}"' in workflow
    assert 'docs/flasher/firmware/pyxis-${VERSION}.pyxis.zip' in workflow
    assert "docs/flasher/firmware/*.pyxis.zip" in workflow
