"""Pytest wrapper for native ring buffer tests (PCM + encoded SPSC)."""

import shutil
import subprocess
from pathlib import Path

import pytest


HERE = Path(__file__).resolve().parent
PYXIS_ROOT = HERE.parent.parent
TEST_SOURCE = HERE / "test_ring_buffers.cpp"


def _find_cxx():
    for cmd in ("clang++", "g++"):
        if shutil.which(cmd):
            return cmd
    pytest.skip("no C++ compiler found")


def test_ring_buffers(tmp_path):
    cxx = _find_cxx()
    binary = tmp_path / "test_ring_buffers"

    cmd = [
        cxx,
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Wno-unused-parameter",
        "-pthread",
        f"-I{HERE}",
        f"-I{PYXIS_ROOT / 'lib' / 'lxst_audio'}",
        str(TEST_SOURCE),
        str(PYXIS_ROOT / "lib" / "lxst_audio" / "packet_ring_buffer.cpp"),
        str(PYXIS_ROOT / "lib" / "lxst_audio" / "encoded_ring_buffer.cpp"),
        "-o", str(binary),
    ]
    compile_result = subprocess.run(cmd, capture_output=True, text=True)
    assert compile_result.returncode == 0, (
        f"compilation failed:\n--- cmd ---\n{' '.join(cmd)}\n"
        f"--- stderr ---\n{compile_result.stderr}"
    )

    run_result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
    assert run_result.returncode == 0, (
        f"tests failed:\n--- stdout ---\n{run_result.stdout}\n"
        f"--- stderr ---\n{run_result.stderr}"
    )
    summary = run_result.stdout.strip().splitlines()[-1]
    parts = summary.split()
    pass_count = int(parts[0])
    fail_count = int(parts[2])
    assert fail_count == 0, run_result.stdout
    assert pass_count >= 13, f"expected at least 13 ring buffer tests, ran {pass_count}"
