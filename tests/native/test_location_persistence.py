"""Compile and execute transactional location-persistence tests."""

from pathlib import Path

from native_test import compile_and_run

HERE = Path(__file__).resolve().parent
PYXIS_ROOT = HERE.parents[1]


def test_location_persistence(tmp_path):
    ran = compile_and_run(
        tmp_path,
        name="test_location_persistence",
        sources=[
            HERE / "test_location_persistence.cpp",
            PYXIS_ROOT / "lib" / "tdeck_ui" / "Telemetry" / "LocationPersistence.cpp",
            PYXIS_ROOT / "lib" / "tdeck_ui" / "Telemetry" / "LocationStateRecord.cpp",
        ],
        include_dirs=[PYXIS_ROOT / "lib" / "tdeck_ui"],
        sanitize=True,
        timeout=60,
    )
    assert "0 failed" in ran.stdout
