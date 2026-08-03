"""Compile and execute LXST signalling-list parser regressions."""

import shutil
import subprocess
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
TEST_SOURCE = HERE / "test_lxst_signal_parser.cpp"


def test_lxst_signal_parser(tmp_path):
    cxx = shutil.which("c++")
    if not cxx:
        pytest.skip("no C++ compiler found")
    binary = tmp_path / "test_lxst_signal_parser"
    compiled = subprocess.run(
        [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(TEST_SOURCE), "-o", str(binary)],
        capture_output=True,
        text=True,
    )
    assert compiled.returncode == 0, compiled.stderr
    ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "15 passed, 0 failed" in ran.stdout
