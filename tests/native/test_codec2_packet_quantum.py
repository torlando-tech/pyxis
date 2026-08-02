"""Compile and execute the portable ULBW-only voice profile policy regression."""

import shutil
import subprocess
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
TEST_SOURCE = HERE / "test_codec2_packet_quantum.cpp"


def test_ulbw_voice_profile_policy(tmp_path):
    cxx = shutil.which("c++")
    if not cxx:
        pytest.skip("no C++ compiler found")
    binary = tmp_path / "test_codec2_packet_quantum"
    compiled = subprocess.run(
        [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(TEST_SOURCE), "-o", str(binary)],
        capture_output=True,
        text=True,
    )
    assert compiled.returncode == 0, compiled.stderr
    ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "13 passed, 0 failed" in ran.stdout
