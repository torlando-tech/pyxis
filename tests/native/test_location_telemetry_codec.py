"""Compile and execute the portable location telemetry codec tests."""

from pathlib import Path

from native_test import compile_and_run

HERE = Path(__file__).resolve().parent
PYXIS_ROOT = HERE.parent.parent


def test_location_telemetry_codec(tmp_path):
    ran = compile_and_run(
        tmp_path,
        name="test_location_telemetry_codec",
        sources=[
            HERE / "test_location_telemetry_codec.cpp",
            PYXIS_ROOT
            / "lib"
            / "tdeck_ui"
            / "Telemetry"
            / "LocationTelemetryCodec.cpp",
        ],
        include_dirs=[PYXIS_ROOT / "lib" / "tdeck_ui"],
        sanitize=True,
    )
    assert "location telemetry codec: 10 passed, 0 failed" in ran.stdout
