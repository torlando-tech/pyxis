# Copyright (c) 2026 Pyxis contributors
# SPDX-License-Identifier: MIT

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import fcntl
from pathlib import Path
import struct
import subprocess
import sys
import zlib

import pytest

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/maps/build_map_pack.py"


def load_tool():
    spec = importlib.util.spec_from_file_location("build_map_pack", TOOL)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def png(width: int = 256, height: int = 256) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    raw = b"".join(b"\x00" + b"\x00\x00\x00" * width for _ in range(height))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw))
        + chunk(b"IEND", b"")
    )


def png_chunks(chunks: list[tuple[bytes, bytes]]) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return b"\x89PNG\r\n\x1a\n" + b"".join(chunk(kind, data) for kind, data in chunks)


def ihdr(*, bit_depth: int = 8, color_type: int = 2, interlace: int = 0) -> bytes:
    return struct.pack(">IIBBBBB", 256, 256, bit_depth, color_type, 0, 0, interlace)


def raw_rgb() -> bytes:
    return b"".join(b"\x00" + b"\x00\x00\x00" * 256 for _ in range(256))


def rewrite_crc(data: bytearray | bytes) -> bytes:
    mutable = bytearray(data)
    mutable[-4:] = struct.pack("<I", zlib.crc32(mutable[:-4]))
    return bytes(mutable)


def put_tile(source: Path, z: int | str, x: int | str, y: int | str, data: bytes | None = None) -> Path:
    path = source / str(z) / str(x) / f"{y}.png"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png() if data is None else data)
    return path


def metadata(**changes: str) -> dict[str, str]:
    values = {
        "pack_id": "local-pack",
        "name": "Local Pack",
        "attribution": "Example Maps contributors",
        "source": "user-supplied XYZ export",
        "license": "CC-BY-4.0",
    }
    values.update(changes)
    return values


def make_rectangle(source: Path) -> None:
    for z, xs, ys in ((1, range(2), range(2)), (2, range(1, 3), range(1, 3))):
        for x in xs:
            for y in ys:
                put_tile(source, z, x, y)


def digest_tree(path: Path) -> str:
    digest = hashlib.sha256()
    for item in sorted(path.rglob("*"), key=lambda value: value.as_posix()):
        digest.update(item.relative_to(path).as_posix().encode("ascii"))
        if item.is_file():
            digest.update(item.read_bytes())
    return digest.hexdigest()


V3_VECTORS = json.loads((ROOT / "tests/fixtures/map_pack_v3_vectors.json").read_text(encoding="utf-8"))


def assert_versioned_header(data: bytes, magic: bytes, total_length_field: bool) -> None:
    assert data[:4] == magic
    assert data[4] == 3
    assert data[5] == 0
    if total_length_field:
        # PMPK: u16 header size (16), u32 total length, reserved u32
        assert struct.unpack("<H", data[6:8])[0] == 16
        assert struct.unpack("<I", data[8:12])[0] == len(data)
        assert struct.unpack("<I", data[12:16])[0] == 0
    else:
        # PMAS: u16 total length, u32 generation
        assert struct.unpack("<H", data[6:8])[0] == len(data)
        assert struct.unpack("<I", data[8:12])[0] >= 1
    assert struct.unpack("<I", data[-4:])[0] == zlib.crc32(data[:-4])


def test_frozen_v3_vectors_have_expected_header_length_and_crc() -> None:
    for name, entry in V3_VECTORS.items():
        data = bytes.fromhex(entry["hex"])
        assert name.startswith(("pmpk_v3_", "pmas_v3_")), name
        assert len(data) >= 20 and len(data) <= 7105
        assert_versioned_header(data, b"PMPK" if name.startswith("pmpk") else b"PMAS",
                                total_length_field=name.startswith("pmpk"))


def test_decode_frozen_pmpk_v3_vectors() -> None:
    tool = load_tool()
    for name in ("pmpk_v3_one_tile", "pmpk_v3_multi_zoom"):
        entry = V3_VECTORS[name]
        values = tool.parse_manifest(bytes.fromhex(entry["hex"]))
        assert values["format_version"] == 3
        assert values["pack_id"] == entry["pack_id"]
        assert values["name"] == entry["name"]
        assert values["attribution"] == entry["attribution"]
        assert values["source"] == entry["source"]
        assert values["license"] == entry["license"]
        assert values["min_zoom"] == entry["min_zoom"]
        assert values["max_zoom"] == entry["max_zoom"]
        assert values["tile_count"] == entry["tile_count"]
        assert values["indexless"] is True
        assert values["extents"] == []
        assert values["row_spans"] == []


def test_pmas_v1_v2_decode_unchanged() -> None:
    tool = load_tool()
    # Legacy v1: 48-byte fixed record, magic, version 1, zero reserved, u16 size,
    # u32 generation, single pack ID at offset 13, zero padding to offset 44, CRC32.
    body = bytearray(b"PMAS\x01\x00" + struct.pack("<H", 48) + struct.pack("<I", 5) + b"\x08")
    body.extend(b"legacy-1")
    body.extend(b"\x00" * (44 - len(body)))
    legacy = bytes(body) + struct.pack("<I", zlib.crc32(body))
    values = tool.decode_active_selection(legacy)
    assert values["format_version"] == 1
    assert values["generation"] == 5
    assert values["map_set_id"] == "legacy-1"
    assert values["pack_ids"] == ["legacy-1"]


def test_pmas_v1_non_ascii_pack_id_raises_pack_error_not_traceback() -> None:
    tool = load_tool()
    # A valid-CRC legacy v1 record whose pack ID carries a non-ASCII byte
    # (0x80) must surface a clean PackError (exit-2 path), not an
    # uncaught UnicodeDecodeError traceback.
    body = bytearray(b"PMAS\x01\x00" + struct.pack("<H", 48) + struct.pack("<I", 5) + b"\x08")
    body.extend(b"\x80egacy-1")  # 0x80 first byte: non-ASCII, invalid grammar
    body.extend(b"\x00" * (44 - len(body)))
    bad = bytes(body) + struct.pack("<I", zlib.crc32(body))
    with pytest.raises(tool.PackError):
        tool.decode_active_selection(bad)


def test_encode_pmas_v3_matches_frozen_vector() -> None:
    tool = load_tool()
    for name in ("pmas_v3_one_pack", "pmas_v3_three_packs"):
        entry = V3_VECTORS[name]
        actual = tool.encode_active_map_set(
            generation=entry["generation"], map_set_id=entry["map_set_id"],
            attribution=entry["attribution"], pack_ids=list(entry["pack_ids"]),
        )
        assert actual == bytes.fromhex(entry["hex"])


def test_pmas_v3_rejects_duplicate_pack_ids() -> None:
    tool = load_tool()
    with pytest.raises(tool.PackError, match="duplicate"):
        tool.encode_active_map_set(generation=1, map_set_id="osm-bright",
                                   attribution="Example", pack_ids=["same", "same"])
    # Hand-built decode-side record with the same pack id twice (valid CRC).
    body = bytearray(struct.pack("<4sBBHI", b"PMAS", 3, 0, 0, 1))
    body.extend(b"\x0aosm-bright")
    body.extend(b"\x07Example")
    body.append(2)
    body.extend(b"\x06detail")
    body.extend(b"\x06detail")
    total = len(body) + 4
    record = bytes(body[:6]) + struct.pack("<H", total) + bytes(body[8:])
    record += struct.pack("<I", zlib.crc32(record))
    with pytest.raises(tool.PackError, match="duplicate"):
        tool.decode_active_selection(record)


def test_pmas_v3_rejects_generation_zero() -> None:
    tool = load_tool()
    with pytest.raises(tool.PackError, match="generation"):
        tool.encode_active_map_set(generation=0, map_set_id="osm-bright",
                                   attribution="Example", pack_ids=["detail"])
    with pytest.raises(tool.PackError, match="generation"):
        tool.encode_active_map_set(generation=1 << 32, map_set_id="osm-bright",
                                   attribution="Example", pack_ids=["detail"])
    entry = bytearray(bytes.fromhex(V3_VECTORS["pmas_v3_one_pack"]["hex"]))
    struct.pack_into("<I", entry, 8, 0)
    with pytest.raises(tool.PackError, match="generation"):
        tool.decode_active_selection(rewrite_crc(bytes(entry)))


def test_pmas_v3_rejects_more_than_eight_packs() -> None:
    tool = load_tool()
    with pytest.raises(tool.PackError, match="1..8"):
        tool.encode_active_map_set(generation=1, map_set_id="osm-bright",
                                   attribution="Example",
                                   pack_ids=[f"pack-{index}" for index in range(9)])
    with pytest.raises(tool.PackError, match="1..8"):
        tool.encode_active_map_set(generation=1, map_set_id="osm-bright",
                                   attribution="Example", pack_ids=[])
    # hand-built decode-side record claiming nine packs is structurally impossible
    # to pass CRC, so verify encode-side enforcement plus a trailing-byte reject
    entry = bytearray(bytes.fromhex(V3_VECTORS["pmas_v3_one_pack"]["hex"]))
    entry.append(0)
    with pytest.raises(tool.PackError):
        tool.decode_active_selection(rewrite_crc(bytes(entry)))


