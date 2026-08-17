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


def test_parse_and_compact_application_recover_from_every_allocation_failure(tmp_path):
    compact_header = (INCLUDE / "NomadNetCompactPage.h").read_text()
    assert "retained_capacity_bytes" not in compact_header
    binary = tmp_path / "test_nomadnet_parse_apply_alloc_failure"
    command = [
        _cxx(), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        f"-I{INCLUDE}", str(HERE / "test_nomadnet_parse_apply_alloc_failure.cpp"),
        str(INCLUDE / "NomadNetDocument.cpp"),
        str(INCLUDE / "NomadNetCompactPage.cpp"),
        str(INCLUDE / "NomadNetForm.cpp"),
        str(INCLUDE / "NomadNetGlyphs.cpp"),
        str(INCLUDE / "NomadNetLibrary.cpp"),
        str(INCLUDE / "NomadNetOwner.cpp"),
        str(INCLUDE / "NomadNetUrl.cpp"), "-o", str(binary),
    ]
    compiled = subprocess.run(command, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
