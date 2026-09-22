import shutil
import subprocess
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
INCLUDE = ROOT / "lib" / "tdeck_ui" / "UI" / "LXMF"
SOURCE = HERE / "test_nomadnet_image_loader.cpp"


def _cxx():
    for name in ("clang++", "g++"):
        if shutil.which(name):
            return name
    pytest.skip("no C++ compiler found")


def test_nomadnet_image_loader_native(tmp_path):
    assert (INCLUDE / "NomadNetImageLoader.cpp").is_file(), "loader source missing"
    binary = tmp_path / "test_nomadnet_image_loader"
    command = [
        _cxx(), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        f"-I{INCLUDE}", str(SOURCE),
        str(INCLUDE / "NomadNetImageLoader.cpp"),
        "-o", str(binary),
    ]
    compiled = subprocess.run(command, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "ALL IMAGE LOADER TESTS PASSED" in result.stdout
