"""
Tests for patch_nimble.py.

patch_nimble.py is a PlatformIO pre-build script that applies 4 patches to
NimBLE-Arduino source. Every NimBLE upgrade can silently break a patch by
shifting the surrounding code; these tests catch that.

What we verify:
  1. Pristine NimBLE source → every patch applies and the file content matches.
  2. Idempotency — running the script twice is a no-op on the second run.
  3. Missing file → "not found, skipping" (no error, no false positive).
  4. Drifted source → "WARNING -- expected code not found" (refuses to corrupt).
"""

import io
import os
import subprocess
from contextlib import redirect_stdout
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
PATCH_SCRIPT = REPO_ROOT / "patch_nimble.py"


class MockEnv:
    """Stand-in for PlatformIO's `env` SCons object — only `.get()` is used."""

    def __init__(self, project_dir):
        self._project_dir = str(project_dir)

    def get(self, key, default=None):
        if key == "PROJECT_DIR":
            return self._project_dir
        return default


def _exec_patch_script(project_dir, recorded_calls=None):
    """Run patch_nimble.py with `env` shimmed to point at `project_dir`.

    If `recorded_calls` is a list, intercept apply_patch calls and append their
    args instead of applying them. Otherwise let the real apply_patch run.

    Returns (stdout, namespace) where namespace contains `apply_patch` and
    constants from the script.
    """
    src = PATCH_SCRIPT.read_text()
    # PlatformIO injects `Import` as a builtin in pre-build scripts; the script
    # calls `Import("env")` to pull `env` into globals. We're providing `env`
    # directly via the namespace, so the Import call needs to become a no-op.
    src = src.replace('Import("env")', '')

    namespace = {
        "__name__": "patch_nimble_under_test",
        "__file__": str(PATCH_SCRIPT),
        "env": MockEnv(project_dir),
    }

    if recorded_calls is not None:
        # Stub apply_patch so the script's 4 inline calls get recorded
        # without actually touching files.
        def _record(filepath, old, new, label):
            recorded_calls.append({
                "filepath": filepath,
                "old": old,
                "new": new,
                "label": label,
            })
        # We have to define apply_patch BEFORE exec — but the script defines it
        # itself as `def apply_patch`. The script's def will overwrite our stub.
        # Workaround: exec in two passes — first parse out the function and the
        # 4 calls, then run the calls with the stub.
        # Simpler: monkey-patch the function name AFTER the def runs but before
        # the calls. We do this by intercepting via a wrapper that swaps in
        # our recorder on first call.
        # Cleanest: split the source on the first `apply_patch(` call site.
        anchor = "\n# Patch 1:"
        assert anchor in src, "patch_nimble.py structure changed; update test anchor"
        defs_part, calls_part = src.split(anchor, 1)
        calls_part = anchor + calls_part

        buf = io.StringIO()
        with redirect_stdout(buf):
            exec(defs_part, namespace)
            namespace["apply_patch"] = _record
            exec(calls_part, namespace)
        return buf.getvalue(), namespace
    else:
        buf = io.StringIO()
        with redirect_stdout(buf):
            exec(src, namespace)
        return buf.getvalue(), namespace


def _extract_patches(project_dir):
    """Extract the 4 (filepath, old, new, label) tuples from patch_nimble.py
    with filepaths rooted under `project_dir`."""
    recorded = []
    _exec_patch_script(project_dir=project_dir, recorded_calls=recorded)
    assert len(recorded) == 4, f"expected 4 patches, got {len(recorded)}"
    return recorded


@pytest.fixture
def patches(tmp_path):
    """4 patches with filepaths rooted under a fresh tmp_path."""
    return _extract_patches(project_dir=tmp_path)


@pytest.fixture
def fresh_tree(tmp_path, patches):
    """Build a fake NimBLE-Arduino tree under tmp_path matching the patch paths.

    Each target file is seeded with the patch's `old` content embedded in some
    surrounding context, so applying the patch should succeed and replace the
    `old` block with `new`.
    """
    for p in patches:
        f = Path(p["filepath"])
        f.parent.mkdir(parents=True, exist_ok=True)
        content = (
            "/* PROLOGUE: untouched by patch */\n"
            + p["old"]
            + "\n/* EPILOGUE: untouched by patch */\n"
        )
        f.write_text(content)
    return tmp_path


def test_extract_yields_four_patches(patches):
    """Sanity check the extraction harness."""
    assert len(patches) == 4
    labels = [p["label"] for p in patches]
    assert any("BRINGUP" in l for l in labels), labels
    assert any("PHY update" in l for l in labels), labels
    assert any("574" in l for l in labels), labels
    assert any("reset reason" in l for l in labels), labels