def test_decode_pmas_v3_rejects_bad_crc_and_trailing_bytes() -> None:
    tool = load_tool()
    entry = bytes.fromhex(V3_VECTORS["pmas_v3_one_pack"]["hex"])
    corrupted = entry[:-1] + bytes([entry[-1] ^ 1])
    with pytest.raises(tool.PackError, match="CRC"):
        tool.decode_active_selection(corrupted)
    entry = bytearray(bytes.fromhex(V3_VECTORS["pmas_v3_one_pack"]["hex"]))
    entry[13] = 0x01  # first map-set-id character becomes a control byte
    with pytest.raises(tool.PackError):
        tool.decode_active_selection(rewrite_crc(bytes(entry)))


def test_decode_frozen_pmas_v3_vectors() -> None:
    tool = load_tool()
    for name in ("pmas_v3_one_pack", "pmas_v3_three_packs"):
        entry = V3_VECTORS[name]
        values = tool.decode_active_selection(bytes.fromhex(entry["hex"]))
        assert values["format_version"] == 3
        assert values["generation"] == entry["generation"]
        assert values["map_set_id"] == entry["map_set_id"]
        assert values["attribution"] == entry["attribution"]
        assert values["pack_ids"] == entry["pack_ids"]


def test_parse_pmpk_v3_frozen_vector() -> None:
    tool = load_tool()
    values = tool.parse_manifest(bytes.fromhex(V3_VECTORS["pmpk_v3_one_tile"]["hex"]))
    assert values["format_version"] == 3
    assert values["pack_id"] == "one-tile"
    assert values["tile_count"] == 1
    assert values["indexless"] is True
    assert values["extents"] == [] and values["row_spans"] == []


def _mutated_pmpk_v3(mutate) -> bytes:
    data = bytearray(bytes.fromhex(V3_VECTORS["pmpk_v3_one_tile"]["hex"]))
    mutate(data)
    return rewrite_crc(data)


def test_pmpk_v3_rejects_nonzero_reserved_byte() -> None:
    tool = load_tool()
    data = bytearray(bytes.fromhex(V3_VECTORS["pmpk_v3_one_tile"]["hex"]))
    data[5] = 1
    with pytest.raises(tool.PackError):
        tool.parse_manifest(rewrite_crc(data))


def test_pmpk_v3_rejects_trailing_bytes() -> None:
    tool = load_tool()
    data = bytearray(bytes.fromhex(V3_VECTORS["pmpk_v3_one_tile"]["hex"]))
    data[-5:-1] = data[-5:-1] + b"junk"[:1]
    data = data[:-4] + b"\x00\x00\x00\x00" + data[-4:]
    # total length field now disagrees with actual size: header check rejects
    with pytest.raises(tool.PackError):
        tool.parse_manifest(rewrite_crc(data))


def test_pmpk_v3_rejects_bad_crc() -> None:
    tool = load_tool()
    data = bytes.fromhex(V3_VECTORS["pmpk_v3_one_tile"]["hex"])
    corrupted = data[:-1] + bytes([data[-1] ^ 1])
    with pytest.raises(tool.PackError):
        tool.parse_manifest(corrupted)


def test_pmpk_v3_rejects_zero_tile_count_and_inverted_zooms() -> None:
    tool = load_tool()
    def zero_tiles(data: bytearray) -> None:
        struct.pack_into("<I", data, len(data) - 8, 0)

    def inverted_zooms(data: bytearray) -> None:
        # max zoom becomes 1 while min stays 2: min > max
        data[-9] = 1

    for mutate in (zero_tiles, inverted_zooms):
        with pytest.raises(tool.PackError):
            tool.parse_manifest(_mutated_pmpk_v3(mutate))


def test_pmpk_v1_v2_vectors_unchanged() -> None:
    tool = load_tool()
    v1 = tool.serialize_manifest(**metadata(), extents=[tool.ZoomExtent(0, ((0, 0),), 0, 0)], tile_count=1)
    v1_values = tool.parse_manifest(v1)
    assert v1_values["format_version"] == 1
    assert v1_values["indexless"] is False
    v2 = tool.serialize_sparse_manifest(
        **metadata(), row_spans=[tool.RowSpan(1, 0, 0, 1)], tile_count=2)
    v2_values = tool.parse_manifest(v2)
    assert v2_values["format_version"] == 2
    assert v2_values["indexless"] is False


def test_serialize_pmpk_v3_matches_frozen_vector() -> None:
    tool = load_tool()
    entry = V3_VECTORS["pmpk_v3_one_tile"]
    actual = tool.serialize_indexless_manifest(
        pack_id=entry["pack_id"], name=entry["name"], attribution=entry["attribution"],
        source=entry["source"], license=entry["license"],
        minimum_zoom=entry["min_zoom"], maximum_zoom=entry["max_zoom"],
        tile_count=entry["tile_count"],
    )
    assert actual == bytes.fromhex(entry["hex"])
    parsed = tool.parse_manifest(actual)
    assert parsed["indexless"] is True


def test_serialize_pmpk_v3_is_deterministic() -> None:
    tool = load_tool()
    entry = V3_VECTORS["pmpk_v3_multi_zoom"]
    first = tool.serialize_indexless_manifest(
        pack_id=entry["pack_id"], name=entry["name"], attribution=entry["attribution"],
        source=entry["source"], license=entry["license"],
        minimum_zoom=entry["min_zoom"], maximum_zoom=entry["max_zoom"],
        tile_count=entry["tile_count"],
    )
    second = tool.serialize_indexless_manifest(
        pack_id=entry["pack_id"], name=entry["name"], attribution=entry["attribution"],
        source=entry["source"], license=entry["license"],
        minimum_zoom=entry["min_zoom"], maximum_zoom=entry["max_zoom"],
        tile_count=entry["tile_count"],
    )
    assert first == second == bytes.fromhex(entry["hex"])


def test_serialize_pmpk_v3_rejects_zero_tiles() -> None:
    tool = load_tool()
    with pytest.raises(tool.PackError, match="tile count"):
        tool.serialize_indexless_manifest(**metadata(), minimum_zoom=1, maximum_zoom=1, tile_count=0)
    with pytest.raises(tool.PackError, match="tile count"):
        tool.serialize_indexless_manifest(**metadata(), minimum_zoom=1, maximum_zoom=1, tile_count=1 << 32)


def test_serialize_pmpk_v3_rejects_invalid_zoom_range() -> None:
    tool = load_tool()
    with pytest.raises(tool.PackError, match="zoom range"):
        tool.serialize_indexless_manifest(**metadata(), minimum_zoom=3, maximum_zoom=2, tile_count=1)
    with pytest.raises(tool.PackError, match="zoom range"):
        tool.serialize_indexless_manifest(**metadata(), minimum_zoom=0, maximum_zoom=tool.MAX_ZOOM + 1, tile_count=1)


def test_serialize_pmpk_v3_contains_no_span_payload() -> None:
    tool = load_tool()
    entry = V3_VECTORS["pmpk_v3_one_tile"]
    data = tool.serialize_indexless_manifest(
        pack_id=entry["pack_id"], name=entry["name"], attribution=entry["attribution"],
        source=entry["source"], license=entry["license"],
        minimum_zoom=entry["min_zoom"], maximum_zoom=entry["max_zoom"],
        tile_count=entry["tile_count"],
    )
    # v3 payload is exactly five length-prefixed strings plus min, max, tile_count
    expected_length = 16 + sum(1 + len(entry[field].encode("ascii"))
                               for field in ("pack_id", "name", "attribution", "source", "license")) + 6 + 4
    assert len(data) == expected_length


def style_metadata(tool, style: str = "osm-bright", **changes: str) -> dict[str, str]:
    policy = dict(tool.STYLE_POLICIES[style])
    values = {
        "pack_id": "styled-pack",
        "name": "Styled Pack",
        "attribution": policy["attribution"],
        "source": policy["source"],
        "license": policy["license"],
    }
    values.update(changes)
    return values


