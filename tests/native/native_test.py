"""Shared compile/run support for portable C++ production-code tests."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess

import pytest


def find_cxx() -> str:
    cxx = shutil.which("clang++") or shutil.which("g++")
    if not cxx:
        pytest.skip("no C++ compiler found")
    return cxx


def compile_and_run(
    tmp_path: Path,
    *,
    name: str,
    sources: list[Path],
    include_dirs: list[Path],
    sanitize: bool = False,
    timeout: int = 30,
) -> subprocess.CompletedProcess[str]:
    """Compile strict C++17 sources and execute the resulting test binary."""
    binary = tmp_path / name
    cmd = [
        find_cxx(),
        "-std=c++17",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-pedantic",
        *[f"-I{path}" for path in include_dirs],
        *[str(path) for path in sources],
        "-o",
        str(binary),
    ]
    if sanitize:
        cmd[1:1] = [
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
        ]

    compiled = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr

    env = os.environ.copy()
    if sanitize:
        env.setdefault("ASAN_OPTIONS", "detect_leaks=1:halt_on_error=1")
        env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    ran = subprocess.run(
        [str(binary)], capture_output=True, text=True, timeout=timeout, env=env
    )
    assert ran.returncode == 0, ran.stdout + ran.stderr
    return ran
