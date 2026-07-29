"""Compile the portable control model as strict C++11 with ASan/UBSan."""
from pathlib import Path
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def test_location_share_control_model_strict_cpp11(tmp_path):
    compiler = shutil.which("clang++") or shutil.which("g++")
    assert compiler, "C++ compiler required"
    binary = tmp_path / "location_share_control_model"
    command = [
        compiler,
        "-std=c++11",
        "-Wall", "-Wextra", "-Werror", "-pedantic",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        f"-I{ROOT / 'lib/tdeck_ui'}",
        str(ROOT / "tests/native/test_location_share_control_model.cpp"),
        str(ROOT / "lib/tdeck_ui/UI/LXMF/LocationShareControlModel.cpp"),
        "-o", str(binary),
    ]
    compiled = subprocess.run(command, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    ran = subprocess.run([str(binary)], capture_output=True, text=True, env=env)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "6 passed, 0 failed" in ran.stdout
