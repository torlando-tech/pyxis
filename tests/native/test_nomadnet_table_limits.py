import shutil
import subprocess
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
INCLUDE = ROOT / "lib" / "tdeck_ui" / "UI" / "LXMF"


def _cxx():
    for name in ("clang++", "g++"):
        if shutil.which(name):
            return name
    pytest.skip("no C++ compiler found")


def test_table_tokenization_and_exact_boundaries_under_sanitizers(tmp_path):
    binary = tmp_path / "test_nomadnet_table_limits"
    command = [
        _cxx(), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        f"-I{INCLUDE}", str(HERE / "test_nomadnet_table_limits.cpp"),
        str(INCLUDE / "NomadNetDocument.cpp"), "-o", str(binary),
    ]
    compiled = subprocess.run(command, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30,
                            env={"ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                                 "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"})
    assert result.returncode == 0, result.stdout + result.stderr
