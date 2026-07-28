"""Build and run telemetry fields through the exact pinned microLXMF source."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MICROLXMF = ROOT / ".pio" / "libdeps" / "tdeck" / "microLXMF"


def test_pinned_microlxmf_telemetry_field_roundtrip(tmp_path):
    if os.environ.get("PYXIS_RUN_MICROLXMF_NATIVE") != "1":
        pytest.skip("set PYXIS_RUN_MICROLXMF_NATIVE=1 for the dependency-level test")

    microlxmf = Path(os.environ.get("PYXIS_MICROLXMF_SRC", DEFAULT_MICROLXMF))
    bridge = microlxmf / "conformance-bridge"
    source = microlxmf / "src"
    if not (source / "LXMF" / "LXMessage.cpp").is_file():
        pytest.fail(f"pinned microLXMF source is unavailable at {microlxmf}")

    expected = "d9bbc04cf69bfa9b555c3f293b89b440b4820518"
    actual = subprocess.run(
        ["git", "-C", str(microlxmf), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    assert actual == expected

    build = tmp_path / "build"
    configure = subprocess.run(
        [
            "cmake",
            "-S",
            str(ROOT / "tests" / "microlxmf"),
            "-B",
            str(build),
            f"-DMICROLXMF_BRIDGE_DIR={bridge}",
            f"-DMICROLXMF_SRC={source}",
            "-DCMAKE_BUILD_TYPE=Debug",
        ],
        capture_output=True,
        text=True,
        timeout=300,
    )
    assert configure.returncode == 0, configure.stdout + configure.stderr

    compiled = subprocess.run(
        ["cmake", "--build", str(build), "--target", "test_pyxis_telemetry_field", "-j2"],
        capture_output=True,
        text=True,
        timeout=600,
    )
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr

    binary = build / "test_pyxis_telemetry_field"
    ran = subprocess.run(
        [str(binary)], capture_output=True, text=True, timeout=60,
        env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1"},
    )
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "microLXMF telemetry field roundtrip: passed" in ran.stdout
