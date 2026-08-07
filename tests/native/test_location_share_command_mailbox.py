"""Strict C++11 sanitizer coverage for the fixed UI command mailbox."""
from pathlib import Path
import os, shutil, subprocess
ROOT = Path(__file__).resolve().parents[2]

def test_location_share_command_mailbox(tmp_path):
    cxx = shutil.which("clang++") or shutil.which("g++")
    binary = tmp_path / "mailbox"
    cmd = [cxx, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
           "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
           f"-I{ROOT / 'lib/tdeck_ui'}", str(ROOT / "tests/native/test_location_share_command_mailbox.cpp"), "-o", str(binary)]
    built = subprocess.run(cmd, capture_output=True, text=True)
    assert built.returncode == 0, built.stdout + built.stderr
    env = os.environ.copy(); env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"; env["UBSAN_OPTIONS"] = "halt_on_error=1"
    ran = subprocess.run([str(binary)], capture_output=True, text=True, env=env)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "4 passed, 0 failed" in ran.stdout
