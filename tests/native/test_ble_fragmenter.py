"""
Pytest wrapper for the native BLEFragmenter <-> BLEReassembler tests.

Same pattern as test_hdlc.py: compile the C++ test against pyxis sources
plus the bytes/Log/OS shims, run, parse the summary line.
"""

import shutil
import subprocess
from pathlib import Path

import pytest


HERE = Path(__file__).resolve().parent
PYXIS_ROOT = HERE.parent.parent
TEST_SOURCE = HERE / "test_ble_fragmenter.cpp"


def _find_cxx():
    for cmd in ("clang++", "g++"):
        if shutil.which(cmd):
            return cmd
    pytest.skip("no C++ compiler found")


def test_ble_fragmenter_round_trip(tmp_path):
    cxx = _find_cxx()
    binary = tmp_path / "test_ble_fragmenter"

    cmd = [
        cxx,
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Wno-unused-parameter",
        f"-I{HERE}",                                        # shims
        f"-I{PYXIS_ROOT / 'lib' / 'ble_interface'}",
        str(TEST_SOURCE),
        str(PYXIS_ROOT / "lib" / "ble_interface" / "BLEFragmenter.cpp"),
        str(PYXIS_ROOT / "lib" / "ble_interface" / "BLEReassembler.cpp"),
        "-o", str(binary),
    ]
    compile_result = subprocess.run(cmd, capture_output=True, text=True)
    assert compile_result.returncode == 0, (
        f"compilation failed:\n--- cmd ---\n{' '.join(cmd)}\n"
        f"--- stderr ---\n{compile_result.stderr}"
    )

    run_result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert run_result.returncode == 0, (
        f"tests failed:\n--- stdout ---\n{run_result.stdout}\n"
        f"--- stderr ---\n{run_result.stderr}"
    )
    summary = run_result.stdout.strip().splitlines()[-1]
    parts = summary.split()
    pass_count = int(parts[0])
    fail_count = int(parts[2])
    assert fail_count == 0, run_result.stdout
    assert pass_count >= 10, f"expected at least 10 BLE fragmenter tests, ran {pass_count}"
