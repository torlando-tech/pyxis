"""Compile and execute outbound location-sharing scheduler tests."""

from pathlib import Path

from native_test import compile_and_run

HERE = Path(__file__).resolve().parent
PYXIS_ROOT = HERE.parent.parent


def test_location_share_scheduler(tmp_path):
    ran = compile_and_run(
        tmp_path,
        name="test_location_share_scheduler",
        sources=[
            HERE / "test_location_share_scheduler.cpp",
            PYXIS_ROOT / "lib" / "tdeck_ui" / "Telemetry" / "LocationShareScheduler.cpp",
        ],
        include_dirs=[PYXIS_ROOT / "lib" / "tdeck_ui"],
        sanitize=True,
        timeout=60,
    )
    assert "location share scheduler:" in ran.stdout
    assert "0 failed" in ran.stdout