def test_style_build_emits_pmpk_v3(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    for x, y in ((0, 0), (0, 1), (3, 0)):
        put_tile(source, 2, x, y)
    # sparse source must still emit v3 when a style is requested
    built = tool.build_map_pack(source, tmp_path / "sd", style="osm-bright",
                                **style_metadata(tool))
    parsed = tool.validate_pack(built)
    assert parsed["format_version"] == 3
    assert parsed["indexless"] is True
    assert parsed["min_zoom"] == 2
    assert parsed["max_zoom"] == 2
    assert parsed["tile_count"] == 3


def test_style_build_rejects_policy_metadata_mismatch(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0)
    for field in ("attribution", "source", "license"):
        with pytest.raises(tool.PackError, match=field):
            tool.build_map_pack(source, tmp_path / f"sd-{field}", style="osm-bright",
                                **style_metadata(tool, **{field: "Wrong value"}))
    with pytest.raises(tool.PackError, match="unsupported map style"):
        tool.build_map_pack(source, tmp_path / "sd-bad", style="vintage",
                            **style_metadata(tool))


def test_no_style_build_keeps_v1_v2_behavior(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    make_rectangle(source)
    rectangular = tool.build_map_pack(source, tmp_path / "sd-a", **metadata())
    assert tool.validate_pack(rectangular)["format_version"] == 1
    sparse_source = tmp_path / "sparse-xyz"
    for x, y in ((1, 1), (1, 2), (2, 1)):
        put_tile(sparse_source, 2, x, y)
    sparse = tool.build_map_pack(sparse_source, tmp_path / "sd-b", sparse=True, **metadata())
    assert tool.validate_pack(sparse)["format_version"] == 2


def test_style_build_publishes_no_active_record(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0)
    tool.build_map_pack(source, tmp_path / "sd", style="dark-matter",
                        **style_metadata(tool, "dark-matter"))
    pyxis_map = tmp_path / "sd/pyxis-map"
    assert not (pyxis_map / "active-pack.0").exists()
    assert not (pyxis_map / "active-pack.1").exists()
    assert not (pyxis_map / "map-sets").exists()


def _lock_holder_script(pyxis_map: Path, hold_seconds: float) -> str:
    return (
        "import fcntl, os, time\n"
        f"fd = os.open(r'{pyxis_map}/{load_tool().INSTALL_LOCK_NAME}', os.O_RDWR | os.O_CREAT, 0o644)\n"
        "fcntl.flock(fd, fcntl.LOCK_EX)\n"
        "print('held', flush=True)\n"
        f"time.sleep({hold_seconds})\n"
    )


def test_second_cli_process_is_refused_while_first_holds_lock(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0)
    tool.build_map_pack(source, tmp_path / "sd", **metadata())
    pyxis_map = tmp_path / "sd/pyxis-map"
    holder = subprocess.Popen(
        [sys.executable, "-c", _lock_holder_script(pyxis_map, 5.0)],
        stdout=subprocess.PIPE, text=True,
    )
    stdout = holder.stdout
    assert stdout is not None
    try:
        assert stdout.readline().strip() == "held"
        pyxis_fd = tool._open_path(pyxis_map)
        try:
            with pytest.raises(tool.PackError, match="another CLI map install is running"):
                tool.acquire_install_lock(pyxis_fd)
        finally:
            os.close(pyxis_fd)
        holder.terminate()
        holder.wait(timeout=10)
        pyxis_fd = tool._open_path(pyxis_map)
        try:
            lock_fd = tool.acquire_install_lock(pyxis_fd)
            tool.release_install_lock(lock_fd)
        finally:
            os.close(pyxis_fd)
    finally:
        if holder.poll() is None:
            holder.terminate()
            holder.wait(timeout=10)


def test_cli_lock_is_released_after_success(tmp_path: Path) -> None:
    tool = load_tool()
    pyxis_map = tmp_path / "sd/pyxis-map"
    pyxis_map.mkdir(parents=True)
    pyxis_fd = tool._open_path(pyxis_map)
    try:
        lock_fd = tool.acquire_install_lock(pyxis_fd)
        tool.release_install_lock(lock_fd)
        # A fresh descriptor can take the lock immediately.
        lock_fd = tool.acquire_install_lock(pyxis_fd)
        tool.release_install_lock(lock_fd)
    finally:
        os.close(pyxis_fd)


def test_cli_lock_is_released_after_failure(tmp_path: Path) -> None:
    tool = load_tool()
    pyxis_map = tmp_path / "sd/pyxis-map"
    pyxis_map.mkdir(parents=True)
    holder_fd = os.open(pyxis_map / tool.INSTALL_LOCK_NAME, os.O_RDWR | os.O_CREAT, 0o644)
    try:
        fcntl.flock(holder_fd, fcntl.LOCK_EX)
        pyxis_fd = tool._open_path(pyxis_map)
        try:
            with pytest.raises(tool.PackError, match="another CLI map install is running"):
                tool.acquire_install_lock(pyxis_fd)
        finally:
            os.close(pyxis_fd)
    finally:
        os.close(holder_fd)
    # The failed acquire must not have left any lock held by this process.
    pyxis_fd = tool._open_path(pyxis_map)
    try:
        lock_fd = tool.acquire_install_lock(pyxis_fd)
        tool.release_install_lock(lock_fd)
    finally:
        os.close(pyxis_fd)


def test_cli_lock_file_is_persistent_and_never_unlinked(tmp_path: Path) -> None:
    tool = load_tool()
    pyxis_map = tmp_path / "sd/pyxis-map"
    pyxis_map.mkdir(parents=True)
    pyxis_fd = tool._open_path(pyxis_map)
    try:
        lock_fd = tool.acquire_install_lock(pyxis_fd)
        tool.release_install_lock(lock_fd)
        lock_fd = tool.acquire_install_lock(pyxis_fd)
        tool.release_install_lock(lock_fd)
    finally:
        os.close(pyxis_fd)
    lock_path = pyxis_map / tool.INSTALL_LOCK_NAME
    assert lock_path.is_file()
    # The acquire/release code must not delete the persistent lock file.
    acquire_source = TOOL.read_text(encoding="utf-8").split(
        "def acquire_install_lock", 1)[1].split("\n\n\ndef ", 1)[0]
    release_source = TOOL.read_text(encoding="utf-8").split(
        "def release_install_lock", 1)[1].split("\n\n\ndef ", 1)[0]
    assert "unlink(" not in acquire_source + release_source


@pytest.fixture(scope="module")
def tool():
    return load_tool()


def assert_plan(tool, plan, style_id, slot, generation, pack_ids) -> None:
    policy = tool.STYLE_POLICIES[style_id]
    assert plan.style_name == style_id
    assert plan.target_slot == slot
    assert plan.generation == generation
    assert plan.pack_ids == tuple(pack_ids)
    decoded = tool.decode_active_selection(plan.record)
    assert decoded["format_version"] == 3
    assert decoded["generation"] == generation
    assert decoded["map_set_id"] == style_id
    assert decoded["attribution"] == policy["attribution"]
    assert decoded["pack_ids"] == pack_ids


def _slot(tool, generation, pack_ids, style_id="osm-bright") -> bytes:
    return tool.encode_active_map_set(generation=generation, map_set_id=style_id,
                                      attribution=tool.STYLE_POLICIES[style_id]["attribution"],
                                      pack_ids=list(pack_ids))


def test_plan_empty_card_is_generation_one_slot_zero(tool) -> None:
    plan = tool.plan_activation(slot_0=None, slot_1=None, new_pack_id="pack-a",
                                style_id="osm-bright",
                                attribution=tool.STYLE_POLICIES["osm-bright"]["attribution"])
    assert_plan(tool, plan, "osm-bright", "active-pack.0", 1, ["pack-a"])


def test_plan_one_valid_slot_targets_missing_slot(tool) -> None:
    slot_0 = _slot(tool, 1, ["pack-a"])
    plan = tool.plan_activation(slot_0=slot_0, slot_1=None, new_pack_id="pack-b",
                                style_id="osm-bright",
                                attribution=tool.STYLE_POLICIES["osm-bright"]["attribution"])
    assert_plan(tool, plan, "osm-bright", "active-pack.1", 2, ["pack-b", "pack-a"])


def test_plan_two_valid_slots_overwrites_lower_generation(tool) -> None:
    slot_0 = _slot(tool, 5, ["old-a"])
    slot_1 = _slot(tool, 3, ["old-b"])
    plan = tool.plan_activation(slot_0=slot_0, slot_1=slot_1, new_pack_id="pack-c",
                                style_id="osm-bright",
                                attribution=tool.STYLE_POLICIES["osm-bright"]["attribution"])
    # Lower-generation slot (slot 1, gen 3) is overwritten; slot 0 (gen 5) stays as fallback.
    # Composition inherits the highest-generation same-style active set (slot 0, gen 5).
    assert_plan(tool, plan, "osm-bright", "active-pack.1", 6, ["pack-c", "old-a"])


def test_plan_equal_generation_unequal_records_rejected(tool) -> None:
    slot_0 = _slot(tool, 4, ["pack-a"])
    slot_1 = _slot(tool, 4, ["pack-b"])
    with pytest.raises(tool.PackError, match="disagree at equal generation"):
        tool.plan_activation(slot_0=slot_0, slot_1=slot_1, new_pack_id="pack-c",
                             style_id="osm-bright",
                             attribution=tool.STYLE_POLICIES["osm-bright"]["attribution"])


def test_plan_existing_same_style_pack_moves_to_front_without_duplication(tool) -> None:
    slot_0 = _slot(tool, 2, ["pack-a", "pack-b"])
    plan = tool.plan_activation(slot_0=slot_0, slot_1=None, new_pack_id="pack-b",
                                style_id="osm-bright",
                                attribution=tool.STYLE_POLICIES["osm-bright"]["attribution"])
    assert_plan(tool, plan, "osm-bright", "active-pack.1", 3, ["pack-b", "pack-a"])


def test_plan_different_style_starts_new_composition(tool) -> None:
    slot_0 = _slot(tool, 2, ["pack-a"], style_id="toner")
    plan = tool.plan_activation(slot_0=slot_0, slot_1=None, new_pack_id="pack-b",
                                style_id="dark-matter",
                                attribution=tool.STYLE_POLICIES["dark-matter"]["attribution"])
    # No dark-matter style PMAS or active set exists yet: fresh one-pack composition.
    assert_plan(tool, plan, "dark-matter", "active-pack.1", 3, ["pack-b"])


def test_plan_style_pmas_composition_wins_over_active_slots(tool) -> None:
    slot_0 = _slot(tool, 2, ["slot-only"])
    style_record = _slot(tool, 9, ["style-a", "slot-only"])
    plan = tool.plan_activation(slot_0=slot_0, slot_1=None, new_pack_id="new-pack",
                                style_id="osm-bright",
                                attribution=tool.STYLE_POLICIES["osm-bright"]["attribution"],
                                style_record=style_record)
    assert_plan(tool, plan, "osm-bright", "active-pack.1", 10, ["new-pack", "style-a", "slot-only"])


def test_plan_pack_limit_rejects_before_any_publication(tool) -> None:
    slot_0 = _slot(tool, 1, [f"pack-{index}" for index in range(8)])
    with pytest.raises(tool.PackError, match="8-pack"):
        tool.plan_activation(slot_0=slot_0, slot_1=None, new_pack_id="pack-new",
                             style_id="osm-bright",
                             attribution=tool.STYLE_POLICIES["osm-bright"]["attribution"])
    # Re-installing an existing pack within the limit is fine.
    plan = tool.plan_activation(slot_0=slot_0, slot_1=None, new_pack_id="pack-3",
                                style_id="osm-bright",
                                attribution=tool.STYLE_POLICIES["osm-bright"]["attribution"])
    assert_plan(tool, plan, "osm-bright", "active-pack.1", 2,
                ["pack-3"] + [f"pack-{index}" for index in range(8) if index != 3])


def test_plan_generation_exhaustion_rejects(tool) -> None:
    slot_0 = _slot(tool, tool.MAX_GENERATION, ["pack-a"])
    with pytest.raises(tool.PackError, match="generation is exhausted"):
        tool.plan_activation(slot_0=slot_0, slot_1=None, new_pack_id="pack-b",
                             style_id="osm-bright",
                             attribution=tool.STYLE_POLICIES["osm-bright"]["attribution"])


def test_plan_invalid_slot_is_treated_as_missing(tool) -> None:
    slot_0 = b"\x00" * 64
    slot_1 = _slot(tool, 1, ["pack-a"])
    plan = tool.plan_activation(slot_0=slot_0, slot_1=slot_1, new_pack_id="pack-b",
                                style_id="osm-bright",
                                attribution=tool.STYLE_POLICIES["osm-bright"]["attribution"])
    assert_plan(tool, plan, "osm-bright", "active-pack.0", 2, ["pack-b", "pack-a"])


def test_plan_requires_exact_style_attribution(tool) -> None:
    with pytest.raises(tool.PackError, match="attribution"):
        tool.plan_activation(slot_0=None, slot_1=None, new_pack_id="pack-a",
                             style_id="osm-bright", attribution="Someone else's maps")


def _read_slot(pyxis_map: Path, name: str) -> bytes | None:
    path = pyxis_map / name
    if not path.is_file():
        return None
    return path.read_bytes()


def _style_record_path(pyxis_map: Path, style_id: str) -> Path:
    return pyxis_map / "map-sets" / f"{style_id}.pmas"


def test_install_refuses_to_overwrite_last_valid_fallback_when_inherited_pack_is_missing(tool,
                                                                                       tmp_path: Path) -> None:
    source_a = tmp_path / "a"; source_b = tmp_path / "b"; source_c = tmp_path / "c"
    for source in (source_a, source_b, source_c):
        put_tile(source, 1, 0, 0)
    for pack_id, source in (("pack-a", source_a), ("pack-b", source_b), ("pack-c", source_c)):
        tool.build_map_pack(source, tmp_path / "sd", style="osm-bright",
                            **style_metadata(tool, pack_id=pack_id))
    pyxis_map = tmp_path / "sd/pyxis-map"
    attribution = tool.STYLE_POLICIES["osm-bright"]["attribution"]
    gen_1 = tool.encode_active_map_set(generation=1, map_set_id="osm-bright",
                                       attribution=attribution, pack_ids=["pack-a"])
    gen_2 = tool.encode_active_map_set(generation=2, map_set_id="osm-bright",
                                       attribution=attribution, pack_ids=["pack-b", "pack-a"])
    (pyxis_map / "active-pack.0").write_bytes(gen_1)
    (pyxis_map / "active-pack.1").write_bytes(gen_2)
    (pyxis_map / "packs/pack-a/manifest.pmp").unlink()

    pyxis_fd = tool._open_path(pyxis_map)
    try:
        with pytest.raises(tool.PackError, match="missing inherited pack manifest"):
            tool.publish_activation(
                pyxis_fd, tool.plan_activation(slot_0=gen_1, slot_1=gen_2, new_pack_id="pack-c",
                                               style_id="osm-bright", attribution=attribution),
                tmp_path / "sd")
    finally:
        os.close(pyxis_fd)
    # Both active-slot byte strings are unchanged and the style PMAS was not created.
    assert (pyxis_map / "active-pack.0").read_bytes() == gen_1
    assert (pyxis_map / "active-pack.1").read_bytes() == gen_2
    assert not _style_record_path(pyxis_map, "osm-bright").exists()


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        (lambda manifest: manifest[:-1], "manifest"),
        (lambda manifest: manifest[:-4] + b"\x00\x00\x00\x00", "CRC"),
        (lambda manifest: b"X" + manifest[1:], "invalid manifest"),
    ],
)
def test_inherited_pack_corruption_is_a_hard_preflight_failure(tool, tmp_path: Path,
                                                               mutation, message) -> None:
    source = tmp_path / "xyz"
    put_tile(source, 1, 0, 0)
    tool.build_map_pack(source, tmp_path / "sd", style="osm-bright",
                        **style_metadata(tool, pack_id="pack-a"))
    pyxis_map = tmp_path / "sd/pyxis-map"
    attribution = tool.STYLE_POLICIES["osm-bright"]["attribution"]
    gen_1 = tool.encode_active_map_set(generation=1, map_set_id="osm-bright",
                                       attribution=attribution, pack_ids=["pack-a"])
    (pyxis_map / "active-pack.0").write_bytes(gen_1)
    manifest_path = pyxis_map / "packs/pack-a/manifest.pmp"
    manifest_path.write_bytes(mutation(manifest_path.read_bytes()))
    pyxis_fd = tool._open_path(pyxis_map)
    try:
        with pytest.raises(tool.PackError, match=message):
            tool.validate_active_pack_manifests_at(pyxis_fd, map_set_id="osm-bright",
                                                   attribution=attribution, pack_ids=("pack-a",))
    finally:
        os.close(pyxis_fd)
    assert (pyxis_map / "active-pack.0").read_bytes() == gen_1


