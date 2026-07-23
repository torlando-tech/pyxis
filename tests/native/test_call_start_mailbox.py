"""Compile and execute the portable LXST call-start mailbox regression."""

import shutil
import subprocess
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
TEST_SOURCE = HERE / "test_call_start_mailbox.cpp"


def test_call_start_mailbox(tmp_path):
    cxx = shutil.which("clang++") or shutil.which("g++")
    if not cxx:
        pytest.skip("no C++ compiler found")
    binary = tmp_path / "test_call_start_mailbox"
    cmd = [
        cxx,
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pthread",
        str(TEST_SOURCE),
        "-o",
        str(binary),
    ]
    compiled = subprocess.run(cmd, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "4 passed, 0 failed" in ran.stdout
