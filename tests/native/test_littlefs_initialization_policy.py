"""Compile and execute the portable LittleFS initialization policy regression."""

from pathlib import Path

from native_test import compile_and_run


HERE = Path(__file__).resolve().parent
PYXIS_ROOT = HERE.parent.parent


def test_littlefs_initialization_policy(tmp_path):
    ran = compile_and_run(
        tmp_path,
        name="test_littlefs_initialization_policy",
        sources=[HERE / "test_littlefs_initialization_policy.cpp"],
        include_dirs=[PYXIS_ROOT / "lib" / "storage"],
    )
    assert "19 passed, 0 failed" in ran.stdout