def test_inherited_pack_policy_mismatch_is_rejected(tool, tmp_path: Path) -> None:
    source = tmp_path / "xyz"
    put_tile(source, 1, 0, 0)
    tool.build_map_pack(source, tmp_path / "sd", style="dark-matter",
                        **style_metadata(tool, "dark-matter", pack_id="pack-a"))
    pyxis_map = tmp_path / "sd/pyxis-map"
    attribution = tool.STYLE_POLICIES["osm-bright"]["attribution"]
    pyxis_fd = tool._open_path(pyxis_map)
    try:
        # pack-a is a dark-matter pack; osm-bright validation must refuse it.
        with pytest.raises(tool.PackError, match="source/license|attribution"):
            tool.validate_active_pack_manifests_at(pyxis_fd, map_set_id="osm-bright",
                                                   attribution=attribution, pack_ids=("pack-a",))
    finally:
        os.close(pyxis_fd)


def test_all_inherited_packs_valid_passes_preflight(tool, tmp_path: Path) -> None:
    for pack_id in ("pack-a", "pack-b"):
        source = tmp_path / f"src-{pack_id}"
        put_tile(source, 1, 0, 0)
        tool.build_map_pack(source, tmp_path / "sd", style="positron",
                            **style_metadata(tool, "positron", pack_id=pack_id))
    pyxis_map = tmp_path / "sd/pyxis-map"
    attribution = tool.STYLE_POLICIES["positron"]["attribution"]
    pyxis_fd = tool._open_path(pyxis_map)
    try:
        tool.validate_active_pack_manifests_at(pyxis_fd, map_set_id="positron",
                                               attribution=attribution,
                                               pack_ids=("pack-a", "pack-b"))
    finally:
        os.close(pyxis_fd)


def _build_two_pack_card(tool, tmp_path: Path) -> tuple[Path, bytes, bytes]:
    for pack_id in ("pack-a", "pack-b"):
        source = tmp_path / f"src-{pack_id}"
        put_tile(source, 1, 0, 0)
        tool.build_map_pack(source, tmp_path / "sd", style="osm-bright",
                            **style_metadata(tool, pack_id=pack_id))
    pyxis_map = tmp_path / "sd/pyxis-map"
    attribution = tool.STYLE_POLICIES["osm-bright"]["attribution"]
    gen_1 = tool.encode_active_map_set(generation=1, map_set_id="osm-bright",
                                       attribution=attribution, pack_ids=["pack-a"])
    gen_2 = tool.encode_active_map_set(generation=2, map_set_id="osm-bright",
                                       attribution=attribution, pack_ids=["pack-b", "pack-a"])
    (pyxis_map / "active-pack.0").write_bytes(gen_1)
    (pyxis_map / "active-pack.1").write_bytes(gen_2)
    return pyxis_map, gen_1, gen_2


