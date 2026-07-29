from pathlib import Path

from native_test import compile_and_run

ROOT = Path(__file__).resolve().parents[2]


def test_location_fix_adapter_native(tmp_path):
    result = compile_and_run(
        tmp_path,
        name="test_location_fix_adapter",
        sources=[
            ROOT / "tests/native/test_location_fix_adapter.cpp",
            ROOT / "lib/tdeck_ui/Telemetry/LocationFixAdapter.cpp",
        ],
        include_dirs=[ROOT / "lib/tdeck_ui"],
        sanitize=True,
    )
    assert "location fix adapter tests passed" in result.stdout
