"""Compile and execute versioned location-state record tests."""

from pathlib import Path

from native_test import compile_and_run

HERE = Path(__file__).resolve().parent
PYXIS_ROOT = HERE.parents[1]


def test_location_state_record(tmp_path):
    ran = compile_and_run(
        tmp_path,
        name="test_location_state_record",
        sources=[
            HERE / "test_location_state_record.cpp",
            PYXIS_ROOT / "lib" / "tdeck_ui" / "Telemetry" / "LocationStateRecord.cpp",
        ],
        include_dirs=[PYXIS_ROOT / "lib" / "tdeck_ui"],
        sanitize=True,
        timeout=60,
    )
    assert "0 failed" in ran.stdout
