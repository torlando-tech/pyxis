"""Compile and execute the fixed-capacity map view-model tests."""
from pathlib import Path

from native_test import compile_and_run

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def test_map_view_model(tmp_path):
    ran = compile_and_run(
        tmp_path,
        name="test_map_view_model",
        sources=[
            HERE / "test_map_view_model.cpp",
            ROOT / "lib/tdeck_ui/UI/LXMF/MapViewModel.cpp",
            ROOT / "lib/tdeck_ui/UI/LXMF/MapProjection.cpp",
        ],
        include_dirs=[ROOT / "lib/tdeck_ui"],
        sanitize=True,
        timeout=60,
    )
    assert "0 failed" in ran.stdout