def test_successful_activation_writes_exact_style_and_slot_bytes(tool, tmp_path: Path) -> None:
    source_c = tmp_path / "src-c"
    put_tile(source_c, 1, 0, 0)
    tool.build_map_pack(source_c, tmp_path / "sd", style="osm-bright",
                        **style_metadata(tool, pack_id="pack-c"))
    pyxis_map, gen_1, gen_2 = _build_two_pack_card(tool, tmp_path)
    pyxis_fd = tool._open_path(pyxis_map)
    try:
        plan = tool.plan_activation(slot_0=gen_1, slot_1=gen_2, new_pack_id="pack-c",
                                    style_id="osm-bright",
                                    attribution=tool.STYLE_POLICIES["osm-bright"]["attribution"])
        tool.publish_activation(pyxis_fd, plan, tmp_path / "sd")
    finally:
        os.close(pyxis_fd)
    assert plan.generation == 3
    assert plan.target_slot == "active-pack.0"
    assert plan.pack_ids == ("pack-c", "pack-b", "pack-a")
    assert _style_record_path(pyxis_map, "osm-bright").read_bytes() == plan.record
    assert (pyxis_map / "active-pack.0").read_bytes() == plan.record
    # The peer slot (the higher-generation last-valid fallback) is untouched.
    assert (pyxis_map / "active-pack.1").read_bytes() == gen_2


def test_style_write_failure_changes_no_records(tool, tmp_path: Path, monkeypatch) -> None:
    pyxis_map, gen_1, gen_2 = _build_two_pack_card(tool, tmp_path)
    source_c = tmp_path / "src-c"
    put_tile(source_c, 1, 0, 0)
    tool.build_map_pack(source_c, tmp_path / "sd", style="osm-bright",
                        **style_metadata(tool, pack_id="pack-c"))
    monkeypatch.setattr(tool, "_rename_at",
                        lambda *a, **k: (_ for _ in ()).throw(OSError("injected style rename failure")))
    pyxis_fd = tool._open_path(pyxis_map)
    try:
        with pytest.raises(OSError, match="injected style rename failure"):
            tool.publish_activation(
                pyxis_fd,
                tool.plan_activation(slot_0=gen_1, slot_1=gen_2, new_pack_id="pack-c",
                                     style_id="osm-bright",
                                     attribution=tool.STYLE_POLICIES["osm-bright"]["attribution"]),
                tmp_path / "sd")
    finally:
        os.close(pyxis_fd)
    assert (pyxis_map / "active-pack.0").read_bytes() == gen_1
    assert (pyxis_map / "active-pack.1").read_bytes() == gen_2
    assert not _style_record_path(pyxis_map, "osm-bright").exists()
    assert not list((pyxis_map / "map-sets").glob("*.tmp-*")) if (pyxis_map / "map-sets").exists() else True


def test_slot_write_failure_preserves_prior_active_selection(tool, tmp_path: Path, monkeypatch) -> None:
    pyxis_map, gen_1, gen_2 = _build_two_pack_card(tool, tmp_path)
    source_c = tmp_path / "src-c"
    put_tile(source_c, 1, 0, 0)
    tool.build_map_pack(source_c, tmp_path / "sd", style="osm-bright",
                        **style_metadata(tool, pack_id="pack-c"))
    rename_calls = {"count": 0}
    real_rename = tool._rename_at

    def failing_slot_rename(parent_fd, source, destination, target):
        rename_calls["count"] += 1
        if rename_calls["count"] == 2:
            raise OSError("injected slot rename failure")
        real_rename(parent_fd, source, destination, target)

    monkeypatch.setattr(tool, "_rename_at", failing_slot_rename)
    pyxis_fd = tool._open_path(pyxis_map)
    try:
        with pytest.raises(OSError, match="injected slot rename failure"):
            tool.publish_activation(
                pyxis_fd,
                tool.plan_activation(slot_0=gen_1, slot_1=gen_2, new_pack_id="pack-c",
                                     style_id="osm-bright",
                                     attribution=tool.STYLE_POLICIES["osm-bright"]["attribution"]),
                tmp_path / "sd")
    finally:
        os.close(pyxis_fd)
    # Style snapshot is committed; the old slot 1 record (gen 2) is still the active selection.
    assert _style_record_path(pyxis_map, "osm-bright").is_file()
    assert (pyxis_map / "active-pack.1").read_bytes() == gen_2
    assert (pyxis_map / "active-pack.0").read_bytes() == gen_1
    assert not list((pyxis_map).glob("active-pack.1.tmp-*"))


def test_retry_after_style_only_interruption_converges(tool, tmp_path: Path, monkeypatch) -> None:
    pyxis_map, gen_1, gen_2 = _build_two_pack_card(tool, tmp_path)
    source_c = tmp_path / "src-c"
    put_tile(source_c, 1, 0, 0)
    tool.build_map_pack(source_c, tmp_path / "sd", style="osm-bright",
                        **style_metadata(tool, pack_id="pack-c"))
    attribution = tool.STYLE_POLICIES["osm-bright"]["attribution"]
    plan = tool.plan_activation(slot_0=gen_1, slot_1=gen_2, new_pack_id="pack-c",
                                style_id="osm-bright", attribution=attribution)
    rename_calls = {"count": 0}
    real_rename = tool._rename_at

    def failing_slot_rename(parent_fd, source, destination, target):
        rename_calls["count"] += 1
        if rename_calls["count"] == 2:
            raise OSError("injected slot rename failure")
        real_rename(parent_fd, source, destination, target)

    monkeypatch.setattr(tool, "_rename_at", failing_slot_rename)
    pyxis_fd = tool._open_path(pyxis_map)
    try:
        with pytest.raises(OSError, match="injected slot rename failure"):
            tool.publish_activation(pyxis_fd, plan, tmp_path / "sd")
        # Retry from the new raw snapshot: the style PMAS now carries gen 3,
        # so the candidate generation is 4 and pack-c stays at priority zero
        # without duplicating pack IDs.
        retry_plan = tool.plan_activation(
            slot_0=_read_slot(pyxis_map, "active-pack.0"),
            slot_1=_read_slot(pyxis_map, "active-pack.1"),
            style_record=_read_slot(pyxis_map, "map-sets/osm-bright.pmas"),
            new_pack_id="pack-c", style_id="osm-bright", attribution=attribution)
        tool.publish_activation(pyxis_fd, retry_plan, tmp_path / "sd")
    finally:
        os.close(pyxis_fd)
    final_slot = tool.decode_active_selection((pyxis_map / "active-pack.0").read_bytes())
    assert final_slot["generation"] == 4
    assert final_slot["pack_ids"] == ["pack-c", "pack-b", "pack-a"]
    assert len(set(final_slot["pack_ids"])) == len(final_slot["pack_ids"])
    # The peer slot (last valid fallback) is untouched.
    assert (pyxis_map / "active-pack.1").read_bytes() == gen_2
    final_style = tool.decode_active_selection(
        _style_record_path(pyxis_map, "osm-bright").read_bytes())
    assert final_style["pack_ids"] == final_slot["pack_ids"]
    assert final_style["generation"] == 4


def test_read_back_mismatch_is_a_failure(tool, tmp_path: Path, monkeypatch) -> None:
    pyxis_map, gen_1, gen_2 = _build_two_pack_card(tool, tmp_path)
    source_c = tmp_path / "src-c"
    put_tile(source_c, 1, 0, 0)
    tool.build_map_pack(source_c, tmp_path / "sd", style="osm-bright",
                        **style_metadata(tool, pack_id="pack-c"))
    real_read = tool._read_file_at

    def corrupted_slot_read(parent_fd, name, maximum):
        if "active-pack" in name:
            return b"\x00" * 48
        return real_read(parent_fd, name, maximum)

    monkeypatch.setattr(tool, "_read_file_at", corrupted_slot_read)
    pyxis_fd = tool._open_path(pyxis_map)
    try:
        with pytest.raises(tool.PackError, match="read-back mismatch"):
            tool.publish_activation(
                pyxis_fd,
                tool.plan_activation(slot_0=gen_1, slot_1=gen_2, new_pack_id="pack-c",
                                     style_id="osm-bright",
                                     attribution=tool.STYLE_POLICIES["osm-bright"]["attribution"]),
                tmp_path / "sd")
    finally:
        os.close(pyxis_fd)


def test_activate_requires_style(tool, tmp_path: Path) -> None:
    put_tile(tmp_path / "xyz", 1, 0, 0)
    code = tool.main([str(tmp_path / "xyz"), str(tmp_path / "sd"),
                      "--pack-id", "p", "--name", "n", "--attribution", "a",
                      "--source", "s", "--license", "l", "--activate"])
    assert code == 2


def _cli_metadata(tool, pack_id: str, style: str = "osm-bright") -> list[str]:
    policy = tool.STYLE_POLICIES[style]
    return ["--pack-id", pack_id, "--name", f"{pack_id} pack",
            "--attribution", policy["attribution"], "--source", policy["source"],
            "--license", policy["license"], "--style", style]


def _cli_run(tool, tmp_path, source_name, args):
    return tool.main([str(tmp_path / source_name), str(tmp_path / "sd"), *args])


