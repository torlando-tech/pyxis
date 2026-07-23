"""Compile and execute the portable trackball button debounce regression."""

import shutil
import subprocess
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
PYXIS_ROOT = HERE.parent.parent
TEST_SOURCE = HERE / "test_button_debouncer.cpp"


def test_button_debouncer(tmp_path):
    cxx = shutil.which("clang++") or shutil.which("g++")
    if not cxx:
        pytest.skip("no C++ compiler found")
    binary = tmp_path / "test_button_debouncer"
    cmd = [
        cxx,
        "-std=c++17",
        "-Wall",
        "-Wextra",
        f"-I{PYXIS_ROOT / 'lib' / 'tdeck_ui' / 'Hardware' / 'TDeck'}",
        str(TEST_SOURCE),
        "-o",
        str(binary),
    ]
    compiled = subprocess.run(cmd, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "19 passed, 0 failed" in ran.stdout
