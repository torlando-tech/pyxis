# Copyright (c) 2026 Pyxis contributors
# SPDX-License-Identifier: MIT

from __future__ import annotations

import hashlib
import importlib.util
import os
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


def rewrite_crc(data: bytearray) -> bytes:
    data[-4:] = struct.pack("<I", zlib.crc32(data[:-4]))
    return bytes(data)


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


# --- PMPK v3 (indexless) + PMAS v3 (active map set) -------------------------

def test_indexless_manifest_matches_js_flasher_byte_for_byte() -> None:
    tool = load_tool()
    fixture = bytes.fromhex(
        "504d504b03001000d0000000000000001f776f726c642d6f736d2d6272696768742d7a30"
        "2d7a392d323032363038303116576f726c64204f534d20427269676874207a302d7a392f"
        "286329204f70656e4d617054696c657320286329204f70656e5374726565744d61702063"
        "6f6e7472696275746f7273274f7865642773204d61702054696c6520446f776e6c6f6164"
        "657220284f534d2042726967687429264f534d204f44624c3b207374796c652043432d"
        "42592d342e302f4253442d332d436c6175736500096f460100e9a28313"
    )
    actual = tool.serialize_indexless_manifest(
        pack_id="world-osm-bright-z0-z9-20260801",
        name="World OSM Bright z0-z9",
        attribution="(c) OpenMapTiles (c) OpenStreetMap contributors",
        source="Oxed's Map Tile Downloader (OSM Bright)",
        license="OSM ODbL; style CC-BY-4.0/BSD-3-Clause",
        min_zoom=0,
        max_zoom=9,
        tile_count=83567,
    )
    assert actual == fixture
    parsed = tool.parse_manifest(actual)
    assert parsed["format_version"] == 3
    assert parsed["tile_count"] == 83567
    assert parsed["min_zoom"] == 0
    assert parsed["max_zoom"] == 9
    assert parsed["extents"] == [] and parsed["row_spans"] == []


def test_indexless_manifest_rejects_bad_ranges() -> None:
    tool = load_tool()
    base = dict(pack_id="p", name="P", attribution="A", source="S", license="L")
    with pytest.raises(tool.PackError, match="range or tile count"):
        tool.serialize_indexless_manifest(min_zoom=3, max_zoom=1, tile_count=1, **base)
    with pytest.raises(tool.PackError, match="range or tile count"):
        tool.serialize_indexless_manifest(min_zoom=0, max_zoom=23, tile_count=1, **base)
    with pytest.raises(tool.PackError, match="range or tile count"):
        tool.serialize_indexless_manifest(min_zoom=0, max_zoom=2, tile_count=0, **base)


def test_pmas_v3_matches_js_flasher_byte_for_byte() -> None:
    tool = load_tool()
    fixture = bytes.fromhex(
        "504d415303007500070000000a6f736d2d6272696768742f286329204f70656e4d6170"
        "54696c657320286329204f70656e5374726565744d617020636f6e7472696275746f72"
        "73021f776f726c642d6f736d2d6272696768742d7a302d7a392d323032363038303108"
        "76697267696e69619e5ec1bc"
    )
    actual = tool.encode_active_map_set(
        generation=7,
        map_set_id="osm-bright",
        attribution="(c) OpenMapTiles (c) OpenStreetMap contributors",
        pack_ids=["world-osm-bright-z0-z9-20260801", "virginia"],
    )
    assert actual == fixture
    parsed = tool.parse_active_map_set(actual)
    assert parsed["format_version"] == 3
    assert parsed["generation"] == 7
    assert parsed["map_set_id"] == "osm-bright"
    assert parsed["packs"] == ["world-osm-bright-z0-z9-20260801", "virginia"]


def test_pmas_rejects_duplicates_and_zero_generation() -> None:
    tool = load_tool()
    with pytest.raises(tool.PackError, match="duplicate"):
        tool.encode_active_map_set(generation=1, map_set_id="s", attribution="a",
                                   pack_ids=["p", "p"])
    with pytest.raises(tool.PackError, match="generation"):
        tool.encode_active_map_set(generation=0, map_set_id="s", attribution="a",
                                   pack_ids=["p"])
    with pytest.raises(tool.PackError, match="packs"):
        tool.encode_active_map_set(generation=1, map_set_id="s", attribution="a",
                                   pack_ids=[])