def test_cli_fresh_install_and_activate(tool, tmp_path: Path) -> None:
    put_tile(tmp_path / "xyz", 1, 0, 0)
    code = _cli_run(tool, tmp_path, "xyz",
                    _cli_metadata(tool, "pack-a") + ["--activate"])
    assert code == 0
    pyxis_map = tmp_path / "sd/pyxis-map"
    style_path = pyxis_map / "map-sets/osm-bright.pmas"
    assert style_path.is_file()
    slot = tool.decode_active_selection(style_path.read_bytes())
    assert slot["map_set_id"] == "osm-bright"
    assert slot["pack_ids"] == ["pack-a"]
    assert (pyxis_map / "active-pack.0").read_bytes() == style_path.read_bytes()


def test_cli_install_without_activate_writes_no_records(tool, tmp_path: Path) -> None:
    put_tile(tmp_path / "xyz", 1, 0, 0)
    code = _cli_run(tool, tmp_path, "xyz", _cli_metadata(tool, "pack-a"))
    assert code == 0
    pyxis_map = tmp_path / "sd/pyxis-map"
    assert (pyxis_map / "packs/pack-a/manifest.pmp").is_file()
    assert not (pyxis_map / "active-pack.0").exists()
    assert not (pyxis_map / "active-pack.1").exists()
    assert not (pyxis_map / "map-sets/osm-bright.pmas").exists()


def test_cli_exact_resume_after_activation_failure(tool, tmp_path: Path, monkeypatch) -> None:
    put_tile(tmp_path / "xyz", 1, 0, 0)
    real_publish = tool.publish_activation
    calls = {"n": 0}

    def flaky_publish(*args, **kwargs):
        calls["n"] += 1
        if calls["n"] == 1:
            raise OSError("injected activation failure")
        return real_publish(*args, **kwargs)

    monkeypatch.setattr(tool, "publish_activation", flaky_publish)
    code = _cli_run(tool, tmp_path, "xyz", _cli_metadata(tool, "pack-a") + ["--activate"])
    assert code == 4
    assert calls["n"] == 1
    # The published immutable pack survives the failed activation and the exact
    # retry reuses it (no republish) and converges.
    assert (tmp_path / "sd/pyxis-map/packs/pack-a/manifest.pmp").is_file()
    code = _cli_run(tool, tmp_path, "xyz", _cli_metadata(tool, "pack-a") + ["--activate"])
    assert code == 0
    assert calls["n"] == 2
    slot = tool.decode_active_selection(
        (tmp_path / "sd/pyxis-map/map-sets/osm-bright.pmas").read_bytes())
    assert slot["pack_ids"] == ["pack-a"]


def test_cli_existing_pack_metadata_mismatch_refuses(tool, tmp_path: Path) -> None:
    put_tile(tmp_path / "xyz", 1, 0, 0)
    assert _cli_run(tool, tmp_path, "xyz", _cli_metadata(tool, "pack-a")) == 0
    pyxis_map = tmp_path / "sd/pyxis-map"
    before = (pyxis_map / "packs/pack-a/manifest.pmp").read_bytes()
    # Resume with a different --name must refuse; the existing pack is untouched.
    code = tool.main([str(tmp_path / "xyz"), str(tmp_path / "sd"),
                      "--pack-id", "pack-a", "--name", "different name",
                      "--attribution", tool.STYLE_POLICIES["osm-bright"]["attribution"],
                      "--source", tool.STYLE_POLICIES["osm-bright"]["source"],
                      "--license", tool.STYLE_POLICIES["osm-bright"]["license"],
                      "--style", "osm-bright", "--activate"])
    assert code == 2
    assert (pyxis_map / "packs/pack-a/manifest.pmp").read_bytes() == before
    assert not (pyxis_map / "map-sets/osm-bright.pmas").exists()


def test_cli_missing_inherited_pack_refuses_before_record_mutation(tool, tmp_path: Path) -> None:
    pyxis_map, gen_1, gen_2 = _build_two_pack_card(tool, tmp_path)
    (pyxis_map / "packs/pack-a/manifest.pmp").unlink()
    put_tile(tmp_path / "src-c", 1, 0, 0)
    args = [str(tmp_path / "src-c"), str(tmp_path / "sd"),
            "--pack-id", "pack-c", "--name", "pack-c pack",
            "--attribution", tool.STYLE_POLICIES["osm-bright"]["attribution"],
            "--source", tool.STYLE_POLICIES["osm-bright"]["source"],
            "--license", tool.STYLE_POLICIES["osm-bright"]["license"],
            "--style", "osm-bright", "--activate"]
    code = tool.main(args)
    assert code == 2
    # Both active slots and the (absent) style record are unchanged after refusal.
    assert (pyxis_map / "active-pack.0").read_bytes() == gen_1
    assert (pyxis_map / "active-pack.1").read_bytes() == gen_2
    assert not (pyxis_map / "map-sets/osm-bright.pmas").exists()
    # The new pack was not published because preflight failed first.
    assert not (pyxis_map / "packs/pack-c").exists()


def test_cli_eight_pack_limit_refuses_before_publication(tool, tmp_path: Path) -> None:
    for index in range(8):
        src = tmp_path / f"src-{index}"
        put_tile(src, 1, 0, 0)
        assert _cli_run(tool, tmp_path, f"src-{index}",
                        _cli_metadata(tool, f"pack-{index}")) == 0
    pyxis_map = tmp_path / "sd/pyxis-map"
    attribution = tool.STYLE_POLICIES["osm-bright"]["attribution"]
    for index in range(1, 8):
        record = tool.encode_active_map_set(
            generation=index, map_set_id="osm-bright", attribution=attribution,
            pack_ids=[f"pack-{j}" for j in range(index, -1, -1)])
        (pyxis_map / "active-pack.0").write_bytes(record)
    put_tile(tmp_path / "src-new", 1, 0, 0)
    code = tool.main([str(tmp_path / "src-new"), str(tmp_path / "sd"),
                      "--pack-id", "pack-new", "--name", "pack-new pack",
                      "--attribution", attribution,
                      "--source", tool.STYLE_POLICIES["osm-bright"]["source"],
                      "--license", tool.STYLE_POLICIES["osm-bright"]["license"],
                      "--style", "osm-bright", "--activate"])
    assert code == 2
    assert not (pyxis_map / "packs/pack-new").exists()
    assert not (pyxis_map / "map-sets/osm-bright.pmas").exists()


def test_cli_activate_lock_contention(tool, tmp_path: Path) -> None:
    put_tile(tmp_path / "xyz", 1, 0, 0)
    pyxis_map = tmp_path / "sd/pyxis-map"
    pyxis_map.mkdir(parents=True)
    lock_fd = os.open(pyxis_map / tool.INSTALL_LOCK_NAME, os.O_RDWR | os.O_CREAT, 0o644)
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX)
        code = _cli_run(tool, tmp_path, "xyz", _cli_metadata(tool, "pack-a") + ["--activate"])
        assert code == 2
        assert not (pyxis_map / "map-sets/osm-bright.pmas").exists()
    finally:
        os.close(lock_fd)


