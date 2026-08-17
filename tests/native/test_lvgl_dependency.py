import importlib.util
import os
import shutil
import subprocess
from pathlib import Path

import pytest


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RESOLVER = ROOT / "tools" / "resolve_lvgl.py"
PATCHER = ROOT / "patch_lvgl_textarea.py"
LVGL = ROOT / ".pio" / "libdeps" / "tdeck" / "lvgl"
PLATFORMIO = Path(
    os.environ.get("PYXIS_PLATFORMIO_BIN")
    or shutil.which("pio")
    or "/tmp/pyxis-platformio/bin/pio"
)
VANILLA_DIGEST = "9a6ca0d597f4a17792c04a19104482c65e5af20401414a3c393c8d8a6a175a47"
EXPECTED_DIGEST = "be9f52b7aa9ca3fe379569cdf59fe43ebd9e676bc775b72b86cde91c59808ec2"


def _resolver_module():
    spec = importlib.util.spec_from_file_location("pyxis_resolve_lvgl", RESOLVER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def run_resolver(root: Path, *, env=None, timeout=300):
    return subprocess.run(
        ["/usr/bin/python3", str(RESOLVER), "--root", str(root)],
        capture_output=True,
        text=True,
        env=env,
        timeout=timeout,
    )


def make_resolver_root(root: Path) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / "platformio.ini", root / "platformio.ini")
    shutil.copy2(PATCHER, root / PATCHER.name)
    return root


@pytest.fixture(scope="module")
def fresh_real_resolution(tmp_path_factory):
    """Install vanilla LVGL with the CI pio, then patch it through the resolver."""
    if not PLATFORMIO.is_file() or not os.access(PLATFORMIO, os.X_OK):
        pytest.fail(f"required CI PlatformIO executable is missing: {PLATFORMIO}")
    root = make_resolver_root(tmp_path_factory.mktemp("fresh-lvgl-worktree"))
    installed = subprocess.run(
        [str(PLATFORMIO), "pkg", "install", "-e", "tdeck"],
        cwd=root,
        capture_output=True,
        text=True,
        timeout=600,
    )
    assert installed.returncode == 0, installed.stdout + installed.stderr
    lvgl = root / ".pio" / "libdeps" / "tdeck" / "lvgl"
    resolver = _resolver_module()
    assert resolver.source_tree_digest(lvgl) == VANILLA_DIGEST

    vanilla = root.parent / "vanilla-lvgl"
    shutil.copytree(lvgl, vanilla)
    env = os.environ.copy()
    env["PYXIS_PLATFORMIO_BIN"] = str(PLATFORMIO)
    first = run_resolver(root, env=env)
    assert first.returncode == 0, first.stdout + first.stderr
    assert f"LVGL 8.4.0 {EXPECTED_DIGEST}" in first.stdout
    assert resolver.source_tree_digest(lvgl) == EXPECTED_DIGEST

    # A second resolver run must be a true no-op over the already patched tree.
    second = run_resolver(root, env=env)
    assert second.returncode == 0, second.stdout + second.stderr
    assert f"LVGL 8.4.0 {EXPECTED_DIGEST}" in second.stdout
    assert resolver.source_tree_digest(lvgl) == EXPECTED_DIGEST
    return root, vanilla


def test_resolver_verifies_exact_version_and_stable_tree_digest():
    result = run_resolver(ROOT)
    assert result.returncode == 0, result.stdout + result.stderr
    assert f"LVGL 8.4.0 {EXPECTED_DIGEST}" in result.stdout
    assert str(LVGL) in result.stdout


def test_resolver_rejects_altered_version(tmp_path):
    root = make_resolver_root(tmp_path / "worktree")
    candidate = root / ".pio" / "libdeps" / "tdeck" / "lvgl"
    shutil.copytree(LVGL, candidate)
    metadata = candidate / "library.json"
    metadata.write_text(metadata.read_text().replace('"version": "8.4.0"', '"version": "8.4.1"'))

    result = run_resolver(root)
    assert result.returncode != 0
    assert "expected LVGL version 8.4.0, found 8.4.1" in result.stderr


def test_resolver_rejects_mutated_patched_tree(tmp_path):
    root = make_resolver_root(tmp_path / "worktree")
    candidate = root / ".pio" / "libdeps" / "tdeck" / "lvgl"
    shutil.copytree(LVGL, candidate)
    source = candidate / "src" / "core" / "lv_obj.c"
    source.write_bytes(source.read_bytes() + b"\n/* altered patched tree */\n")

    result = run_resolver(root)
    assert result.returncode != 0
    assert "LVGL source digest mismatch before patch" in result.stderr
    assert VANILLA_DIGEST in result.stderr
    assert EXPECTED_DIGEST in result.stderr


def test_missing_dependency_uses_real_ci_platformio_and_is_idempotent(fresh_real_resolution):
    root, _ = fresh_real_resolution
    assert (root / ".pio" / "libdeps" / "tdeck" / "lvgl").is_dir()


def test_resolver_rejects_mutated_vanilla_tree_before_patching(
        tmp_path, fresh_real_resolution):
    _, vanilla = fresh_real_resolution
    root = make_resolver_root(tmp_path / "mutated-vanilla-worktree")
    candidate = root / ".pio" / "libdeps" / "tdeck" / "lvgl"
    shutil.copytree(vanilla, candidate)
    source = candidate / "src" / "core" / "lv_obj.c"
    source.write_bytes(source.read_bytes() + b"\n/* altered vanilla tree */\n")

    result = run_resolver(root)
    assert result.returncode != 0
    assert "LVGL source digest mismatch before patch" in result.stderr
    assert VANILLA_DIGEST in result.stderr
    assert EXPECTED_DIGEST in result.stderr


def test_platformio_dependency_is_exact_not_range_pinned():
    platformio = (ROOT / "platformio.ini").read_text()
    assert "lvgl/lvgl@8.4.0" in platformio
    assert "lvgl/lvgl@^8.3.11" not in platformio
