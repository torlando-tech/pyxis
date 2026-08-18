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
        str(INCLUDE / "NomadNetForm.cpp"),
        str(INCLUDE / "NomadNetPartialController.cpp"),
        str(INCLUDE / "NomadNetPartialScheduler.cpp"),
        "-o", str(binary),
    ]
    compiled = subprocess.run(command, capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip() == "partial core parser checks passed"


def test_partial_transport_is_generation_owned_bounded_and_non_cacheable():
    manager_h = (INCLUDE / "UIManager.h").read_text()
    manager_cpp = (INCLUDE / "UIManager.cpp").read_text()
    controller_h = (INCLUDE / "NomadNetPartialController.h").read_text()
    assert "NomadNet::PartialScheduler _nomad_partial_scheduler;" in manager_h
    assert "NomadNet::PartialController _nomad_partial_controller;" in manager_h
    advance = manager_cpp.split(
        "uint32_t UIManager::nomad_advance_navigation_generation() {", 1
    )[1].split("\n}", 1)[0]
    assert "_nomad_partial_scheduler.cancel(_nomad_navigation_generation);" in advance
    assert "_nomad_partial_controller.cancel();" in advance
    publication = manager_cpp.split(
        "NomadNet::PageApplyResult UIManager::nomad_apply_page_document(", 1
    )[1].split("void UIManager::nomad_update()", 1)[0]
    assert publication.index("result != NomadNet::PageApplyResult::APPLIED") < publication.index(
        "_nomad_partial_scheduler.configure("
    )
    assert "_nomad_partial_controller.reset_page(document.source_bytes);" in publication
    assert "_nomad_partial_scheduler.poll(" in manager_cpp
    assert "prepare_partial_request(" in manager_cpp
    assert "apply_partial_fragment(" in manager_cpp
    assert "MAX_RESPONSE_BYTES = 16 * 1024" in controller_h
    assert "MAX_EXPANDED_SOURCE_BYTES" in controller_h
    start_link = manager_cpp.split("void UIManager::nomad_start_link() {", 1)[1].split(
        "void UIManager::nomad_identify_link_if_configured()", 1
    )[0]
    assert "_nomad_partial_controller.active()" in start_link
    assert "PartialController::MAX_RESPONSE_WIRE_BYTES" in start_link
    prepare_failure = manager_cpp.split("if (!prepared) {", 1)[1].split("return;", 1)[0]
    assert "_nomad_partial_scheduler.defer(request);" in prepare_failure
    assert "_nomad_partial_scheduler.complete(request, false" not in prepare_failure
    url_parse = manager_cpp.split("NomadNet::Url target;", 1)[1].split(
        "if (!nomad_supersede_transport", 1
    )[0]
    assert "allocation_failed = true;" in url_parse
    assert "_nomad_partial_scheduler.defer(request);" in url_parse
    request_function = manager_cpp.split("void UIManager::nomad_send_request()", 1)[1]
    request_oom = request_function.split("} catch (const std::bad_alloc&) {", 1)[1].split(
        "return;", 1
    )[0]
    assert "nomad_defer_partial(" in request_oom
    begin_partial = manager_cpp.split(
        "void UIManager::nomad_begin_partial_transport()", 1
    )[1].split("void UIManager::nomad_finish_partial", 1)[0]
    assert "catch (const std::bad_alloc&)" in begin_partial
    assert "nomad_defer_partial(" in begin_partial
    assert "bytes_equal_lower_hex(" in begin_partial
    release_partial = manager_cpp.split(
        "void UIManager::nomad_release_partial", 1
    )[1].split("void UIManager::nomad_begin_live_transport", 1)[0]
    assert "if (success) nomad_finish_request_keep_link();" in release_partial
    assert "nomad_release_request();" in release_partial
    response_branch = manager_cpp.split(
        "if (_nomad_partial_controller.active()) {", 6
    )[-1].split("NomadNet::Document document;", 1)[0]
    assert "commit_fragment(" in response_branch
    assert "_nomad_cache_pending_body" not in response_branch
    local_jump = manager_cpp.split(
        "if (local_result == NomadNet::LocalNavigationResult::APPLIED)", 1
    )[1].split("return;", 1)[0]
    assert "nomad_advance_navigation_generation()" not in local_jump
