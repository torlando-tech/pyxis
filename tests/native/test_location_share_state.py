"""Compile and execute bounded peer location state tests."""

from pathlib import Path

from native_test import compile_and_run

HERE = Path(__file__).resolve().parent
PYXIS_ROOT = HERE.parent.parent


def test_location_share_state(tmp_path):
    ran = compile_and_run(
        tmp_path,
        name="test_location_share_state",
        sources=[
            HERE / "test_location_share_state.cpp",
            PYXIS_ROOT / "lib" / "tdeck_ui" / "Telemetry" / "LocationShareState.cpp",
        ],
        include_dirs=[PYXIS_ROOT / "lib" / "tdeck_ui"],
        sanitize=True,
        timeout=60,
    )
    assert "location share state:" in ran.stdout
    assert "0 failed" in ran.stdout