def test_style_install_emits_indexless_v3_and_is_validated(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    # Deliberately sparse: not a rectangle and not antimeridian, so v1 would fail.
    for x, y in ((0, 0), (0, 3), (1, 1), (2, 2), (3, 0)):
        put_tile(source, 2, x, y)
    policy = tool.STYLE_POLICIES["osm-bright"]
    built = tool.build_map_pack(
        source, tmp_path / "sd", pack_id="world-osm-bright-z0-z9-20260801",
        name="World OSM Bright z0-z9",
        attribution=policy["attribution"], source=policy["source"],
        license=policy["license"], style="osm-bright",
    )
    parsed = tool.validate_pack(built)
    assert parsed["format_version"] == 3
    assert parsed["tile_count"] == 5
    assert parsed["min_zoom"] == 2 and parsed["max_zoom"] == 2


def test_style_policy_mismatch_is_rejected(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0)
    with pytest.raises(tool.PackError, match="style policy"):
        tool.build_map_pack(source, tmp_path / "sd", pack_id="p", name="P",
                            attribution="wrong", source="wrong", license="wrong",
                            style="osm-bright")


def test_sparse_beyond_span_cap_falls_back_to_indexless_v3(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    # Every zoom is a single column (x = 0) with an even-y (noncontiguous)
    # set, so no zoom is a rectangle and the sparse path is required:
    #   zoom 10: 512 tiles (even y), 512 single-tile row spans
    #   zoom 9:  256 tiles (even y), 256 spans
    # 768 row spans > MAX_ROW_SPANS (512), so v2 cannot represent it and the
    # builder must fall back to an indexless v3 pack.
    for y in range(0, 1024, 2):
        put_tile(source, 10, 0, y)
    for y in range(0, 512, 2):
        put_tile(source, 9, 0, y)
    built = tool.build_map_pack(source, tmp_path / "sd", sparse=True, **metadata())
    parsed = tool.validate_pack(built)
    assert parsed["format_version"] == 3
    assert parsed["tile_count"] == 768
    assert parsed["min_zoom"] == 9 and parsed["max_zoom"] == 10


def test_activation_publishes_pmas_and_is_idempotent_reorder(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 1, 0, 0)
    policy = tool.STYLE_POLICIES["osm-bright"]
    sd = tmp_path / "sd"
    tool.build_map_pack(source, sd, pack_id="pack-a", name="A",
                        attribution=policy["attribution"], source=policy["source"],
                        license=policy["license"], style="osm-bright", activate=True)
    active = (sd / "pyxis-map/active-pack.0").read_bytes()
    record = tool.decode_active_selection(active)
    assert record["format_version"] == 3
    assert record["generation"] == 1
    assert record["map_set_id"] == "osm-bright"
    assert record["packs"] == ["pack-a"]
    style_record = (sd / "pyxis-map/map-sets/osm-bright.pmas").read_bytes()
    assert style_record == active

    # A second pack of the same style moves to the front, generation bumps.
    source2 = tmp_path / "xyz2"
    put_tile(source2, 1, 1, 1)
    tool.build_map_pack(source2, sd, pack_id="pack-b", name="B",
                        attribution=policy["attribution"], source=policy["source"],
                        license=policy["license"], style="osm-bright", activate=True)
    # Generation 2 goes into the other slot.
    active2 = (sd / "pyxis-map/active-pack.1").read_bytes()
    record2 = tool.decode_active_selection(active2)
    assert record2["generation"] == 2
    assert record2["packs"] == ["pack-b", "pack-a"]
    # The old slot keeps its record; the firmware picks the higher generation.
    assert (sd / "pyxis-map/active-pack.0").read_bytes() == active
    assert (sd / "pyxis-map/map-sets/osm-bright.pmas").read_bytes() == active2


def test_activation_requires_style(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0)
    with pytest.raises(tool.PackError, match="requires --style"):
        tool.build_map_pack(source, tmp_path / "sd", **metadata(), activate=True)


def _seed_style_pack(tool, tmp_path: Path, sd: Path, index: int, pack_id: str) -> None:
    source = tmp_path / f"seed{index}"
    put_tile(source, 1, index % 2, index % 2)
    policy = tool.STYLE_POLICIES["osm-bright"]
    tool.build_map_pack(source, sd, pack_id=pack_id, name=f"Seed {index}",
                        attribution=policy["attribution"], source=policy["source"],
                        license=policy["license"], style="osm-bright", activate=True)


def test_interrupted_activation_replacement_keeps_previous_records(tmp_path: Path,
                                                                  monkeypatch: pytest.MonkeyPatch) -> None:
    # Simulating a crash between the temp write and its rename must never
    # truncate the previous style record or active slot (P1: non-atomic
    # O_TRUNC replacement corrupts style discovery).
    tool = load_tool()
    policy = tool.STYLE_POLICIES["osm-bright"]
    source = tmp_path / "xyz"
    put_tile(source, 1, 0, 0)
    sd = tmp_path / "sd"
    tool.build_map_pack(source, sd, pack_id="pack-a", name="A",
                        attribution=policy["attribution"], source=policy["source"],
                        license=policy["license"], style="osm-bright", activate=True)
    style_path = sd / "pyxis-map/map-sets/osm-bright.pmas"
    slot_path = sd / "pyxis-map/active-pack.0"
    style_before = style_path.read_bytes()
    slot_before = slot_path.read_bytes()

    real_renameat2 = tool._renameat2

    def interrupted_renameat2(fd, src, dst, dirfd, flags):
        # Interrupt only the record replacement (flags 0); pack staging
        # (RENAME_NOREPLACE, flags 1) must proceed so the failure lands
        # after publication, exactly like a crash mid-activation.
        if flags == 0:
            raise OSError("injected interruption before rename")
        return real_renameat2(fd, src, dst, dirfd, flags)

    monkeypatch.setattr(tool, "_renameat2", interrupted_renameat2)
    source2 = tmp_path / "xyz2"
    put_tile(source2, 1, 1, 1)
    with pytest.raises(tool.PackError, match="cannot atomically replace"):
        tool.build_map_pack(source2, sd, pack_id="pack-b", name="B",
                            attribution=policy["attribution"], source=policy["source"],
                            license=policy["license"], style="osm-bright", activate=True)

    # Previous records are byte-identical; no staging debris is left behind.
    assert style_path.read_bytes() == style_before
    assert slot_path.read_bytes() == slot_before
    assert tool.decode_active_selection(style_before)["packs"] == ["pack-a"]
    assert not list((sd / "pyxis-map/map-sets").glob(".osm-bright.pmas.tmp-*"))
    assert not list((sd / "pyxis-map").glob("active-pack.1.tmp-*"))
    # The just-published pack stays valid on disk; the failure is confined to
    # the record publication, which a later run can repair.
    assert (sd / "pyxis-map/packs/pack-b/manifest.pmp").is_file()


def test_activation_ninth_distinct_pack_fails_before_publishing(tmp_path: Path) -> None:
    # The firmware rejects PMAS records with more than MAX_PACKS entries, so
    # the writer must validate the inherited composition before the new pack
    # is permanently published -- otherwise the failure orphans the pack
    # (P1: activation failure leaves orphan pack).
    tool = load_tool()
    assert tool.MAX_ACTIVE_PACKS == 8
    policy = tool.STYLE_POLICIES["osm-bright"]
    sd = tmp_path / "sd"
    for index in range(tool.MAX_ACTIVE_PACKS):
        _seed_style_pack(tool, tmp_path, sd, index, f"seed-{index}")
    style_path = sd / "pyxis-map/map-sets/osm-bright.pmas"
    slots = [sd / f"pyxis-map/active-pack.{i}" for i in range(2)]
    style_before = style_path.read_bytes()
    slots_before = [slot.read_bytes() for slot in slots]
    assert tool.decode_active_selection(style_before)["generation"] == tool.MAX_ACTIVE_PACKS

    source = tmp_path / "ninth"
    put_tile(source, 1, 0, 1)
    with pytest.raises(tool.PackError, match="pack limit exceeded"):
        tool.build_map_pack(source, sd, pack_id="ninth-pack", name="Ninth",
                            attribution=policy["attribution"], source=policy["source"],
                            license=policy["license"], style="osm-bright", activate=True)

    # No record was rewritten and the rejected pack was not published.
    assert style_path.read_bytes() == style_before
    assert [slot.read_bytes() for slot in slots] == slots_before
    assert not (sd / "pyxis-map/packs/ninth-pack").exists()


def test_activation_conflicting_slots_rejected_before_publishing(tmp_path: Path) -> None:
    # Two active slots at the same generation with different content is a
    # conflict activate_map_set refuses -- the preflight must refuse it too,
    # before the new pack is permanently published (Greploop round 2).
    tool = load_tool()
    policy = tool.STYLE_POLICIES["osm-bright"]
    sd = tmp_path / "sd"
    (sd / "pyxis-map").mkdir(parents=True)
    slot0 = tool.encode_active_map_set(generation=5, map_set_id="osm-bright",
                                       attribution=policy["attribution"],
                                       pack_ids=["existing-a"])
    slot1 = tool.encode_active_map_set(generation=5, map_set_id="osm-bright",
                                       attribution=policy["attribution"],
                                       pack_ids=["existing-b"])
    (sd / "pyxis-map/active-pack.0").write_bytes(slot0)
    (sd / "pyxis-map/active-pack.1").write_bytes(slot1)

    source = tmp_path / "xyz"
    put_tile(source, 1, 0, 0)
    with pytest.raises(tool.PackError, match="conflicting active map-set records"):
        tool.build_map_pack(source, sd, pack_id="conflict-pack", name="C",
                            attribution=policy["attribution"], source=policy["source"],
                            license=policy["license"], style="osm-bright", activate=True)

    # The conflicting slots are untouched and nothing was published.
    assert (sd / "pyxis-map/active-pack.0").read_bytes() == slot0
    assert (sd / "pyxis-map/active-pack.1").read_bytes() == slot1
    assert not (sd / "pyxis-map/packs/conflict-pack").exists()


def test_interrupted_activation_retry_converges_records(tmp_path: Path) -> None:
    # A run interrupted between the slot-record and style-record commits
    # leaves a published, ACTIVE pack (slot committed) with a missing style
    # record. The firmware keeps serving tiles from the authoritative slot
    # and tolerates the missing style record (MapStyleCatalog::discover
    # synthesizes the candidate from the slot); re-running the same source
    # verifies the existing pack and converges the style record
    # (Greploop round 3 + round 4: slot-first ordering).
    tool = load_tool()
    policy = tool.STYLE_POLICIES["osm-bright"]
    sd = tmp_path / "sd"
    (sd / "pyxis-map").mkdir(parents=True)
    source = tmp_path / "xyz"
    put_tile(source, 1, 0, 0)

    real_renameat2 = tool._renameat2
    interrupted = {"done": False}

    def interrupted_renameat2(oldfd, oldpath, newfd, newpath, flags):
        # Interrupt only the style-record replacement (flags 0, newpath is
        # the .pmas). Pack staging (flags 1) and the slot-record commit
        # (flags 0, newpath an active-pack slot) must proceed so the
        # failure lands between the two record commits, leaving the map
        # active but the style record stale.
        if not interrupted["done"] and flags == 0 and newpath.endswith(b".pmas"):
            interrupted["done"] = True
            raise OSError("injected interruption between record commits")
        return real_renameat2(oldfd, oldpath, newfd, newpath, flags)

    monkeypatch = pytest.MonkeyPatch()
    monkeypatch.setattr(tool, "_renameat2", interrupted_renameat2)
    try:
        with pytest.raises(tool.PackError, match="cannot atomically replace"):
            tool.build_map_pack(source, sd, pack_id="resume-pack", name="R",
                                attribution=policy["attribution"], source=policy["source"],
                                license=policy["license"], style="osm-bright", activate=True)
    finally:
        monkeypatch.undo()

    # The pack published and the authoritative slot committed; the style
    # record did not. The map is therefore active, not broken.
    assert (sd / "pyxis-map/packs/resume-pack/manifest.pmp").is_file()
    style_path = sd / "pyxis-map/map-sets/osm-bright.pmas"
    assert not style_path.exists()
    assert (sd / "pyxis-map/active-pack.0").read_bytes() is not None

    # Retry with the same source: the pack verifies byte-identical and
    # converges the style record. The retry writes the free slot
    # (active-pack.1) at a higher generation than the interrupted first run
    # left in active-pack.0, and the style record matches the authoritative
    # (highest-generation) slot.
    result = tool.build_map_pack(source, sd, pack_id="resume-pack", name="R",
                                 attribution=policy["attribution"], source=policy["source"],
                                 license=policy["license"], style="osm-bright", activate=True)
    assert result == sd / "pyxis-map/packs/resume-pack"
    slot0 = tool.decode_active_selection((sd / "pyxis-map/active-pack.0").read_bytes())
    slot1 = tool.decode_active_selection((sd / "pyxis-map/active-pack.1").read_bytes())
    authoritative = slot1 if slot1["generation"] > slot0["generation"] else slot0
    style = tool.decode_active_selection(style_path.read_bytes())
    assert style == authoritative
    assert style["packs"][0] == "resume-pack"


def _flock_holder(path: str) -> subprocess.Popen:
    # Spawn a subprocess that takes an exclusive flock on the lock file and
    # signals readiness over stdout. This is the only way to exercise a real
    # concurrent lock holder: flock is a per-process kernel state, so a fake
    # pid in a file cannot hold it.
    code = (
        "import fcntl, os, sys, time\n"
        "fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT, 0o644)\n"
        "fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)\n"
        "sys.stdout.write('ready\\n')\n"
        "sys.stdout.flush()\n"
        "time.sleep(60)\n"
    )
    return subprocess.Popen([sys.executable, "-c", code, path],
                            stdout=subprocess.PIPE, text=True)


def test_concurrent_activation_is_serialized_by_lock(tmp_path: Path) -> None:
    # A second builder cannot activate while the first holds the install
    # flock; when the holder exits, the kernel releases the lock and the
    # build proceeds (Greploop round 3 + round 4: lock must be a real,
    # race-free kernel primitive, not a pid file).
    tool = load_tool()
    policy = tool.STYLE_POLICIES["osm-bright"]
    sd = tmp_path / "sd"
    (sd / "pyxis-map").mkdir(parents=True)
    lock_file = sd / "pyxis-map/.pyxis-install.lock"
    source = tmp_path / "xyz"
    put_tile(source, 1, 0, 0)

    holder = _flock_holder(str(lock_file))
    try:
        assert holder.stdout.readline().strip() == "ready"
        with pytest.raises(tool.PackError, match="already running"):
            tool.build_map_pack(source, sd, pack_id="locked-pack", name="L",
                                attribution=policy["attribution"], source=policy["source"],
                                license=policy["license"], style="osm-bright", activate=True)
        assert not (sd / "pyxis-map/packs/locked-pack").exists()
    finally:
        holder.terminate()
        holder.wait()

    # Holder exited: the kernel released the flock, so the build now proceeds.
    result = tool.build_map_pack(source, sd, pack_id="locked-pack", name="L",
                                 attribution=policy["attribution"], source=policy["source"],
                                 license=policy["license"], style="osm-bright", activate=True)
    assert (sd / "pyxis-map/packs/locked-pack/manifest.pmp").is_file()
    assert (sd / "pyxis-map/active-pack.0").is_file()
    # The lock file is persistent (never unlinked) -- see _install_lock_at.
    assert lock_file.exists()


def test_existing_pack_rejects_different_source(tmp_path: Path) -> None:
    # Resume is only allowed for a byte-identical republish; a different
    # source must fail before touching the pack or the records.
    tool = load_tool()
    policy = tool.STYLE_POLICIES["osm-bright"]
    sd = tmp_path / "sd"
    (sd / "pyxis-map").mkdir(parents=True)
    source = tmp_path / "xyz"
    put_tile(source, 1, 0, 0)
    tool.build_map_pack(source, sd, pack_id="dup-pack", name="D",
                        attribution=policy["attribution"], source=policy["source"],
                        license=policy["license"])
    pack_before = (sd / "pyxis-map/packs/dup-pack/manifest.pmp").read_bytes()

    different = tmp_path / "xyz2"
    put_tile(different, 1, 0, 1)  # different tile -> different manifest
    with pytest.raises(tool.PackError, match="does not match this source"):
        tool.build_map_pack(different, sd, pack_id="dup-pack", name="D",
                            attribution=policy["attribution"], source=policy["source"],
                            license=policy["license"], style="osm-bright", activate=True)
    assert (sd / "pyxis-map/packs/dup-pack/manifest.pmp").read_bytes() == pack_before
    assert not (sd / "pyxis-map/map-sets").exists()


def test_decode_selection_handles_v1_and_v2(tmp_path: Path) -> None:
    tool = load_tool()
    # v1: 48 bytes, magic, version 1, len 48, generation, then id at 12..44
    pack = b"v1pack"
    v1 = bytearray(b"PMAS")
    v1 += bytes([1, 0])
    v1 += struct.pack("<H", 48)
    v1 += struct.pack("<I", 5)
    v1 += bytes([len(pack)])
    v1 += pack
    v1 += bytes(44 - len(v1))
    v1 += struct.pack("<I", zlib.crc32(bytes(v1)))
    got = tool.decode_active_selection(bytes(v1))
    assert got["format_version"] == 1
    assert got["packs"] == ["v1pack"]
    assert got["generation"] == 5

    # v2: magic, version 2, reserved 0, len, generation, then
    # map-set id, attribution, pack count, and one pack with one span.
    v2 = bytearray(b"PMAS")
    v2 += bytes([2, 0])
    body = bytearray()
    body += bytes([6]) + b"mapset"   # map_set_id (identifier grammar: 3+ chars)
    body += bytes([1]) + b"a"        # attribution
    body += bytes([1])               # pack count
    body += bytes([2]) + b"p1"       # pack id
    body += struct.pack("<H", 1)     # span count
    body += bytes([1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0])  # one span z1 y0 x0..1
    total = 12 + len(body) + 4
    v2 += struct.pack("<H", total)
    v2 += struct.pack("<I", 9)
    v2 += body
    v2 += struct.pack("<I", zlib.crc32(bytes(v2)))
    got2 = tool.decode_active_selection(bytes(v2))
    assert got2["format_version"] == 2
    assert got2["packs"] == ["p1"]
    assert got2["generation"] == 9


def test_unfilter_fast_path_matches_slow_path() -> None:
    tool = load_tool()
    import random as _random
    _random.seed(1234)
    # All-None-filter decode must equal the generic predictor loop output.
    rows = bytearray()
    for _ in range(256):
        rows += b"\x00" + bytes(_random.getrandbits(8) for _ in range(768))
    fast = tool._unfilter_png_rows(bytes(rows), 768, 3, "fast")
    slow = tool._unfilter_png_rows(bytes(rows), 768, 3, "slow")
    assert fast == slow
    # A mixed-filter image must still take the correct generic path.
    mixed = bytearray(rows)
    mixed[0] = 2  # Up filter on first row: value == above (zero row) => same pixels
    assert tool._unfilter_png_rows(bytes(mixed), 768, 3, "mixed") == fast


def test_metadata_json_is_ignored_but_validated(tmp_path: Path) -> None:
    tool = load_tool()
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0)
    (source / "metadata.json").write_text('{"maxZoom": 0}', encoding="utf-8")
    built = tool.build_map_pack(source, tmp_path / "sd", **metadata())
    parsed = tool.validate_pack(built)
    assert parsed["tile_count"] == 1
    assert not (built / "tiles/metadata.json").exists()

    bad = tmp_path / "xyz-bad"
    put_tile(bad, 0, 0, 0)
    (bad / "metadata.json").write_text("not json", encoding="utf-8")
    with pytest.raises(tool.PackError, match="metadata.json is not valid JSON"):
        tool.build_map_pack(bad, tmp_path / "sd-bad", **metadata())


def test_indexed_png_fast_path_accepts_in_range_and_rejects_out_of_range(tmp_path: Path) -> None:
    tool = load_tool()
    # 8-bit indexed, 2-entry palette, indices 0/1 only -> in range, must pass.
    good = png_chunks([(b"IHDR", ihdr(color_type=3)), (b"PLTE", b"\x00\x00\x00" * 2),
                       (b"IDAT", zlib.compress(b"".join(b"\x00" + b"\x01\x00" * 128 for _ in range(256)))),
                       (b"IEND", b"")])
    source = tmp_path / "xyz"
    put_tile(source, 0, 0, 0, good)
    built = tool.build_map_pack(source, tmp_path / "sd", **metadata())
    assert tool.validate_pack(built)["tile_count"] == 1

    # Same tile but one pixel uses index 5 (>= 2 entries) -> must be rejected.
    bad = png_chunks([(b"IHDR", ihdr(color_type=3)), (b"PLTE", b"\x00\x00\x00" * 2),
                      (b"IDAT", zlib.compress(b"".join(b"\x00" + (b"\x05" + b"\x01" * 255) for _ in range(256)))),
                      (b"IEND", b"")])
    source_bad = tmp_path / "xyz-bad"
    put_tile(source_bad, 0, 0, 0, bad)
    with pytest.raises(tool.PackError, match="palette index out of range"):
        tool.build_map_pack(source_bad, tmp_path / "sd-bad", **metadata())
