#!/usr/bin/env python3
"""Fail closed when a tdeck-release build is not release-qualified."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]
ENVIRONMENT = "tdeck-release"
BUILD = ROOT / ".pio" / "build" / ENVIRONMENT
EXCLUDED_DEFINITIONS = (
    "PYXIS_TEST_HOOKS",
    "PYXIS_TEST_TCP_HOST",
    "PYXIS_TEST_TCP_PORT",
    "MEMORY_INSTRUMENTATION_ENABLED",
    "BOOT_PROFILING_ENABLED",
)
EXCLUDED_STRINGS = (
    b"T:CALL_PROFILE",
    b"T:CALL_INJECT",
    b"T:DEST",
    b"PYXIS_TEST_HOOKS",
    b"Boot Profile Summary",
    b"Memory monitor started",
    b"Firmware: v1.0.0",
    b"SPIFFS FileSystem mount failed",
)
REQUIRED_STRINGS = (
    b"FileSystem mount failed; preserving persistent data",
)
EXCLUDED_SYMBOLS = (
    "test_call_",
    "process_test_serial_command",
    "MemoryMonitor::",
    "BootProfiler::",
)
PINNED_DEPENDENCIES = {
    "microReticulum": "1bbd422a3b25b4a710642fc5cd01039e5774a4a7",
    "microLXMF": "d9bbc04cf69bfa9b555c3f293b89b440b4820518",
    "microStore": "c5fb69d68229e684c7fbd17692a67ae8193b84e2",
}


def run(*args: str, cwd: Path = ROOT) -> str:
    return subprocess.run(
        args,
        cwd=cwd,
        check=True,
        text=True,
        capture_output=True,
    ).stdout.strip()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"release audit failed: {message}")


def package_commit(name: str) -> str:
    package = ROOT / ".pio" / "libdeps" / ENVIRONMENT / name
    require(package.is_dir(), f"missing dependency package {name}")
    top = Path(run("git", "rev-parse", "--show-toplevel", cwd=package)).resolve()
    require(top == package.resolve(), f"{name} is not an independently verifiable VCS package")
    return run("git", "rev-parse", "HEAD", cwd=package)


def locate_nm() -> str:
    direct = shutil.which("xtensa-esp32s3-elf-nm")
    if direct:
        return direct
    candidate = (
        Path.home()
        / ".platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-nm"
    )
    require(candidate.is_file(), "xtensa-esp32s3-elf-nm not found")
    return str(candidate)


def main() -> None:
    run("pio", "run", "-e", ENVIRONMENT, "-t", "compiledb")

    compile_commands = ROOT / "compile_commands.json"
    commands = json.loads(compile_commands.read_text())
    main_compile = next(
        entry for entry in commands if Path(entry["file"]).as_posix().endswith("src/main.cpp")
    )
    command = main_compile.get("command") or " ".join(main_compile["arguments"])
    present = [flag for flag in EXCLUDED_DEFINITIONS if flag in command]
    require(not present, f"excluded compiler definitions present: {', '.join(present)}")

    firmware = BUILD / "firmware.bin"
    elf = BUILD / "firmware.elf"
    require(firmware.is_file(), "firmware.bin missing")
    require(elf.is_file(), "firmware.elf missing")

    firmware_bytes = firmware.read_bytes()
    strings_present = [value.decode() for value in EXCLUDED_STRINGS if value in firmware_bytes]
    require(not strings_present, f"excluded firmware strings present: {', '.join(strings_present)}")
    strings_missing = [value.decode() for value in REQUIRED_STRINGS if value not in firmware_bytes]
    require(not strings_missing, f"required firmware strings missing: {', '.join(strings_missing)}")

    expected_version = os.environ.get("PYXIS_VERSION_OVERRIDE") or run(
        "git", "describe", "--tags", "--always", "--dirty"
    ).removeprefix("v")
    expected_version_label = f"Firmware: {expected_version}".encode()
    require(expected_version_label in firmware_bytes, f"embedded version label missing: {expected_version}")

    symbols = run(locate_nm(), "-C", str(elf))
    symbols_present = [value for value in EXCLUDED_SYMBOLS if value in symbols]
    require(not symbols_present, f"excluded ELF symbols present: {', '.join(symbols_present)}")

    for name, expected in PINNED_DEPENDENCIES.items():
        actual = package_commit(name)
        require(actual == expected, f"{name} resolved to {actual}, expected {expected}")

    compile_commands.unlink()
    print("release audit passed")
    print(f"firmware_size={len(firmware_bytes)}")
    print(f"firmware_version={expected_version}")
    for name, expected in PINNED_DEPENDENCIES.items():
        print(f"{name}={expected}")


if __name__ == "__main__":
    main()
