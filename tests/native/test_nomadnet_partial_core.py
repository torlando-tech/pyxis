import shutil
import subprocess
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
INCLUDE = ROOT / "lib" / "tdeck_ui" / "UI" / "LXMF"
SOURCE = HERE / "test_nomadnet_partial_core.cpp"


def _cxx():
    for name in ("clang++", "g++"):
        if shutil.which(name):
            return name
    pytest.skip("no C++ compiler found")


def test_nomadnet_partial_core_native(tmp_path):
    binary = tmp_path / "test_nomadnet_partial_core"
    command = [
        _cxx(), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        f"-I{INCLUDE}", str(SOURCE),
        str(INCLUDE / "NomadNetDocument.cpp"),
        str(INCLUDE / "NomadNetCompactPage.cpp"),
        str(INCLUDE / "NomadNetGlyphs.cpp"),
        str(INCLUDE / "NomadNetPartialScheduler.cpp"),
        "-o", str(binary),
    ]
    compiled = subprocess.run(command, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip() == "partial core parser checks passed"


def test_partial_scheduler_is_generation_owned_but_transport_dispatch_is_deferred():
    manager_h = (INCLUDE / "UIManager.h").read_text()
    manager_cpp = (INCLUDE / "UIManager.cpp").read_text()
    assert "NomadNet::PartialScheduler _nomad_partial_scheduler;" in manager_h
    advance = manager_cpp.split(
        "uint32_t UIManager::nomad_advance_navigation_generation() {", 1
    )[1].split("\n}", 1)[0]
    assert "_nomad_partial_scheduler.cancel(_nomad_navigation_generation);" in advance
    publication = manager_cpp.split(
        "NomadNet::PageApplyResult UIManager::nomad_apply_page_document(", 1
    )[1].split("void UIManager::nomad_update()", 1)[0]
    assert publication.index("result != NomadNet::PageApplyResult::APPLIED") < publication.index(
        "_nomad_partial_scheduler.configure("
    )
    # This increment owns only the bounded model and scheduler. Partial transport,
    # response replacement, and form-map integration belong to the next increment.
    assert "_nomad_partial_scheduler.poll(" not in manager_cpp
    local_jump = manager_cpp.split(
        "if (local_result == NomadNet::LocalNavigationResult::APPLIED)", 1
    )[1].split("return;", 1)[0]
    assert "nomad_advance_navigation_generation()" not in local_jump
