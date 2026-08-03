import os
import runpy
import sys
import types
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "sync_file_libdeps.py"


class FakeEnv(dict):
    def get(self, key, default=None):
        return super().get(key, default)


def test_local_override_uses_content_not_mtime(tmp_path, monkeypatch):
    source = tmp_path / "source"
    destination = tmp_path / "libdeps" / "microReticulum"
    source.mkdir()
    destination.mkdir(parents=True)

    source_file = source / "Transport.cpp"
    destination_file = destination / "Transport.cpp"
    source_file.write_text("patched source\n")
    destination_file.write_text("stale fetched source\n")

    # A just-fetched dependency can have a newer mtime than the local checkout.
    # The override must still win based on bytes, not timestamps.
    os.utime(source_file, (1, 1))
    os.utime(destination_file, (2, 2))

    helpers = types.ModuleType("_build_helpers")
    setattr(helpers, "env_libdeps_dir", lambda env: str(tmp_path / "libdeps"))
    monkeypatch.setitem(sys.modules, "_build_helpers", helpers)
    monkeypatch.setenv("PYXIS_MICRORETICULUM_DIR", str(source))

    env = FakeEnv(PROJECT_DIR=str(ROOT))
    runpy.run_path(
        str(SCRIPT),
        init_globals={"Import": lambda _: None, "env": env},
    )

    assert destination_file.read_text() == "patched source\n"
    assert destination_file.stat().st_mtime > 2


def test_local_override_runs_before_dependency_patch_scripts():
    config = (ROOT / "platformio.ini").read_text()
    scripts = config.split("extra_scripts =", 1)[1].split("platform =", 1)[0]
    assert scripts.index("pre:sync_file_libdeps.py") < scripts.index("pre:patch_littlefs_paths.py")
