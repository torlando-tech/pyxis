"""Build-contract checks for the portable telemetry production sources."""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_tdeck_ui_builds_telemetry_cpp_sources():
    manifest = json.loads((ROOT / "lib" / "tdeck_ui" / "library.json").read_text())
    assert "+<Telemetry/*.cpp>" in manifest["build"]["srcFilter"]
