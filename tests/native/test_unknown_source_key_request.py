"""Compile and execute the unknown-source key-request policy test."""

from pathlib import Path

from native_test import compile_and_run

HERE = Path(__file__).resolve().parent
PYXIS_ROOT = HERE.parent.parent


def test_unknown_source_key_request(tmp_path):
    ran = compile_and_run(
        tmp_path,
        name="test_unknown_source_key_request",
        sources=[HERE / "test_unknown_source_key_request.cpp"],
        include_dirs=[PYXIS_ROOT / "lib" / "tdeck_ui"],
        sanitize=True,
        timeout=60,
    )
    assert "unknown source key request policy:" in ran.stdout
    assert "0 failed" in ran.stdout
