from __future__ import annotations

from pathlib import Path
import os
import subprocess

import pytest

from native_test import find_cxx

ROOT = Path(__file__).resolve().parents[2]
TEST_SOURCE = ROOT / "tests/native/test_map_style_catalog.cpp"
CATALOG_SOURCE = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapStyleCatalog.cpp"
CODEC_SOURCE = ROOT / "lib/tdeck_ui/Hardware/TDeck/ActiveMapSetCodec.cpp"
PACK_SOURCE = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTilePack.cpp"
MANIFEST_SOURCE = ROOT / "lib/tdeck_ui/UI/LXMF/MapPackManifest.cpp"


@pytest.mark.parametrize("sanitize", [False, True], ids=["strict-cxx11", "asan-ubsan"])
def test_map_style_catalog(tmp_path: Path, sanitize: bool) -> None:
    binary = tmp_path / "test_map_style_catalog"
    command = [
        find_cxx(), "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
        "-Wconversion", "-Wsign-conversion", f"-I{ROOT / 'lib/tdeck_ui'}",
        str(TEST_SOURCE), str(CATALOG_SOURCE), str(CODEC_SOURCE), str(PACK_SOURCE),
        str(MANIFEST_SOURCE), "-o", str(binary),
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
    assert ran.stdout == "map style catalog: 12 tests passed\n"
