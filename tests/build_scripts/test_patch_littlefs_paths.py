"""Regression tests for persistence-safe LittleFS build patching."""

import io
from contextlib import redirect_stdout
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
PATCH_SCRIPT = REPO_ROOT / "patch_littlefs_paths.py"
ADAPTER_REL = Path(".pio/libdeps/tdeck/microStore/include/microStore/Adapters/LittleFSFileSystem.h")
TRANSPORT_REL = Path(".pio/libdeps/tdeck/microReticulum/src/microReticulum/Transport.cpp")


class MockEnv:
    def __init__(self, project_dir):
        self.project_dir = str(project_dir)

    def get(self, key, default=None):
        if key == "PROJECT_DIR":
            return self.project_dir
        if key == "PIOENV":
            return "tdeck"
        return default


def run_patch(project_dir):
    src = PATCH_SCRIPT.read_text().replace('Import("env")', "")
    namespace = {
        "__name__": "patch_littlefs_paths_under_test",
        "__file__": str(PATCH_SCRIPT),
        "env": MockEnv(project_dir),
    }
    output = io.StringIO()
    with redirect_stdout(output):
        exec(src, namespace)
    return output.getvalue()


@pytest.fixture
def pristine_tree(tmp_path):
    adapter = tmp_path / ADAPTER_REL
    adapter.parent.mkdir(parents=True)
    adapter.write_text(
        """#include <LittleFS.h>
namespace microStore { namespace Adapters {
bool init() { return LittleFS.begin(true, _basepath); }
auto a = LittleFS.open(path, pmode);
auto b = LittleFS.open(path, FILE_READ);
auto c = LittleFS.open(path);
auto d = LittleFS.exists(path);
auto e = LittleFS.remove(path);
auto f = LittleFS.rename(from_path, to_path);
auto g = LittleFS.mkdir(path);
auto h = LittleFS.rmdir(path);
}}
"""
    )
    transport = tmp_path / TRANSPORT_REL
    transport.parent.mkdir(parents=True)
    transport.write_text(
        'store.init(Utilities::OS::get_filesystem(), "./path_store", false);\n'
    )
    return tmp_path


def test_patch_disables_format_on_mount_failure(pristine_tree):
    output = run_patch(pristine_tree)
    adapter = (pristine_tree / ADAPTER_REL).read_text()

    assert "LittleFS.begin(false, _basepath)" in adapter
    assert "LittleFS.begin(true, _basepath)" not in adapter
    assert "_pyxis_norm_path" in adapter
    assert "disabled LittleFS format-on-mount-failure" in output
    assert '"/path_store"' in (pristine_tree / TRANSPORT_REL).read_text()


def test_existing_path_patch_still_receives_safe_mount(pristine_tree):
    run_patch(pristine_tree)
    adapter_path = pristine_tree / ADAPTER_REL
    adapter_path.write_text(
        adapter_path.read_text().replace(
            "LittleFS.begin(false, _basepath)",
            "LittleFS.begin(true, _basepath)",
        )
    )

    output = run_patch(pristine_tree)
    adapter = adapter_path.read_text()

    assert "path normalization already patched" in output
    assert "disabled LittleFS format-on-mount-failure" in output
    assert "LittleFS.begin(false, _basepath)" in adapter


def test_patch_is_idempotent(pristine_tree):
    run_patch(pristine_tree)
    adapter_path = pristine_tree / ADAPTER_REL
    transport_path = pristine_tree / TRANSPORT_REL
    before = (adapter_path.read_text(), transport_path.read_text())

    output = run_patch(pristine_tree)

    assert "non-destructive LittleFS mount already patched" in output
    assert before == (adapter_path.read_text(), transport_path.read_text())


def test_unknown_mount_call_fails_closed(pristine_tree):
    adapter_path = pristine_tree / ADAPTER_REL
    adapter_path.write_text(
        adapter_path.read_text().replace(
            "LittleFS.begin(true, _basepath)",
            "LittleFS.begin(upstream_default, _basepath)",
        )
    )

    with pytest.raises(SystemExit):
        run_patch(pristine_tree)


def test_application_explicitly_disables_reformat_on_init():
    source = (REPO_ROOT / "src/main.cpp").read_text()
    assert "fs.init(false)" in source
    assert "fs.init()" not in source


def test_persistent_partitions_do_not_overlap_app_slots():
    rows = []
    for raw in (REPO_ROOT / "partitions.csv").read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = [field.strip() for field in line.split(",")]
        rows.append((fields[0], int(fields[3], 0), int(fields[4], 0)))

    by_name = {name: (offset, offset + size) for name, offset, size in rows}
    persistent = [by_name["nvs"], by_name["spiffs"]]
    applications = [by_name["app0"], by_name["app1"]]

    for data_start, data_end in persistent:
        for app_start, app_end in applications:
            assert data_end <= app_start or app_end <= data_start
