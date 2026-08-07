"""Compile and execute the portable location LXMF adapter integration harness."""
from pathlib import Path
import os
import subprocess

from native_test import find_cxx

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent


def test_location_lxmf_adapter(tmp_path):
    binary = tmp_path / "test_location_lxmf_adapter"
    sources = [
        HERE / "test_location_lxmf_adapter.cpp",
        ROOT / "lib/tdeck_ui/Telemetry/LocationLxmfAdapter.cpp",
        ROOT / "lib/tdeck_ui/Telemetry/LocationTelemetryCodec.cpp",
        ROOT / "lib/tdeck_ui/Telemetry/LocationShareState.cpp",
        ROOT / "lib/tdeck_ui/Telemetry/LocationShareScheduler.cpp",
    ]
    command = [
        find_cxx(), "-std=c++11", "-Wall", "-Wextra", "-Werror",
        "-Wconversion", "-Wsign-conversion", "-pedantic",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        f"-I{ROOT / 'lib/tdeck_ui'}", *map(str, sources), "-o", str(binary),
    ]
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    ran = subprocess.run(
        [str(binary)], capture_output=True, text=True, timeout=90,
        env={**os.environ,
             "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
             "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"},
    )
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "location LXMF adapter:" in ran.stdout
    assert "0 failed" in ran.stdout