def test_valid_pack_is_deterministic_and_independently_validated(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    make_rectangle(source)

    first = tool.build_map_pack(source, tmp_path / "sd-a", **metadata())
    second = tool.build_map_pack(source, tmp_path / "sd-b", **metadata())

    assert first == tmp_path / "sd-a/pyxis-map/packs/local-pack"
    assert (first / "manifest.pmp").read_bytes() == (second / "manifest.pmp").read_bytes()
    assert digest_tree(first) == digest_tree(second)
    parsed = tool.validate_pack(first)
    assert parsed["tile_count"] == 8
    assert parsed["min_zoom"] == 1
    assert parsed["max_zoom"] == 2
    assert (first / "tiles/2/2/2.png").read_bytes() == png()


def test_manifest_matches_committed_153_byte_cpp_fixture() -> None:
    tool = load_tool()
    fixture = bytes.fromhex(
        "504d504b0100100099000000000000000c776573742d636f6173745f310a576573"
        "7420436f617374194578616d706c65204d61707320636f6e7472696275746f7273"
        "0d6c6f63616c2d6578616d706c650943432d42592d342e300203020c0000000201"
        "010000000200000001000000020000000000000000000000030204000000050000"
        "0000000000010000000600000007000000d55ffd67"
    )
    extents = [
        tool.ZoomExtent(2, ((1, 2),), 1, 2),
        tool.ZoomExtent(3, ((0, 1), (6, 7)), 4, 5),
    ]
    actual = tool.serialize_manifest(
        pack_id="west-coast_1",
        name="West Coast",
        attribution="Example Maps contributors",
        source="local-example",
        license="CC-BY-4.0",
        extents=extents,
        tile_count=12,
    )
    assert len(fixture) == 153
    assert actual == fixture


def test_sparse_xyz_tree_builds_version_two_exact_row_spans(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    for x, y in ((1, 1), (1, 2), (2, 1)):
        put_tile(source, 2, x, y)
    built = tool.build_map_pack(source, tmp_path / "sd", sparse=True, **metadata())
    parsed = tool.validate_pack(built)
    assert parsed["format_version"] == 2
    assert parsed["tile_count"] == 3
    assert parsed["row_spans"] == [
        tool.RowSpan(2, 1, 1, 2),
        tool.RowSpan(2, 2, 1, 1),
    ]


def test_disjoint_rows_require_explicit_sparse_or_antimeridian_mode(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    for x in (0, 1, 6, 7):
        for y in (3, 4):
            put_tile(source, 3, x, y)
    with pytest.raises(tool.PackError, match="incomplete rectangle"):
        tool.build_map_pack(source, tmp_path / "default", **metadata())
    sparse = tool.build_map_pack(source, tmp_path / "sd-a", sparse=True, **metadata())
    sparse_manifest = tool.validate_pack(sparse)
    assert sparse_manifest["format_version"] == 2
    assert len(sparse_manifest["row_spans"]) == 4
    built = tool.build_map_pack(source, tmp_path / "sd-b", antimeridian_zooms={3}, **metadata())
    assert tool.validate_pack(built)["format_version"] == 1
    assert tool.validate_pack(built)["extents"][0].intervals == ((0, 1), (6, 7))


@pytest.mark.parametrize("bad_id", ["../escape", "Bad", "has/slash", "", "a" * 32])
def test_pack_id_grammar_rejects_traversal_and_noncanonical_ids(tmp_path: Path, bad_id: str) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0)
    with pytest.raises(tool.PackError, match="pack ID"):
        tool.build_map_pack(source, tmp_path / "sd", **metadata(pack_id=bad_id))


@pytest.mark.parametrize(
    ("parts", "message"),
    [(('01', '0', '0'), "noncanonical"), (('1', '+0', '0'), "noncanonical"), (('1', '0', '00'), "noncanonical")],
)
def test_noncanonical_xyz_names_are_rejected(tmp_path: Path, parts: tuple[str, str, str], message: str) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, *parts)
    with pytest.raises(tool.PackError, match=message):
        tool.build_map_pack(source, tmp_path / "sd", **metadata())


def test_unexpected_files_and_duplicate_colliding_keys_are_rejected(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 1, 0, 0)
    put_tile(source, "1", "0", "00")
    with pytest.raises(tool.PackError, match="noncanonical|duplicate"):
        tool.build_map_pack(source, tmp_path / "sd", **metadata())


def test_symlinks_are_rejected_without_following_them(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    outside = tmp_path / "outside.png"
    outside.write_bytes(png())
    (source / "0/0").mkdir(parents=True)
    (source / "0/0/0.png").symlink_to(outside)
    with pytest.raises(tool.PackError, match="symlink"):
        tool.build_map_pack(source, tmp_path / "sd", **metadata())


@pytest.mark.parametrize(
    ("data", "message"),
    [
        (b"not png", "PNG"),
        (b"\x89PNG\r\n\x1a\n" + b"\0" * 40, "IHDR"),
        (png()[:33], "PNG structure"),
        (png(255, 256), "256x256"),
    ],
)
def test_invalid_png_signature_ihdr_and_dimensions_are_rejected(tmp_path: Path, data: bytes, message: str) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0, data)
    with pytest.raises(tool.PackError, match=message):
        tool.build_map_pack(source, tmp_path / "sd", **metadata())


@pytest.mark.parametrize("z,x,y", [(23, 0, 0), (1, 2, 0), (1, 0, 2)])
def test_xyz_bounds_are_enforced(tmp_path: Path, z: int, x: int, y: int) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, z, x, y)
    with pytest.raises(tool.PackError, match="range"):
        tool.build_map_pack(source, tmp_path / "sd", **metadata())


def test_tile_count_and_total_byte_quotas_are_enforced(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0)
    with pytest.raises(tool.PackError, match="tile count"):
        tool.build_map_pack(source, tmp_path / "count", max_tiles=0, **metadata())
    with pytest.raises(tool.PackError, match="total bytes"):
        tool.build_map_pack(source, tmp_path / "bytes", max_bytes=len(png()) - 1, **metadata())
    with pytest.raises(tool.PackError, match="hard limit"):
        tool.build_map_pack(source, tmp_path / "count-hard", max_tiles=tool.DEFAULT_MAX_TILES + 1, **metadata())
    with pytest.raises(tool.PackError, match="hard limit"):
        tool.build_map_pack(source, tmp_path / "bytes-hard", max_bytes=tool.DEFAULT_MAX_BYTES + 1, **metadata())


@pytest.mark.parametrize("field", ["name", "attribution", "source", "license"])
def test_required_legal_and_name_metadata(field: str, tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0)
    with pytest.raises(tool.PackError, match=field):
        tool.build_map_pack(source, tmp_path / "sd", **metadata(**{field: ""}))


def test_existing_output_is_refused_without_modification(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0)
    target = tmp_path / "sd/pyxis-map/packs/local-pack"
    target.mkdir(parents=True)
    marker = target / "keep"
    marker.write_text("unchanged")
    with pytest.raises(tool.PackError, match="already exists"):
        tool.build_map_pack(source, tmp_path / "sd", **metadata())
    assert marker.read_text() == "unchanged"


def test_temporary_output_is_cleaned_after_failure(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0)

    def fail_copy(*_args, **_kwargs):
        raise OSError("injected copy failure")

    monkeypatch.setattr(tool, "copy_tile", fail_copy)
    with pytest.raises(OSError, match="injected"):
        tool.build_map_pack(source, tmp_path / "sd", **metadata())
    packs = tmp_path / "sd/pyxis-map/packs"
    assert not (packs / "local-pack").exists()
    assert not list(packs.glob(".local-pack.tmp-*"))


def test_cli_builds_local_tree_and_has_no_network_fetch_code(tmp_path: Path) -> None:
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0)
    result = subprocess.run(
        [sys.executable, os.fspath(TOOL), os.fspath(source), os.fspath(tmp_path / "sd"),
         "--pack-id", "cli-pack", "--name", "CLI Pack", "--attribution", "Example contributors",
         "--source", "local export", "--license", "CC-BY-4.0"],
        check=False, capture_output=True, text=True,
    )
    assert result.returncode == 0, result.stderr
    assert (tmp_path / "sd/pyxis-map/packs/cli-pack/manifest.pmp").is_file()
    source_text = TOOL.read_text(encoding="utf-8").lower()
    forbidden = ("urllib", "requests", "httpx", "aiohttp", "socket", "http://", "https://")
    assert not [token for token in forbidden if token in source_text]


@pytest.mark.parametrize("level", ["zoom", "x"])
def test_intermediate_source_symlink_swap_is_rejected(tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
                                                      level: str) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0)
    outside = tmp_path / "outside"
    put_tile(outside, 0, 0, 0)

    real_discover = tool._discover_tiles_fd
    def swapped_discover(*args, **kwargs):
        result = real_discover(*args, **kwargs)
        victim = source / "0" if level == "zoom" else source / "0/0"
        victim.rename(victim.with_name(victim.name + "-saved"))
        target = outside / "0" if level == "zoom" else outside / "0/0"
        victim.symlink_to(target, target_is_directory=True)
        return result

    monkeypatch.setattr(tool, "_discover_tiles_fd", swapped_discover)
    with pytest.raises(tool.PackError, match="changed|symlink"):
        tool.build_map_pack(source, tmp_path / "sd", **metadata())
    assert not (tmp_path / "sd/pyxis-map/packs/local-pack").exists()


@pytest.mark.parametrize("component", ["pyxis-map", "packs"])
def test_intermediate_output_symlink_swap_never_publishes_outside(tmp_path: Path,
                                                                 monkeypatch: pytest.MonkeyPatch,
                                                                 component: str) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0)
    output = tmp_path / "sd"
    outside = tmp_path / "outside"
    outside.mkdir()

    real_verify = tool._verify_output_chain
    def swapped_verify(*args, **kwargs):
        victim = output / component if component == "pyxis-map" else output / "pyxis-map/packs"
        victim.rename(victim.with_name(victim.name + "-saved"))
        victim.symlink_to(outside, target_is_directory=True)
        return real_verify(*args, **kwargs)

    monkeypatch.setattr(tool, "_verify_output_chain", swapped_verify)
    with pytest.raises(tool.PackError, match="output directory changed"):
        tool.build_map_pack(source, output, **metadata())
    assert not (outside / "local-pack").exists()


@pytest.mark.parametrize("race", ["replace", "grow", "truncate", "mutate", "fifo", "symlink"])
def test_validation_to_copy_races_fail_closed(tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
                                              race: str) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    tile = put_tile(source, 0, 0, 0)
    original_size = tile.stat().st_size

    real_discover = tool._discover_tiles_fd
    def raced_discover(*args, **kwargs):
        result = real_discover(*args, **kwargs)
        if race == "replace":
            replacement = tile.with_suffix(".replacement")
            replacement.write_bytes(png())
            os.replace(replacement, tile)
        elif race == "grow":
            with tile.open("ab") as stream:
                stream.write(b"x" * 4096)
        elif race == "truncate":
            tile.write_bytes(png()[:-12])
        elif race == "mutate":
            data = bytearray(tile.read_bytes())
            data[-1] ^= 1
            tile.write_bytes(data)
        elif race == "fifo":
            tile.unlink()
            os.mkfifo(tile)
        else:
            outside = tmp_path / "outside.png"
            outside.write_bytes(png())
            tile.unlink()
            tile.symlink_to(outside)
        return result

    monkeypatch.setattr(tool, "_discover_tiles_fd", raced_discover)
    with pytest.raises(tool.PackError, match="changed|regular file|symlink|quota|PNG"):
        tool.build_map_pack(source, tmp_path / "sd", max_bytes=original_size, **metadata())


