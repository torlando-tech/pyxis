"""Verify committed vectors byte-for-byte against authoritative Sideband."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
VECTORS = ROOT / "tests" / "fixtures" / "location_telemetry_vectors.json"
DEFAULT_SIDEBAND = Path("/tmp/pyxis-sideband-reference-20260728")
REQUIRED_VECTOR_NAMES = {
    "san_francisco",
    "bounds_signed",
    "southern",
    "zero_time_accuracy",
    "uint64_timestamps",
}
REQUIRED_VECTOR_KEYS = {
    "name", "sensor_timestamp_seconds", "latitude", "longitude",
    "altitude", "speed_kmh", "bearing", "accuracy",
    "location_timestamp_seconds", "packed_hex",
    "microlxmf_raw_value_hex", "expected_fixed",
}


def _sideband_source() -> Path:
    source = Path(os.environ.get("SIDEBAND_SRC", DEFAULT_SIDEBAND))
    if not (source / "sbapp" / "sideband" / "sense.py").is_file():
        pytest.skip("set SIDEBAND_SRC to an authoritative Sideband checkout")
    return source


def test_fixture_has_required_protocol_matrix_and_schema():
    fixture = json.loads(VECTORS.read_text())
    vectors = fixture["vectors"]
    assert {vector["name"] for vector in vectors} == REQUIRED_VECTOR_NAMES
    for vector in vectors:
        assert set(vector) == REQUIRED_VECTOR_KEYS, vector["name"]
        assert bytes.fromhex(vector["microlxmf_raw_value_hex"])[0] == 0xC4
        assert len(bytes.fromhex(vector["packed_hex"])) <= 0xFF


def test_vectors_match_pinned_sideband_commit():
    fixture = json.loads(VECTORS.read_text())
    source = _sideband_source()
    actual = subprocess.run(
        ["git", "-C", str(source), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    assert actual == fixture["reference"]["commit"]

    sys.path.insert(0, str(source))
    import importlib

    sense = importlib.import_module("sbapp.sideband.sense")
    from RNS.vendor import umsgpack

    original_time = sense.time.time
    try:
        for vector in fixture["vectors"]:
            now = vector["sensor_timestamp_seconds"]
            sense.time.time = lambda now=now: now
            telemeter = sense.Telemeter(from_packed=True)
            telemeter.synthesize("location")
            telemeter.sensors["location"].data = {
                "latitude": vector["latitude"],
                "longitude": vector["longitude"],
                "altitude": vector["altitude"],
                "speed": vector["speed_kmh"],
                "bearing": vector["bearing"],
                "accuracy": vector["accuracy"],
                "last_update": vector["location_timestamp_seconds"],
            }
            packed = telemeter.packed()
            assert packed.hex() == vector["packed_hex"], vector["name"]

            # Pinned microLXMF stores field values as raw MessagePack spans and
            # splices them with packRawBytes(). FIELD_TELEMETRY must therefore
            # be a raw BIN token decoded by Python LXMF as packed bytes.
            raw_value = bytes.fromhex(vector["microlxmf_raw_value_hex"])
            assert umsgpack.unpackb(raw_value) == packed, vector["name"]

            decoded = sense.Telemeter.from_packed(packed)
            assert decoded is not None, vector["name"]
            readings = decoded.read_all()
            assert readings["time"]["utc"] == vector["sensor_timestamp_seconds"]
            assert readings["location"]["speed"] == vector["speed_kmh"]
            assert readings["location"]["last_update"] == vector["location_timestamp_seconds"]
    finally:
        sense.time.time = original_time
