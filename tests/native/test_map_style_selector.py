"""Compile and run the bounded map-style selector under ASan/UBSan."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess

import pytest
from native_test import find_cxx

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


@pytest.mark.parametrize("sanitized", [False, True], ids=["strict-cxx11", "asan-ubsan"])
def test_map_style_selector(tmp_path, sanitized):
    binary = tmp_path / "test_map_style_selector"
    command = [
        find_cxx(), "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
        f"-I{ROOT / 'lib/tdeck_ui'}",
        str(HERE / "test_map_style_selector.cpp"),
        str(ROOT / "lib/tdeck_ui/UI/LXMF/MapStyleSelector.cpp"),
        "-o", str(binary),
    ]
    if sanitized:
        command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    env = os.environ.copy()
    if sanitized:
        env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60, env=env)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "0 failed" in ran.stdout
