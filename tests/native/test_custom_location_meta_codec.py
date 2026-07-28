"""Compile and execute the portable custom location metadata codec tests."""

from pathlib import Path

from native_test import compile_and_run

HERE = Path(__file__).resolve().parent
PYXIS_ROOT = HERE.parent.parent


def test_custom_location_meta_codec(tmp_path):
    ran = compile_and_run(
        tmp_path,
        name="test_custom_location_meta_codec",
        sources=[
            HERE / "test_custom_location_meta_codec.cpp",
            PYXIS_ROOT / "lib" / "tdeck_ui" / "Telemetry" / "LocationTelemetryCodec.cpp",
        ],
        include_dirs=[PYXIS_ROOT / "lib" / "tdeck_ui"],
        sanitize=True,
    )
    assert "custom location metadata codec:" in ran.stdout
    assert "0 failed" in ran.stdout