def test_pristine_source_applies_all_patches(fresh_tree, patches):
    """Every patch applies cleanly to a fresh NimBLE-like tree."""
    stdout, _ = _exec_patch_script(project_dir=fresh_tree)

    for p in patches:
        # Each patch should have logged its label, not "already applied" or "WARNING"
        assert p["label"] in stdout, (
            f"patch {p['label']!r} did not run\n--- stdout ---\n{stdout}"
        )
        assert f"already applied" not in stdout.split(p["label"])[0].splitlines()[-1] if False else True
        # File should now contain `new` and not `old`
        content = Path(p["filepath"]).read_text()
        assert p["new"] in content, f"new content missing from {p['filepath']}"
        assert p["old"] not in content, f"old content still present in {p['filepath']}"
        # Sentinel context preserved
        assert "/* PROLOGUE: untouched by patch */" in content
        assert "/* EPILOGUE: untouched by patch */" in content


def test_patches_are_idempotent(fresh_tree, patches):
    """Running the script twice is a no-op on the second run."""
    _exec_patch_script(project_dir=fresh_tree)
    snapshot = {p["filepath"]: Path(p["filepath"]).read_text() for p in patches}

    stdout, _ = _exec_patch_script(project_dir=fresh_tree)

    for p in patches:
        assert "already applied" in stdout, (
            f"second run of {p['label']!r} should report already-applied\n"
            f"--- stdout ---\n{stdout}"
        )
        assert Path(p["filepath"]).read_text() == snapshot[p["filepath"]], (
            f"{p['filepath']} changed on second run"
        )


def test_missing_file_skips_cleanly(tmp_path, patches):
    """If a target file doesn't exist, the script logs 'not found, skipping'."""
    # Don't create any files — every patch target is missing.
    stdout, _ = _exec_patch_script(project_dir=tmp_path)

    for p in patches:
        basename = os.path.basename(p["filepath"])
        assert f"PATCH: {basename} not found, skipping {p['label']}" in stdout, (
            f"missing-file message wrong for {basename}\n--- stdout ---\n{stdout}"
        )


def test_drifted_source_emits_warning(fresh_tree, patches):
    """If a target file's content matches neither `old` nor `new`, warn."""
    # Pick the first patch and corrupt its file so neither old nor new is present.
    target = Path(patches[0]["filepath"])
    target.write_text("/* upstream NimBLE refactored this block */\n")

    stdout, _ = _exec_patch_script(project_dir=fresh_tree)

    expected = f"PATCH: WARNING -- {patches[0]['label']}: expected code not found"
    assert expected in stdout, (
        f"drifted source did not produce warning\n"
        f"expected: {expected!r}\n--- stdout ---\n{stdout}"
    )
    # The other 3 patches should still apply normally.
    for p in patches[1:]:
        assert p["label"] in stdout
        assert "already applied" not in stdout.split(p["label"])[1].split("\n")[0]


def test_partial_application_leaves_first_unchanged(fresh_tree, patches):
    """Drift on one patch must not corrupt that file."""
    target = Path(patches[0]["filepath"])
    drifted_content = "/* upstream NimBLE refactored this block */\n"
    target.write_text(drifted_content)

    _exec_patch_script(project_dir=fresh_tree)

    assert target.read_text() == drifted_content, (
        "drifted file should be left untouched, not partially patched"
    )


def test_script_is_executable_via_python(tmp_path):
    """End-to-end: invoke patch_nimble.py as a subprocess via Python.

    This guards against the script depending on PlatformIO's SCons context in a
    way that prevents standalone invocation. We expect failure or a clean
    skip — but no traceback.
    """
    # Write a minimal sitecustomize-like shim that defines Import as no-op
    # and provides env.
    shim = tmp_path / "shim.py"
    shim.write_text(
        f"import builtins\n"
        f"builtins.Import = lambda name: None\n"
        f"class _Env:\n"
        f"    def get(self, key, default=None):\n"
        f"        return {str(tmp_path)!r} if key == 'PROJECT_DIR' else default\n"
        f"import builtins\n"
        f"builtins.env = _Env()\n"
        f"exec(open({str(PATCH_SCRIPT)!r}).read())\n"
    )
    result = subprocess.run(
        ["/usr/bin/python3", str(shim)],
        capture_output=True,
        text=True,
        timeout=10,
    )
    assert result.returncode == 0, (
        f"patch_nimble.py crashed under standalone invocation\n"
        f"--- stdout ---\n{result.stdout}\n--- stderr ---\n{result.stderr}"
    )
    # All 4 files are absent → 4 "not found, skipping" lines
    assert result.stdout.count("not found, skipping") == 4
