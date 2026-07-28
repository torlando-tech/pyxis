"""Compile and execute the portable location telemetry codec tests."""

from __future__ import annotations

import json
from pathlib import Path

from native_test import compile_and_run

HERE = Path(__file__).resolve().parent
PYXIS_ROOT = HERE.parent.parent
VECTORS = PYXIS_ROOT / "tests" / "fixtures" / "location_telemetry_vectors.json"


def _write_vector_header(path: Path) -> None:
    fixture = json.loads(VECTORS.read_text())
    lines = [
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "namespace Fixture {",
        "struct Vector {",
        "  const char* name; const uint8_t* packed; std::size_t packed_size;",
        "  int32_t latitude_e6; int32_t longitude_e6; int32_t altitude_cm;",
        "  uint32_t speed_centi_kmh; int32_t bearing_cdeg; uint16_t accuracy_cm;",
        "  uint64_t location_timestamp_seconds; uint64_t sensor_timestamp_seconds;",
        "};",
    ]
    for index, vector in enumerate(fixture["vectors"]):
        packed = bytes.fromhex(vector["packed_hex"])
        encoded = ", ".join(f"0x{byte:02x}" for byte in packed)
        lines.append(f"static const uint8_t PACKED_{index}[] = {{{encoded}}};")
    lines.append("static const Vector VECTORS[] = {")
    for index, vector in enumerate(fixture["vectors"]):
        fixed = vector["expected_fixed"]
        lines.append(
            "  {"
            f'"{vector["name"]}", PACKED_{index}, sizeof(PACKED_{index}), '
            f'{fixed["latitude_e6"]}, {fixed["longitude_e6"]}, '
            f'{fixed["altitude_cm"]}, {fixed["speed_centi_kmh"]}U, '
            f'{fixed["bearing_cdeg"]}, {fixed["accuracy_cm"]}U, '
            f'{vector["location_timestamp_seconds"]}ULL, '
            f'{vector["sensor_timestamp_seconds"]}ULL'
            "},"
        )
    lines.extend(
        [
            "};",
            "static const std::size_t VECTOR_COUNT = sizeof(VECTORS) / sizeof(VECTORS[0]);",
            "}  // namespace Fixture",
        ]
    )
    path.write_text("\n".join(lines) + "\n")


def test_location_telemetry_codec(tmp_path):
    _write_vector_header(tmp_path / "location_telemetry_vectors_generated.h")
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
        include_dirs=[tmp_path, PYXIS_ROOT / "lib" / "tdeck_ui"],
        sanitize=True,
    )
    assert "location telemetry codec:" in ran.stdout
    assert "0 failed" in ran.stdout
