"""Compile and execute live location persistence-controller tests."""
from pathlib import Path

from native_test import compile_and_run

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def test_location_persistence_controller(tmp_path):
    ran = compile_and_run(
        tmp_path,
        name="test_location_persistence_controller",
        sources=[
            HERE / "test_location_persistence_controller.cpp",
            ROOT / "lib/tdeck_ui/Telemetry/LocationPersistenceController.cpp",
            ROOT / "lib/tdeck_ui/Telemetry/LocationPersistence.cpp",
            ROOT / "lib/tdeck_ui/Telemetry/LocationStateRecord.cpp",
            ROOT / "lib/tdeck_ui/Telemetry/LocationShareState.cpp",
            ROOT / "lib/tdeck_ui/Telemetry/LocationShareScheduler.cpp",
        ],
        include_dirs=[ROOT / "lib/tdeck_ui"],
        sanitize=True,
        timeout=60,
    )
    assert "0 failed" in ran.stdout
