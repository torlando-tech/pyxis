from __future__ import annotations

from pathlib import Path
import os
import shutil
import subprocess

import pytest

from native_test import find_cxx

ROOT = Path(__file__).resolve().parents[2]
TEST_SOURCE = ROOT / "tests/native/test_map_tile_store_path_capacity.cpp"
PACK_SOURCE = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTilePack.cpp"
STORE_SOURCE = ROOT / "lib/tdeck_ui/Hardware/TDeck/MapTileStoreSD.cpp"
SDACCESS_SOURCE = ROOT / "lib/tdeck_ui/Hardware/TDeck/SDAccess.cpp"
CODEC_SOURCE = ROOT / "lib/tdeck_ui/Hardware/TDeck/ActiveMapSetCodec.cpp"
MANIFEST_SOURCE = ROOT / "lib/tdeck_ui/UI/LXMF/MapPackManifest.cpp"
SHIM_DIR = ROOT / "tests/native/sdhostshim"


def sudo_ok() -> bool:
    return shutil.which("sudo") is not None and subprocess.run(
        ["sudo", "-n", "true"], capture_output=True, timeout=10
    ).returncode == 0


def sd_writable() -> bool:
    return os.access("/sd", os.W_OK)


def run_test(binary: Path, card_root: Path, env: dict, timeout: int) -> subprocess.CompletedProcess[str]:
    """Run the test binary, arranging a /sd bind mount for the end-to-end
    section when possible.

    The production store reads the literal "/sd" mount point, so the
    end-to-end card-content section only runs when "/sd" is a writable
    directory. Without a usable password-less sudo we still run the binary
    normally: the core regression section (mount-independent) executes, and
    the binary prints a skip note for the card section.
    """
    command = [str(binary), str(card_root)]
    mounted = False
    run_as_root = False
    if not sd_writable() and sudo_ok():
        probe = subprocess.run(["mountpoint", "-q", "/sd"], capture_output=True, timeout=10)
        if probe.returncode == 0:
            # A live mount we did not create: leave it alone, run normally.
            pass
        else:
            subprocess.run(
                ["sudo", "-n", "mkdir", "-p", "/sd"], capture_output=True, timeout=10)
            mnt = subprocess.run(
                ["sudo", "-n", "mount", "--bind", str(card_root), "/sd"],
                capture_output=True, text=True, timeout=30,
            )
            if mnt.returncode == 0:
                mounted = True
                run_as_root = True  # bind mount content is root-owned
            else:
                raise pytest.skip(f"could not bind-mount card root at /sd: {mnt.stderr.strip()}")
    if run_as_root:
        command = ["sudo", "-n"] + command
    try:
        return subprocess.run(command, capture_output=True, text=True,
                              timeout=timeout, env=env)
    finally:
        if mounted:
            subprocess.run(["sudo", "-n", "umount", "/sd"], capture_output=True, timeout=30)


@pytest.mark.parametrize("sanitize", [False, True], ids=["strict-cxx11", "asan-ubsan"])
def test_map_tile_store_path_capacity(tmp_path: Path, sanitize: bool) -> None:
    # Note: -Wconversion/-Wsign-conversion are deliberately omitted here (and
    # only here): SDAccess.cpp is ARDUINO-gated production code that only ever
    # compiles under PlatformIO's ESP32 flag set, where the uint8_t/int
    # Arduino API conversions are routine. The other flags above are kept so
    # the unmodified store/pack sources stay strictly checked.
    binary = tmp_path / "test_map_tile_store_path_capacity"
    card_root = tmp_path / "card"
    card_root.mkdir()
    command = [
        find_cxx(), "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
        "-DARDUINO=100",
        f"-I{SHIM_DIR}", f"-I{ROOT / 'lib/tdeck_ui'}",
        str(TEST_SOURCE), str(PACK_SOURCE), str(STORE_SOURCE),
        str(SDACCESS_SOURCE), str(CODEC_SOURCE), str(MANIFEST_SOURCE),
        "-o", str(binary),
    ]
    if sanitize:
        command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=120,
                              cwd=ROOT)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    env = os.environ.copy()
    if sanitize:
        env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    ran = run_test(binary, card_root, env, timeout=120)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    # The end-to-end section (4 tests) is conditional on a writable /sd mount;
    # the core regression section (6 tests) always runs.
    for expected in (
        "map tile store path capacity: 6 tests passed\n",
        "map tile store path capacity: 10 tests passed\n",
    ):
        if ran.stdout == expected:
            return
    raise AssertionError(f"unexpected output: {ran.stdout!r}")
