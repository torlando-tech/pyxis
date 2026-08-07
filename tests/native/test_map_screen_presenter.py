"""Compile the presenter as strict C++11 and run 100k operations under ASan/UBSan."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess

from native_test import find_cxx

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def test_map_screen_presenter_cpp11_sanitized(tmp_path):
    binary = tmp_path / "test_map_screen_presenter"
    command = [
        find_cxx(),
        "-std=c++11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pedantic",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        f"-I{ROOT / 'lib/tdeck_ui'}",
        str(HERE / "test_map_screen_presenter.cpp"),
        str(ROOT / "lib/tdeck_ui/UI/LXMF/MapScreenPresenter.cpp"),
        str(ROOT / "lib/tdeck_ui/UI/LXMF/MapViewModel.cpp"),
        str(ROOT / "lib/tdeck_ui/UI/LXMF/MapProjection.cpp"),
        "-o",
        str(binary),
    ]
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    ran = subprocess.run(
        [str(binary)], capture_output=True, text=True, timeout=180, env=env
    )
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "0 failed" in ran.stdout