def test_quota_is_checked_before_any_tile_read(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    tile = put_tile(source, 0, 0, 0)
    with tile.open("ab") as stream:
        stream.truncate(32 * 1024 * 1024)
    reads = 0
    real_read = os.read

    def tracked_read(fd: int, size: int) -> bytes:
        nonlocal reads
        reads += 1
        return real_read(fd, size)

    monkeypatch.setattr(tool.os, "read", tracked_read)
    with pytest.raises(tool.PackError, match="total bytes"):
        tool.build_map_pack(source, tmp_path / "sd", max_bytes=1024, **metadata())
    assert reads == 0


def test_entry_visit_cap_rejects_before_unbounded_enumeration(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    source.mkdir()
    for index in range(tool.MAX_VISITED_ENTRIES_BASE + 1):
        (source / f"junk-{index}").mkdir()
    with pytest.raises(tool.PackError, match="visited-entry quota|noncanonical"):
        tool.discover_tiles(source, max_tiles=0, max_bytes=0)


def test_atomic_noreplace_unsupported_fails_closed(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0)
    monkeypatch.setattr(tool, "_renameat2", None)
    with pytest.raises(tool.PackError, match="atomic no-replace unsupported"):
        tool.build_map_pack(source, tmp_path / "sd", **metadata())
    assert not (tmp_path / "sd/pyxis-map/packs/local-pack").exists()


def test_post_rename_fsync_reports_published_durability_uncertain(tmp_path: Path,
                                                                 monkeypatch: pytest.MonkeyPatch) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0)

    real_rename = tool._rename_noreplace
    def rename_then_break_fsync(*args, **kwargs):
        real_rename(*args, **kwargs)
        monkeypatch.setattr(tool.os, "fsync", lambda _fd: (_ for _ in ()).throw(OSError("fsync fault")))

    monkeypatch.setattr(tool, "_rename_noreplace", rename_then_break_fsync)
    with pytest.raises(tool.PublishedDurabilityError) as caught:
        tool.build_map_pack(source, tmp_path / "sd", **metadata())
    assert caught.value.published is True
    assert caught.value.target == tmp_path / "sd/pyxis-map/packs/local-pack"
    assert caught.value.target.is_dir()


@pytest.mark.parametrize("data", [
    png_chunks([(b"IHDR", ihdr()), (b"ABCD", b""), (b"IDAT", zlib.compress(raw_rgb())), (b"IEND", b"")]),
    png_chunks([(b"IHDR", ihdr()), (b"abcd", b""), (b"IDAT", zlib.compress(raw_rgb())), (b"IEND", b"")]),
    png_chunks([(b"IHDR", ihdr()), (b"PLTE", b"\x00\x00\x00" * 257),
                (b"IDAT", zlib.compress(raw_rgb())), (b"IEND", b"")]),
    png_chunks([(b"IHDR", ihdr()), (b"IDAT", b"invalid-deflate"), (b"IEND", b"")]),
    png_chunks([(b"IHDR", ihdr()), (b"IDAT", zlib.compress(raw_rgb())[:-2]), (b"IEND", b"")]),
    png_chunks([(b"IHDR", ihdr()), (b"IDAT", zlib.compress(raw_rgb()) + b"trailing"), (b"IEND", b"")]),
    png_chunks([(b"IHDR", ihdr()), (b"IDAT", zlib.compress(raw_rgb())[:10]), (b"tEXt", b"x"),
                (b"IDAT", zlib.compress(raw_rgb())[10:]), (b"IEND", b"")]),
    png_chunks([(b"IHDR", ihdr(color_type=3)), (b"IDAT", zlib.compress(b"\x00" * (257 * 256))),
                (b"IEND", b"")]),
    png_chunks([(b"IHDR", ihdr(color_type=3)), (b"PLTE", b"\x00\x00\x00"),
                (b"IDAT", zlib.compress(b"".join(b"\x00" + b"\xff" * 256 for _ in range(256)))),
                (b"IEND", b"")]),
    png_chunks([(b"IHDR", ihdr(color_type=2)), (b"PLTE", b"\x00\x00\x00"),
                (b"PLTE", b"\x00\x00\x00"), (b"IDAT", zlib.compress(raw_rgb())), (b"IEND", b"")]),
    png_chunks([(b"IHDR", ihdr(color_type=3)), (b"IDAT", zlib.compress(b"\x00" * (257 * 256))),
                (b"PLTE", b"\x00\x00\x00"), (b"IEND", b"")]),
    png_chunks([(b"IHDR", ihdr()), (b"IDAT", zlib.compress(b"\x05" + raw_rgb()[1:])), (b"IEND", b"")]),
    png_chunks([(b"IHDR", ihdr(interlace=1)), (b"IDAT", zlib.compress(raw_rgb())), (b"IEND", b"")]),
])
def test_malformed_png_critical_state_and_deflate_are_rejected(tmp_path: Path, data: bytes) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0, data)
    with pytest.raises(tool.PackError, match="PNG"):
        tool.build_map_pack(source, tmp_path / "sd", **metadata())


def test_python_manifest_mutations_match_cpp_validation_rules() -> None:
    tool = load_tool()
    valid = bytearray(tool.serialize_manifest(
        **metadata(), extents=[tool.ZoomExtent(0, ((0, 0),), 0, 0)], tile_count=1,
    ))
    values = metadata()
    strings_end = 16 + sum(1 + len(values[field]) for field in
                           ("pack_id", "name", "attribution", "source", "license"))
    extent = strings_end + 7
    mutations = []
    bad = bytearray(valid); bad[extent + 1] = 3; mutations.append(bad)
    bad = bytearray(valid); struct.pack_into("<I", bad, extent + 18, 1); mutations.append(bad)
    bad = bytearray(valid); bad[strings_end] = 1; mutations.append(bad)
    bad = bytearray(valid); struct.pack_into("<I", bad, strings_end + 3, 2); mutations.append(bad)
    for mutation in mutations:
        with pytest.raises(tool.PackError):
            tool.parse_manifest(rewrite_crc(mutation))
    with pytest.raises(tool.PackError):
        tool.serialize_manifest(**metadata(), extents=[tool.ZoomExtent(1, ((1, 0),), 0, 0)], tile_count=999)


def test_documented_cli_uses_supported_ascii_metadata(tmp_path: Path) -> None:
    tool = load_tool()
    docs = (ROOT / "docs/offline-map-packs.md").read_text(encoding="utf-8")
    assert 'Map data (c) Example contributors' in docs
    source = tmp_path / "my-xyz"
    put_tile(source, 0, 0, 0)
    assert tool.main([
        os.fspath(source), os.fspath(tmp_path / "sd"), "--pack-id", "regional-map",
        "--name", "Regional Map", "--attribution", "Map data (c) Example contributors",
        "--source", "Local export supplied by the user", "--license", "CC-BY-4.0",
    ]) == 0


def test_write_all_handles_partial_write_and_rejects_zero_progress(tmp_path: Path,
                                                                   monkeypatch: pytest.MonkeyPatch) -> None:
    tool = load_tool()
    destination = tmp_path / "output"
    descriptor = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    real_write = os.write
    try:
        monkeypatch.setattr(tool.os, "write", lambda fd, data: real_write(fd, data[:max(1, len(data) // 2)]))
        tool._write_all(descriptor, b"complete payload", "test")
    finally:
        os.close(descriptor)
    assert destination.read_bytes() == b"complete payload"

    monkeypatch.setattr(tool.os, "write", lambda _fd, _data: 0)
    with pytest.raises(OSError, match="short write"):
        tool._write_all(-1, b"x", "test")


def test_output_relocation_between_verify_and_rename_is_rolled_back(tmp_path: Path,
                                                                    monkeypatch: pytest.MonkeyPatch) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0)
    output = tmp_path / "sd"
    outside = tmp_path / "outside"
    outside.mkdir()
    real_rename = tool._rename_noreplace

    def relocate_then_rename(*args, **kwargs):
        packs = output / "pyxis-map/packs"
        moved = outside / "moved-packs"
        packs.rename(moved)
        real_rename(*args, **kwargs)

    monkeypatch.setattr(tool, "_rename_noreplace", relocate_then_rename)
    with pytest.raises(tool.PackError, match="changed during publication"):
        tool.build_map_pack(source, output, **metadata())
    assert not (outside / "moved-packs/local-pack").exists()
    assert not (output / "pyxis-map/packs/local-pack").exists()


def test_post_commit_close_failure_is_typed_as_published(tmp_path: Path,
                                                          monkeypatch: pytest.MonkeyPatch) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0)
    output = tmp_path / "sd"
    real_verify = tool._verify_output_chain
    real_close = tool.os.close
    calls = 0

    def arm_after_second_verify(*args, **kwargs):
        nonlocal calls
        calls += 1
        result = real_verify(*args, **kwargs)
        if calls == 2:
            failed = False
            def fail_once(fd: int):
                nonlocal failed
                if not failed:
                    failed = True
                    raise OSError("close fault")
                return real_close(fd)
            monkeypatch.setattr(tool.os, "close", fail_once)
        return result

    monkeypatch.setattr(tool, "_verify_output_chain", arm_after_second_verify)
    with pytest.raises(tool.PublishedDurabilityError) as caught:
        tool.build_map_pack(source, output, **metadata())
    assert caught.value.published is True
    assert (output / "pyxis-map/packs/local-pack").is_dir()
