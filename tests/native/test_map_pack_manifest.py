from __future__ import annotations

from pathlib import Path
import os
import subprocess

import pytest

from native_test import find_cxx

ROOT = Path(__file__).resolve().parents[2]
TEST_SOURCE = ROOT / "tests/native/test_map_pack_manifest.cpp"
PRODUCTION_SOURCE = ROOT / "lib/tdeck_ui/UI/LXMF/MapPackManifest.cpp"


@pytest.mark.parametrize("sanitize", [False, True], ids=["strict-cxx11", "asan-ubsan"])
def test_map_pack_manifest(tmp_path: Path, sanitize: bool) -> None:
    binary = tmp_path / "test_map_pack_manifest"
    command = [
        find_cxx(), "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
        "-Wconversion", "-Wsign-conversion", f"-I{ROOT / 'lib/tdeck_ui'}",
        str(TEST_SOURCE), str(PRODUCTION_SOURCE), "-o", str(binary),
    ]
    if sanitize:
        command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    env = os.environ.copy()
    if sanitize:
        env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60, env=env)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert ran.stdout == "map pack manifest: 13 tests passed\n"
