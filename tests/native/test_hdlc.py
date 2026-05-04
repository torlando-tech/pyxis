"""
Pytest wrapper that compiles + runs the native HDLC C++ tests.

This sidesteps PlatformIO entirely — we compile the C++ test directly with
the system g++/clang++, using a minimal Bytes shim so HDLC.h works without
its full microReticulum dependency tree.

The pattern is intentionally generic: any pyxis-unique C++ unit can get a
sibling test_<thing>.cpp + test_<thing>.py wrapper using the same shim.
"""

import os
import shutil
import subprocess
from pathlib import Path

import pytest


HERE = Path(__file__).resolve().parent
TEST_SOURCE = HERE / "test_hdlc.cpp"


def _find_cxx():
    """Pick a C++ compiler. Prefer clang++ (Mac default), fall back to g++."""
    for cmd in ("clang++", "g++"):
        if shutil.which(cmd):
            return cmd
    pytest.skip("no C++ compiler found")


def test_hdlc_compiles_and_passes(tmp_path):
    cxx = _find_cxx()
    binary = tmp_path / "test_hdlc"

    cmd = [
        cxx,
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Wno-unused-parameter",
        f"-I{HERE}",          # bytes_shim.h + Bytes.h compatibility header
        str(TEST_SOURCE),
        "-o", str(binary),
    ]
    compile_result = subprocess.run(cmd, capture_output=True, text=True)
    assert compile_result.returncode == 0, (
        f"compilation failed:\n"
        f"--- cmd ---\n{' '.join(cmd)}\n"
        f"--- stderr ---\n{compile_result.stderr}"
    )

    run_result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    assert run_result.returncode == 0, (
        f"tests failed:\n--- stdout ---\n{run_result.stdout}\n"
        f"--- stderr ---\n{run_result.stderr}"
    )

    # Sanity: parse the "N passed, M failed" tail line to confirm tests ran.
    # Catches a regression where main() forgets to RUN() any tests.
    summary = run_result.stdout.strip().splitlines()[-1]  # "N passed, M failed"
    parts = summary.split()
    pass_count = int(parts[0])
    fail_count = int(parts[2])
    assert fail_count == 0, run_result.stdout
    assert pass_count >= 10, f"expected at least 10 HDLC tests, ran {pass_count}"
