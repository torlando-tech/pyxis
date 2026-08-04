import shutil
import subprocess
from pathlib import Path

import pytest


HERE = Path(__file__).resolve().parent
PYXIS_ROOT = HERE.parent.parent
TEST_SOURCE = HERE / "test_sx1262_bitrate.cpp"
INTERFACE_SOURCE = PYXIS_ROOT / "lib" / "sx1262_interface" / "SX1262Interface.cpp"


def _find_cxx():
    for command in ("clang++", "g++"):
        if shutil.which(command):
            return command
    pytest.skip("no C++ compiler found")


def test_sx1262_bitrate_matches_python_rns_units(tmp_path):
    binary = tmp_path / "test_sx1262_bitrate"
    command = [
        _find_cxx(),
        "-std=c++17",
        "-Wall",
        "-Wextra",
        f"-I{PYXIS_ROOT / 'lib' / 'sx1262_interface'}",
        str(TEST_SOURCE),
        "-o",
        str(binary),
    ]
    compiled = subprocess.run(command, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr

    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip() == "6 passed, 0 failed"


def test_sx1262_interface_uses_the_unit_safe_bitrate_helper():
    source = INTERFACE_SOURCE.read_text()
    assert source.count("calculate_lora_bitrate_bps(") == 2
    assert "_config.bandwidth / 1000.0" not in source
