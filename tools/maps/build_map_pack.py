#!/usr/bin/env python3
# Copyright (c) 2026 Pyxis contributors
# SPDX-License-Identifier: MIT
"""Build a Pyxis offline map pack exclusively from a local XYZ PNG tree.

Security-sensitive filesystem operations intentionally support Linux only. PNG
support is intentionally limited to non-interlaced images.

Coverage models:
- default: PMPK v1 rectangle extents (contiguous rectangles, optional
  antimeridian splits)
- --sparse: PMPK v2 row spans for sparse state/polygon coverage, falling
  back to PMPK v3 indexless records when the coverage exceeds the 512-span
  cap
- --style: always emit an indexless PMPK v3 pack using the firmware's
  canonical style policy metadata; --activate additionally publishes the
  v3 active map-set record so the device renders the pack immediately
"""

from __future__ import annotations

import argparse
import contextlib
import ctypes
from dataclasses import dataclass
import errno
import fcntl
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys
import time
from typing import Any, cast, Iterable
import zlib

MAGIC = b"PMPK"
FORMAT_VERSION = 1
SPARSE_FORMAT_VERSION = 2
INDEXLESS_FORMAT_VERSION = 3
SELECTION_MAGIC = b"PMAS"
HEADER_SIZE = 16
EXTENT_SIZE = 26
ROW_SPAN_SIZE = 13
MAX_ROW_SPANS = 512
MAX_ZOOM = 22
MAX_ACTIVE_PACKS = 8  # firmware ActiveMapSetCodec::MAX_PACKS
DEFAULT_MAX_TILES = 100_000
DEFAULT_MAX_BYTES = 8 * 1024 * 1024 * 1024
MAX_VISITED_ENTRIES_BASE = 64
PACK_ID_RE = re.compile(r"[a-z0-9_-]{1,31}\Z")
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
_STRING_LIMITS = {"pack_id": 31, "name": 63, "attribution": 127, "source": 127, "license": 63,
                  "map_set_id": 31}
MAX_MANIFEST_BYTES = 7100
_DIRECTORY_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
_FILE_FLAGS = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC

if sys.platform.startswith("linux"):
    _libc = ctypes.CDLL(None, use_errno=True)
    _renameat2 = getattr(_libc, "renameat2", None)
    if _renameat2 is not None:
        _renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p,
                               ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
        _renameat2.restype = ctypes.c_int
else:
    _renameat2 = None


class PackError(ValueError):
    """Input cannot be represented as a safe, complete Pyxis map pack."""


class PublishedDurabilityError(PackError):
    """Publication committed, but syncing its parent directory failed."""

    published = True

    def __init__(self, target: Path, cause: OSError):
        self.target = target
        super().__init__(f"pack published at {target}, but durability is uncertain: {cause}")


@dataclass(frozen=True)
class ZoomExtent:
    zoom: int
    intervals: tuple[tuple[int, int], ...]
    y_minimum: int
    y_maximum: int


@dataclass(frozen=True)
class RowSpan:
    zoom: int
    y: int
    x_minimum: int
    x_maximum: int


@dataclass(frozen=True)
class Tile:
    zoom: int
    x: int
    y: int
    path: Path
    size: int
    zoom_identity: tuple[int, ...] = ()
    x_identity: tuple[int, ...] = ()
    file_identity: tuple[int, ...] = ()


def _identity(info: os.stat_result) -> tuple[int, ...]:
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def _checked_text(field: str, value: str) -> bytes:
    if not value:
        raise PackError(f"{field} metadata is required")
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as exc:
        raise PackError(f"{field} must contain printable ASCII") from exc
    if len(encoded) > _STRING_LIMITS[field] or any(byte < 0x20 or byte > 0x7E for byte in encoded):
        raise PackError(f"{field} must be 1-{_STRING_LIMITS[field]} printable ASCII bytes")
    return encoded


def _validate_metadata(pack_id: str, name: str, attribution: str, source: str, license: str) -> None:
    if not PACK_ID_RE.fullmatch(pack_id):
        raise PackError("pack ID must match [a-z0-9_-]{1,31}")
    for field, value in (("name", name), ("attribution", attribution), ("source", source), ("license", license)):
        _checked_text(field, value)


def _canonical_number(name: str, label: str) -> int:
    if not name.isascii() or not name.isdecimal() or str(int(name)) != name:
        raise PackError(f"noncanonical {label} path component: {name!r}")
    return int(name)


def _fsync_directory_fd(descriptor: int) -> None:
    os.fsync(descriptor)


def _open_path(directory: Path, *, create: bool = False) -> int:
    path = Path(directory)
    if path.is_absolute():
        descriptor = os.open("/", _DIRECTORY_FLAGS)
        parts = path.parts[1:]
    else:
        descriptor = os.open(".", _DIRECTORY_FLAGS)
        parts = path.parts
    try:
        for part in parts:
            if part in ("", "."):
                continue
            if part == "..":
                raise PackError(f"parent traversal is not allowed: {path}")
            try:
                child = os.open(part, _DIRECTORY_FLAGS, dir_fd=descriptor)
            except FileNotFoundError:
                if not create:
                    raise PackError(f"directory does not exist: {path}")
                os.mkdir(part, 0o755, dir_fd=descriptor)
                _fsync_directory_fd(descriptor)
                child = os.open(part, _DIRECTORY_FLAGS, dir_fd=descriptor)
            except OSError as exc:
                raise PackError(f"directory must be a real, nonsymlink directory: {path}: {exc}") from exc
            os.close(descriptor)
            descriptor = child
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


def _mkdir_open(parent_fd: int, name: str) -> tuple[int, tuple[int, ...]]:
    try:
        os.mkdir(name, 0o755, dir_fd=parent_fd)
        _fsync_directory_fd(parent_fd)
    except FileExistsError:
        pass
    try:
        descriptor = os.open(name, _DIRECTORY_FLAGS, dir_fd=parent_fd)
    except OSError as exc:
        raise PackError(f"output component is not a real directory: {name}: {exc}") from exc
    return descriptor, _identity(os.fstat(descriptor))


def _read_exact(descriptor: int, size: int, label: str) -> bytes:
    result = bytearray()
    while len(result) < size:
        block = os.read(descriptor, size - len(result))
        if not block:
            raise PackError(f"invalid PNG structure: {label}")
        result.extend(block)
    return bytes(result)


def _write_all(descriptor: int, data: bytes | memoryview, label: str) -> None:
    remaining = memoryview(data)
    while remaining:
        written = os.write(descriptor, remaining)
        if written <= 0:
            raise OSError(errno.EIO, f"short write while writing {label}")
        remaining = remaining[written:]


def _inflate_block(inflater: Any, block: bytes, decoded: bytearray,
                   expected: int, label: str) -> None:
    try:
        output = inflater.decompress(block, expected + 1 - len(decoded))
    except zlib.error as exc:
        raise PackError(f"invalid PNG deflate stream: {label}") from exc
    decoded.extend(output)
    if len(decoded) > expected or inflater.unconsumed_tail or inflater.unused_data:
        raise PackError(f"invalid PNG deflate length: {label}")


def _unfilter_png_rows(decoded: bytes, row_bytes: int, bytes_per_pixel: int,
                       label: str) -> bytes:
    stride = row_bytes + 1
    rows = len(decoded) // stride
    # Fast path: every row uses the None filter (the common case for
    # rendered tile PNGs). A single bytes scan is orders of magnitude
    # cheaper than the per-pixel predictor loop below.
    filter_bytes = bytes(decoded[offset] for offset in range(0, rows * stride, stride))
    if filter_bytes == b"\x00" * rows:
        output = bytearray()
        for offset in range(0, rows * stride, stride):
            output.extend(decoded[offset + 1:offset + stride])
        return bytes(output)
    output = bytearray()
    previous = bytearray(row_bytes)
    for offset in range(0, len(decoded), stride):
        filter_type = decoded[offset]
        if filter_type > 4:
            raise PackError(f"invalid PNG row filter: {label}")
        encoded = decoded[offset + 1:offset + stride]
        row = bytearray(row_bytes)
        for index, value in enumerate(encoded):
            left = row[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
            above = previous[index]
            upper_left = previous[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
            if filter_type == 0:
                predictor = 0
            elif filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = above
            elif filter_type == 3:
                predictor = (left + above) // 2
            else:
                estimate = left + above - upper_left
                distances = (abs(estimate - left), abs(estimate - above), abs(estimate - upper_left))
                predictor = (left, above, upper_left)[distances.index(min(distances))]
            row[index] = (value + predictor) & 0xff
        output.extend(row)
        previous = row
    return bytes(output)


def _validate_png_fd(descriptor: int, label: str, maximum_size: int) -> int:
    before = os.fstat(descriptor)
    if not stat.S_ISREG(before.st_mode):
        raise PackError(f"PNG must be a regular file: {label}")
    if before.st_size > maximum_size:
        raise PackError(f"total bytes exceed quota of {maximum_size}: {label}")
    os.lseek(descriptor, 0, os.SEEK_SET)
    if _read_exact(descriptor, 8, label) != PNG_SIGNATURE:
        raise PackError(f"invalid PNG signature: {label}")
    remaining = before.st_size - 8
    first = True
    saw_plte = False
    saw_idat = False
    idat_closed = False
    saw_iend = False
    inflater = zlib.decompressobj()
    decoded = bytearray()
    expected = row_bytes = color_type = bit_depth = bytes_per_pixel = 0
    palette_entries = 0

    while remaining:
        header = _read_exact(descriptor, 8, label)
        remaining -= 8
        length, kind = struct.unpack(">I4s", header)
        if length > remaining - 4:
            raise PackError(f"invalid PNG structure: {label}")
        if first and (kind != b"IHDR" or length != 13):
            raise PackError(f"invalid PNG IHDR: {label}")
        if any(not (65 <= byte <= 90 or 97 <= byte <= 122) for byte in kind) or kind[2] & 0x20:
            raise PackError(f"invalid PNG chunk type: {label}")
        if kind == b"PLTE" and length > 768:
            raise PackError(f"invalid PNG PLTE: {label}")
        if not first and kind == b"IHDR":
            raise PackError(f"invalid PNG critical chunk state: {label}")
        if kind not in (b"IHDR", b"PLTE", b"IDAT", b"IEND") and kind[0] & 0x20 == 0:
            raise PackError(f"unknown PNG critical chunk: {label}")
        if saw_idat and kind != b"IDAT" and kind != b"IEND":
            idat_closed = True
        if kind == b"IDAT" and idat_closed:
            raise PackError(f"nonconsecutive PNG IDAT chunks: {label}")

        crc = zlib.crc32(kind)
        payload = bytearray() if kind in (b"IHDR", b"PLTE") else None
        unread = length
        while unread:
            block = os.read(descriptor, min(unread, 1024 * 1024))
            if not block:
                raise PackError(f"invalid PNG structure: {label}")
            unread -= len(block)
            crc = zlib.crc32(block, crc)
            if payload is not None:
                payload.extend(block)
            if kind == b"IDAT":
                _inflate_block(inflater, block, decoded, expected, label)
        encoded_crc = _read_exact(descriptor, 4, label)
        remaining -= length + 4
        if crc != struct.unpack(">I", encoded_crc)[0]:
            raise PackError(f"invalid PNG chunk CRC: {label}")

        if kind == b"IHDR":
            assert payload is not None
            width, height, bit_depth, color_type, compression, filtering, interlace = struct.unpack(">IIBBBBB", payload)
            valid_depths = {0: {1, 2, 4, 8, 16}, 2: {8, 16}, 3: {1, 2, 4, 8}, 4: {8, 16}, 6: {8, 16}}
            if (width, height) != (256, 256):
                raise PackError(f"PNG must be 256x256: {label}")
            if (color_type not in valid_depths or bit_depth not in valid_depths[color_type]
                    or compression != 0 or filtering != 0 or interlace != 0):
                raise PackError(f"invalid or unsupported non-interlaced PNG IHDR: {label}")
            channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type]
            row_bytes = (width * channels * bit_depth + 7) // 8
            bytes_per_pixel = max(1, (channels * bit_depth + 7) // 8)
            expected = (row_bytes + 1) * height
            first = False
        elif kind == b"PLTE":
            if saw_plte or saw_idat or color_type in (0, 4) or payload is None:
                raise PackError(f"invalid PNG PLTE state: {label}")
            entries = len(payload) // 3
            if len(payload) == 0 or len(payload) % 3 or entries > 256 or (color_type == 3 and entries > (1 << bit_depth)):
                raise PackError(f"invalid PNG PLTE: {label}")
            saw_plte = True
            palette_entries = entries
        elif kind == b"IDAT":
            if color_type == 3 and not saw_plte:
                raise PackError(f"indexed PNG requires PLTE before IDAT: {label}")
            saw_idat = True
        elif kind == b"IEND":
            if length != 0 or not saw_idat or remaining != 0:
                raise PackError(f"invalid PNG IEND: {label}")
            saw_iend = True

    if first or not saw_iend or not inflater.eof or inflater.unused_data or len(decoded) != expected:
        raise PackError(f"invalid PNG structure, deflate, or terminal state: {label}")
    pixels = _unfilter_png_rows(bytes(decoded), row_bytes, bytes_per_pixel, label)
    if color_type == 3:
        if bit_depth == 8:
            # Fast path: one C-level scan replaces the 65,536-iteration
            # per-pixel loop. For 8-bit index PNGs the packed bytes ARE the
            # indices, so any byte at or above the palette length is invalid.
            # Single-pass scan via a lookup table: mark every out-of-range
            # index byte, then reject if any appear.
            bad = bytearray(256)
            for index in range(palette_entries, 256):
                bad[index] = 1
            if bytes.translate(pixels, bytes(bad)).count(1) > 0:
                raise PackError(f"indexed PNG palette index out of range: {label}")
        else:
            for row in range(256):
                packed = pixels[row * row_bytes:(row + 1) * row_bytes]
                for column in range(256):
                    bit_offset = column * bit_depth
                    byte = packed[bit_offset // 8]
                    shift = 8 - bit_depth - (bit_offset % 8)
                    if ((byte >> shift) & ((1 << bit_depth) - 1)) >= palette_entries:
                        raise PackError(f"indexed PNG palette index out of range: {label}")
    after = os.fstat(descriptor)
    if _identity(before) != _identity(after):
        raise PackError(f"PNG changed while being validated: {label}")
    return before.st_size


def _open_child_directory(parent_fd: int, name: str, label: str) -> tuple[int, tuple[int, ...]]:
    try:
        descriptor = os.open(name, _DIRECTORY_FLAGS, dir_fd=parent_fd)
    except OSError as exc:
        raise PackError(f"symlink or invalid source directory: {label}: {exc}") from exc
    return descriptor, _identity(os.fstat(descriptor))


def _discover_tiles_fd(source_fd: int, source_label: Path, max_tiles: int, max_bytes: int) -> list[Tile]:
    if max_tiles < 0 or max_bytes < 0:
        raise PackError("quotas must not be negative")
    if max_tiles > DEFAULT_MAX_TILES or max_bytes > DEFAULT_MAX_BYTES:
        raise PackError("quota exceeds importer hard limit")
    tiles: list[Tile] = []
    keys: set[tuple[int, int, int]] = set()
    total_bytes = 0
    visited = 0
    visit_limit = MAX_VISITED_ENTRIES_BASE + max_tiles * 3

    def visit() -> None:
        nonlocal visited
        visited += 1
        if visited > visit_limit:
            raise PackError(f"visited-entry quota exceeded ({visit_limit})")

    try:
        zoom_names = (entry.name for entry in os.scandir(source_fd))
        for zoom_name in zoom_names:
            visit()
            if zoom_name == "metadata.json":
                # Optional tile-provider metadata (e.g. Oxed's
                # metadata.json). The manifest carries its own style
                # metadata, so this file is validated as readable JSON and
                # otherwise ignored.
                try:
                    metadata_fd = os.open(zoom_name, _FILE_FLAGS, dir_fd=source_fd)
                except OSError as exc:
                    raise PackError(f"metadata.json is not a regular file: {exc}") from exc
                try:
                    metadata_info = os.fstat(metadata_fd)
                    if not stat.S_ISREG(metadata_info.st_mode) or metadata_info.st_size > 65536:
                        raise PackError("metadata.json is oversized")
                    try:
                        json.loads(os.read(metadata_fd, metadata_info.st_size).decode("utf-8"))
                    except (UnicodeDecodeError, ValueError) as exc:
                        raise PackError(f"metadata.json is not valid JSON: {exc}") from exc
                finally:
                    os.close(metadata_fd)
                continue
            zoom = _canonical_number(zoom_name, "zoom")
            if zoom > MAX_ZOOM:
                raise PackError(f"zoom out of range: {zoom}")
            zoom_fd, zoom_identity = _open_child_directory(source_fd, zoom_name, f"{source_label}/{zoom_name}")
            try:
                world_size = 1 << zoom
                for x_entry in os.scandir(zoom_fd):
                    visit()
                    x = _canonical_number(x_entry.name, "x")
                    if x >= world_size:
                        raise PackError(f"x out of range at zoom {zoom}: {x}")
                    x_fd, x_identity = _open_child_directory(zoom_fd, x_entry.name, f"{source_label}/{zoom}/{x}")
                    try:
                        for y_entry in os.scandir(x_fd):
                            visit()
                            if not y_entry.name.endswith(".png"):
                                raise PackError(f"unexpected source entry: {source_label}/{zoom}/{x}/{y_entry.name}")
                            y = _canonical_number(y_entry.name[:-4], "y")
                            if y >= world_size:
                                raise PackError(f"y out of range at zoom {zoom}: {y}")
                            if len(tiles) >= max_tiles:
                                raise PackError(f"tile count exceeds quota of {max_tiles}")
                            key = (zoom, x, y)
                            if key in keys:
                                raise PackError(f"duplicate tile key: {zoom}/{x}/{y}")
                            try:
                                file_fd = os.open(y_entry.name, _FILE_FLAGS, dir_fd=x_fd)
                            except OSError as exc:
                                raise PackError(f"symlink or invalid PNG: {source_label}/{zoom}/{x}/{y}.png: {exc}") from exc
                            try:
                                info = os.fstat(file_fd)
                                if not stat.S_ISREG(info.st_mode):
                                    raise PackError(f"PNG must be a regular file: {source_label}/{zoom}/{x}/{y}.png")
                                remaining_bytes = max_bytes - total_bytes
                                if info.st_size > remaining_bytes:
                                    raise PackError(f"total bytes exceed quota of {max_bytes}")
                                size = _validate_png_fd(file_fd, f"{source_label}/{zoom}/{x}/{y}.png", remaining_bytes)
                                identity = _identity(os.fstat(file_fd))
                            finally:
                                os.close(file_fd)
                            keys.add(key)
                            total_bytes += size
                            tiles.append(Tile(zoom, x, y, source_label / str(zoom) / str(x) / f"{y}.png",
                                              size, zoom_identity, x_identity, identity))
                    finally:
                        os.close(x_fd)
            finally:
                os.close(zoom_fd)
    except OSError as exc:
        raise PackError(f"cannot read source directory {source_label}: {exc}") from exc
    if not tiles:
        raise PackError("source contains no tiles")
    return sorted(tiles, key=lambda tile: (tile.zoom, tile.x, tile.y))


def discover_tiles(source_directory: Path, max_tiles: int, max_bytes: int) -> list[Tile]:
    descriptor = _open_path(Path(source_directory))
    try:
        return _discover_tiles_fd(descriptor, Path(source_directory), max_tiles, max_bytes)
    finally:
        os.close(descriptor)


def _runs(values: set[int]) -> tuple[tuple[int, int], ...]:
    ordered = sorted(values)
    runs: list[tuple[int, int]] = []
    start = previous = ordered[0]
    for value in ordered[1:]:
        if value != previous + 1:
            runs.append((start, previous))
            start = value
        previous = value
    runs.append((start, previous))
    return tuple(runs)


def calculate_extents(tiles: Iterable[Tile], antimeridian_zooms: set[int]) -> list[ZoomExtent]:
    by_zoom: dict[int, set[tuple[int, int]]] = {}
    for tile in tiles:
        by_zoom.setdefault(tile.zoom, set()).add((tile.x, tile.y))
    zooms = sorted(by_zoom)
    if zooms != list(range(zooms[0], zooms[-1] + 1)):
        raise PackError("zoom levels must be contiguous")
    if not antimeridian_zooms.issubset(by_zoom):
        raise PackError("antimeridian zoom option names a missing zoom")
    extents: list[ZoomExtent] = []
    for zoom in zooms:
        keys = by_zoom[zoom]
        xs = {key[0] for key in keys}
        ys = {key[1] for key in keys}
        x_runs = _runs(xs)
        y_runs = _runs(ys)
        if len(y_runs) != 1:
            raise PackError(f"incomplete rectangle at zoom {zoom}: noncontiguous y range")
        if zoom in antimeridian_zooms:
            world_maximum = (1 << zoom) - 1
            if len(x_runs) != 2 or x_runs[0][0] != 0 or x_runs[1][1] != world_maximum:
                raise PackError(f"ambiguous antimeridian split at zoom {zoom}")
            intervals = x_runs
        else:
            if len(x_runs) != 1:
                raise PackError(f"incomplete rectangle at zoom {zoom}: noncontiguous x range")
            intervals = x_runs
        expected = sum(last - first + 1 for first, last in intervals) * (y_runs[0][1] - y_runs[0][0] + 1)
        if len(keys) != expected:
            raise PackError(f"incomplete rectangle at zoom {zoom}: missing tile")
        extents.append(ZoomExtent(zoom, intervals, y_runs[0][0], y_runs[0][1]))
    return extents


def calculate_row_spans(tiles: Iterable[Tile]) -> list[RowSpan]:
    by_row: dict[tuple[int, int], set[int]] = {}
    zooms: set[int] = set()
    for tile in tiles:
        by_row.setdefault((tile.zoom, tile.y), set()).add(tile.x)
        zooms.add(tile.zoom)
    ordered_zooms = sorted(zooms)
    if not ordered_zooms or ordered_zooms != list(range(ordered_zooms[0], ordered_zooms[-1] + 1)):
        raise PackError("zoom levels must be contiguous")
    spans: list[RowSpan] = []
    for (zoom, y), xs in sorted(by_row.items()):
        for minimum, maximum in _runs(xs):
            spans.append(RowSpan(zoom, y, minimum, maximum))
            if len(spans) > MAX_ROW_SPANS:
                raise PackError(f"sparse coverage exceeds row-span limit of {MAX_ROW_SPANS}")
    return spans


def _validate_row_spans(row_spans: list[RowSpan], tile_count: int) -> None:
    if not row_spans or len(row_spans) > MAX_ROW_SPANS:
        raise PackError("manifest row-span count is invalid")
    zooms: set[int] = set()
    total = 0
    previous: RowSpan | None = None
    for span in row_spans:
        if span.zoom < 0 or span.zoom > MAX_ZOOM:
            raise PackError("invalid manifest row span")
        world_maximum = (1 << span.zoom) - 1
        if span.y < 0 or span.y > world_maximum or span.x_minimum < 0 \
                or span.x_minimum > span.x_maximum or span.x_maximum > world_maximum:
            raise PackError("invalid manifest row span")
        if previous is not None and ((span.zoom, span.y) < (previous.zoom, previous.y) or
                ((span.zoom, span.y) == (previous.zoom, previous.y) and
                 span.x_minimum <= previous.x_maximum + 1)):
            raise PackError("manifest row spans are not canonical")
        total += span.x_maximum - span.x_minimum + 1
        if total > 0xFFFFFFFF:
            raise PackError("manifest tile count overflows uint32")
        zooms.add(span.zoom)
        previous = span
    ordered_zooms = sorted(zooms)
    if ordered_zooms != list(range(ordered_zooms[0], ordered_zooms[-1] + 1)):
        raise PackError("manifest row spans must cover contiguous zooms")
    if tile_count <= 0 or tile_count != total:
        raise PackError("invalid manifest tile count")


def serialize_sparse_manifest(*, pack_id: str, name: str, attribution: str, source: str,
                              license: str, row_spans: list[RowSpan], tile_count: int) -> bytes:
    _validate_metadata(pack_id, name, attribution, source, license)
    _validate_row_spans(row_spans, tile_count)
    payload = bytearray()
    for field, value in (("pack_id", pack_id), ("name", name), ("attribution", attribution),
                         ("source", source), ("license", license)):
        encoded = _checked_text(field, value)
        payload.append(len(encoded))
        payload.extend(encoded)
    payload.extend((row_spans[0].zoom, row_spans[-1].zoom))
    payload.extend(struct.pack("<HI", len(row_spans), tile_count))
    for span in row_spans:
        payload.extend(struct.pack("<BIII", span.zoom, span.y, span.x_minimum, span.x_maximum))
    length = HEADER_SIZE + len(payload) + 4
    result = bytearray(struct.pack("<4sBBHII", MAGIC, SPARSE_FORMAT_VERSION, 0, HEADER_SIZE, length, 0))
    result.extend(payload)
    result.extend(struct.pack("<I", zlib.crc32(result)))
    return bytes(result)


def serialize_indexless_manifest(*, pack_id: str, name: str, attribution: str, source: str,
                                 license: str, min_zoom: int, max_zoom: int,
                                 tile_count: int) -> bytes:
    _validate_metadata(pack_id, name, attribution, source, license)
    if min_zoom < 0 or min_zoom > max_zoom or max_zoom > MAX_ZOOM or tile_count <= 0:
        raise PackError("indexless manifest range or tile count is invalid")
    payload = bytearray()
    for field, value in (("pack_id", pack_id), ("name", name), ("attribution", attribution),
                         ("source", source), ("license", license)):
        encoded = _checked_text(field, value)
        payload.append(len(encoded))
        payload.extend(encoded)
    payload.extend((min_zoom, max_zoom))
    payload.extend(struct.pack("<I", tile_count))
    length = HEADER_SIZE + len(payload) + 4
    result = bytearray(struct.pack("<4sBBHII", MAGIC, INDEXLESS_FORMAT_VERSION, 0, HEADER_SIZE, length, 0))
    result.extend(payload)
    result.extend(struct.pack("<I", zlib.crc32(result)))
    return bytes(result)


def _validate_manifest_values(extents: list[ZoomExtent], tile_count: int) -> None:
    if not extents or len(extents) > MAX_ZOOM + 1:
        raise PackError("manifest extent count is invalid")
    if extents[0].zoom < 0 or extents[-1].zoom > MAX_ZOOM or [item.zoom for item in extents] != list(
            range(extents[0].zoom, extents[-1].zoom + 1)):
        raise PackError("manifest extents must cover contiguous valid zooms")
    total = 0
    for extent in extents:
        world_maximum = (1 << extent.zoom) - 1
        if len(extent.intervals) not in (1, 2) or extent.y_minimum < 0 or extent.y_minimum > extent.y_maximum \
                or extent.y_maximum > world_maximum:
            raise PackError("invalid manifest extent")
        columns = 0
        for minimum, maximum in extent.intervals:
            if minimum < 0 or minimum > maximum or maximum > world_maximum:
                raise PackError("invalid manifest extent")
            columns += maximum - minimum + 1
        if len(extent.intervals) == 2:
            first, second = extent.intervals
            if first[0] != 0 or second[1] != world_maximum or first[1] >= second[0] or first[1] + 1 == second[0]:
                raise PackError("invalid manifest antimeridian intervals")
        level = columns * (extent.y_maximum - extent.y_minimum + 1)
        if level > 0xFFFFFFFF or total > 0xFFFFFFFF - level:
            raise PackError("manifest tile count overflows uint32")
        total += level
    if tile_count <= 0 or tile_count > 0xFFFFFFFF or total != tile_count:
        raise PackError("invalid manifest tile count")


def serialize_manifest(*, pack_id: str, name: str, attribution: str, source: str,
                       license: str, extents: list[ZoomExtent], tile_count: int) -> bytes:
    _validate_metadata(pack_id, name, attribution, source, license)
    _validate_manifest_values(extents, tile_count)
    payload = bytearray()
    for field, value in (("pack_id", pack_id), ("name", name), ("attribution", attribution),
                         ("source", source), ("license", license)):
        encoded = _checked_text(field, value)
        payload.append(len(encoded))
        payload.extend(encoded)
    payload.extend((extents[0].zoom, extents[-1].zoom, len(extents)))
    payload.extend(struct.pack("<I", tile_count))
    for extent in extents:
        intervals = extent.intervals + (((0, 0),) if len(extent.intervals) == 1 else ())
        payload.extend((extent.zoom, len(extent.intervals)))
        payload.extend(struct.pack("<II", extent.y_minimum, extent.y_maximum))
        for minimum, maximum in intervals:
            payload.extend(struct.pack("<II", minimum, maximum))
    length = HEADER_SIZE + len(payload) + 4
    result = bytearray(struct.pack("<4sBBHII", MAGIC, FORMAT_VERSION, 0, HEADER_SIZE, length, 0))
    result.extend(payload)
    result.extend(struct.pack("<I", zlib.crc32(result)))
    return bytes(result)


def parse_manifest(data: bytes) -> dict[str, object]:
    if len(data) < 20 or len(data) > MAX_MANIFEST_BYTES:
        raise PackError("manifest is truncated or oversized")
    magic, version, reserved, header_size, length, reserved_word = struct.unpack("<4sBBHII", data[:16])
    if magic != MAGIC or version not in (FORMAT_VERSION, SPARSE_FORMAT_VERSION,
                                         INDEXLESS_FORMAT_VERSION) or \
            (reserved, header_size, length, reserved_word) != (0, 16, len(data), 0):
        raise PackError("invalid manifest header")
    if zlib.crc32(data[:-4]) != struct.unpack("<I", data[-4:])[0]:
        raise PackError("invalid manifest CRC")
    position = 16
    values: dict[str, object] = {}
    for field in ("pack_id", "name", "attribution", "source", "license"):
        if position >= len(data) - 4:
            raise PackError("manifest string is truncated")
        size = data[position]
        position += 1
        if size > len(data) - 4 - position:
            raise PackError("manifest string is truncated")
        try:
            value = data[position:position + size].decode("ascii")
        except UnicodeDecodeError as exc:
            raise PackError("invalid manifest string") from exc
        position += size
        _checked_text(field, value)
        if field == "pack_id" and not PACK_ID_RE.fullmatch(value):
            raise PackError("invalid manifest pack ID")
        values[field] = value
    if version == INDEXLESS_FORMAT_VERSION:
        if position + 6 > len(data) - 4:
            raise PackError("manifest indexless fields are truncated")
        minimum, maximum = data[position:position + 2]
        position += 2
        tile_count = struct.unpack("<I", data[position:position + 4])[0]
        position += 4
        if position != len(data) - 4 or minimum > maximum or maximum > MAX_ZOOM or not tile_count:
            raise PackError("invalid indexless manifest fields")
        values.update(format_version=version, min_zoom=minimum, max_zoom=maximum,
                      tile_count=tile_count, extents=[], row_spans=[])
        return values
    if version == FORMAT_VERSION:
        if position + 7 > len(data) - 4:
            raise PackError("manifest fields are truncated")
        minimum, maximum, count = data[position:position + 3]
        position += 3
        tile_count = struct.unpack("<I", data[position:position + 4])[0]
        position += 4
        if count > MAX_ZOOM + 1 or count > (len(data) - 4 - position) // EXTENT_SIZE:
            raise PackError("invalid manifest extent count")
        extents: list[ZoomExtent] = []
        for _ in range(count):
            zoom, interval_count = data[position:position + 2]
            y_minimum, y_maximum, x0, x1, x2, x3 = struct.unpack("<IIIIII", data[position + 2:position + 26])
            position += EXTENT_SIZE
            if interval_count not in (1, 2) or (interval_count == 1 and (x2 != 0 or x3 != 0)):
                raise PackError("invalid manifest extent")
            intervals = ((x0, x1),) if interval_count == 1 else ((x0, x1), (x2, x3))
            extents.append(ZoomExtent(zoom, intervals, y_minimum, y_maximum))
        if position != len(data) - 4 or not extents or minimum != extents[0].zoom or maximum != extents[-1].zoom:
            raise PackError("invalid manifest length or zoom range")
        _validate_manifest_values(extents, tile_count)
        values.update(format_version=version, min_zoom=minimum, max_zoom=maximum,
                      tile_count=tile_count, extents=extents, row_spans=[])
    else:
        if position + 8 > len(data) - 4:
            raise PackError("manifest sparse fields are truncated")
        minimum, maximum = data[position:position + 2]
        position += 2
        count, tile_count = struct.unpack("<HI", data[position:position + 6])
        position += 6
        if count == 0 or count > MAX_ROW_SPANS or count > (len(data) - 4 - position) // ROW_SPAN_SIZE:
            raise PackError("invalid manifest row-span count")
        row_spans: list[RowSpan] = []
        for _ in range(count):
            zoom, y, x_minimum, x_maximum = struct.unpack("<BIII", data[position:position + ROW_SPAN_SIZE])
            position += ROW_SPAN_SIZE
            row_spans.append(RowSpan(zoom, y, x_minimum, x_maximum))
        if position != len(data) - 4 or minimum != row_spans[0].zoom or maximum != row_spans[-1].zoom:
            raise PackError("invalid manifest length or zoom range")
        _validate_row_spans(row_spans, tile_count)
        values.update(format_version=version, min_zoom=minimum, max_zoom=maximum,
                      tile_count=tile_count, extents=[], row_spans=row_spans)
    return values


def encode_active_map_set(*, generation: int, map_set_id: str, attribution: str,
                          pack_ids: list[str]) -> bytes:
    """Serialize a v3 (indexless) PMAS active map-set record.

    Byte-compatible with the web flasher's encodeActiveMapSet and the
    firmware's ActiveMapSetCodec (PMAS magic, version 3, u16 total length,
    u32 generation, then length-prefixed map-set ID, attribution, pack
    count, and ordered pack IDs, trailing CRC-32).
    """
    if not 1 <= generation <= 0xFFFFFFFF:
        raise PackError("active map-set generation is invalid")
    map_set = _checked_text("map_set_id", map_set_id)
    credit = _checked_text("attribution", attribution)
    if not PACK_ID_RE.fullmatch(map_set_id) or not pack_ids or len(pack_ids) > MAX_ACTIVE_PACKS:
        raise PackError(f"invalid active map-set packs (limit {MAX_ACTIVE_PACKS})")
    seen: set[str] = set()
    encoded_packs: list[bytes] = []
    for pack_id in pack_ids:
        identifier = _checked_text("pack_id", pack_id)
        if not PACK_ID_RE.fullmatch(pack_id) or pack_id in seen:
            raise PackError("invalid or duplicate active map-set pack")
        seen.add(pack_id)
        encoded_packs.append(identifier)
    length = 12 + 1 + len(map_set) + 1 + len(credit) + 1 + sum(1 + len(item) for item in encoded_packs) + 4
    result = bytearray(struct.pack("<4sBBHI", SELECTION_MAGIC, INDEXLESS_FORMAT_VERSION, 0, length, generation))
    result.append(len(map_set)); result.extend(map_set)
    result.append(len(credit)); result.extend(credit)
    result.append(len(encoded_packs))
    for identifier in encoded_packs:
        result.append(len(identifier)); result.extend(identifier)
    result.extend(struct.pack("<I", zlib.crc32(result)))
    return bytes(result)


def parse_active_map_set(data: bytes) -> dict[str, object]:
    if len(data) < 16 or len(data) > 7105:
        raise PackError("active map-set record length is invalid")
    magic, version, reserved, length, generation = struct.unpack("<4sBBHI", data[:12])
    if magic != SELECTION_MAGIC or version != INDEXLESS_FORMAT_VERSION or reserved != 0 \
            or length != len(data) or generation == 0 \
            or zlib.crc32(data[:-4]) != struct.unpack("<I", data[-4:])[0]:
        raise PackError("invalid active map-set record")
    position = 12

    def read_text(label: str, maximum: int, identifier: bool) -> str:
        nonlocal position
        if position >= len(data) - 4:
            raise PackError(f"{label} is truncated")
        size = data[position]
        position += 1
        if size == 0 or size > maximum or position + size > len(data) - 4:
            raise PackError(f"{label} is invalid")
        value = data[position:position + size].decode("ascii")
        position += size
        if any(byte < 0x20 or byte > 0x7E for byte in value.encode("ascii")) or \
                (identifier and not PACK_ID_RE.fullmatch(value)):
            raise PackError(f"{label} is invalid")
        return value

    map_set_id = read_text("map set ID", 31, True)
    attribution = read_text("attribution", 127, False)
    if position >= len(data) - 4:
        raise PackError("active map-set pack count is truncated")
    count = data[position]
    position += 1
    if count == 0 or count > 8:
        raise PackError("active map-set pack count is invalid")
    packs: list[str] = []
    for _ in range(count):
        pack_id = read_text("pack ID", 31, True)
        if pack_id in packs:
            raise PackError("duplicate active map-set pack")
        packs.append(pack_id)
    if position != len(data) - 4:
        raise PackError("active map-set record has trailing data")
    return {"format_version": version, "generation": generation, "map_set_id": map_set_id,
            "attribution": attribution, "packs": packs}


def _open_verified_tile(source_fd: int, tile: Tile) -> int:
    zoom_fd, zoom_identity = _open_child_directory(source_fd, str(tile.zoom), str(tile.path.parent.parent))
    try:
        if zoom_identity != tile.zoom_identity:
            raise PackError(f"source directory changed after validation: {tile.path}")
        x_fd, x_identity = _open_child_directory(zoom_fd, str(tile.x), str(tile.path.parent))
        try:
            if x_identity != tile.x_identity:
                raise PackError(f"source directory changed after validation: {tile.path}")
            try:
                descriptor = os.open(f"{tile.y}.png", _FILE_FLAGS, dir_fd=x_fd)
            except OSError as exc:
                raise PackError(f"source PNG changed, became a symlink, or is invalid: {tile.path}: {exc}") from exc
            if _identity(os.fstat(descriptor)) != tile.file_identity:
                os.close(descriptor)
                raise PackError(f"source PNG changed after validation: {tile.path}")
            return descriptor
        finally:
            os.close(x_fd)
    finally:
        os.close(zoom_fd)


def _ensure_stage_path(stage_fd: int, tile: Tile) -> tuple[int, str]:
    tiles_fd, _ = _mkdir_open(stage_fd, "tiles")
    try:
        zoom_fd, _ = _mkdir_open(tiles_fd, str(tile.zoom))
        try:
            x_fd, _ = _mkdir_open(zoom_fd, str(tile.x))
            return x_fd, f"{tile.y}.png"
        finally:
            os.close(zoom_fd)
    finally:
        os.close(tiles_fd)


def copy_tile(source_fd: int, tile: Tile, stage_fd: int, maximum_bytes: int) -> None:
    input_fd = _open_verified_tile(source_fd, tile)
    output_parent_fd = -1
    output_fd = -1
    try:
        output_parent_fd, output_name = _ensure_stage_path(stage_fd, tile)
        output_fd = os.open(output_name, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC,
                            0o644, dir_fd=output_parent_fd)
        copied = 0
        while True:
            block = os.read(input_fd, min(1024 * 1024, maximum_bytes + 1 - copied))
            if not block:
                break
            copied += len(block)
            if copied > maximum_bytes:
                raise PackError(f"total bytes exceed quota of {maximum_bytes}")
            _write_all(output_fd, block, f"staged {tile.zoom}/{tile.x}/{tile.y}.png")
        if copied != tile.size or _identity(os.fstat(input_fd)) != tile.file_identity:
            raise PackError(f"source PNG changed while copying: {tile.path}")
        _validate_png_fd(output_fd, f"staged {tile.zoom}/{tile.x}/{tile.y}.png", maximum_bytes)
        os.fsync(output_fd)
        _fsync_directory_fd(output_parent_fd)
    finally:
        if output_fd >= 0:
            os.close(output_fd)
        if output_parent_fd >= 0:
            os.close(output_parent_fd)
        os.close(input_fd)


def _read_file_at(parent_fd: int, name: str, maximum: int) -> bytes:
    descriptor = os.open(name, _FILE_FLAGS, dir_fd=parent_fd)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_size > maximum:
            raise PackError(f"invalid or oversized file: {name}")
        return _read_exact(descriptor, info.st_size, name)
    finally:
        os.close(descriptor)


def _read_selection_at(parent_fd: int, name: str) -> bytes | None:
    try:
        return _read_file_at(parent_fd, name, 7105)
    except FileNotFoundError:
        return None
    except OSError as exc:
        raise PackError(f"cannot read {name}: {exc}") from exc


def decode_active_selection(data: bytes) -> dict[str, object]:
    """Decode v1/v2/v3 PMAS active map-set records.

    Byte-compatible with the firmware's ActiveMapSetCodec and the web
    flasher's decodeActiveSelection. Row spans in v2 records are skipped
    with bound checks only; this tool never emits spans.
    """
    if len(data) < 16 or len(data) > 7105:
        raise PackError("active map-set record length is invalid")
    magic, version, reserved, length, generation = struct.unpack("<4sBBHI", data[:12])
    if magic != SELECTION_MAGIC or version not in (1, 2, 3) or reserved != 0 \
            or length != len(data) or generation == 0 \
            or zlib.crc32(data[:-4]) != struct.unpack("<I", data[-4:])[0]:
        raise PackError("invalid active map-set record")
    end = len(data) - 4
    if version == 1:
        if len(data) != 48:
            raise PackError("invalid v1 active map-set length")
        size = data[12]
        if not 1 <= size <= 31:
            raise PackError("invalid v1 active map-set pack ID")
        pack_id = data[13:13 + size].decode("ascii")
        if any(byte != 0 for byte in data[13 + size:end]) or not PACK_ID_RE.fullmatch(pack_id):
            raise PackError("invalid v1 active map-set record")
        return {"format_version": 1, "generation": generation, "map_set_id": None,
                "attribution": None, "packs": [pack_id]}
    position = 12

    def read_text(label: str, maximum: int, identifier: bool) -> str:
        nonlocal position
        if position >= end:
            raise PackError(f"{label} is truncated")
        size = data[position]
        position += 1
        if size == 0 or size > maximum or position + size > end:
            raise PackError(f"{label} is invalid")
        value = data[position:position + size].decode("ascii")
        position += size
        if any(byte < 0x20 or byte > 0x7E for byte in value.encode("ascii")) or \
                (identifier and not PACK_ID_RE.fullmatch(value)):
            raise PackError(f"{label} is invalid")
        return value

    map_set_id = read_text("map set ID", 31, True)
    attribution = read_text("attribution", 127, False)
    if position >= end:
        raise PackError("active map-set pack count is truncated")
    count = data[position]
    position += 1
    if count == 0 or count > 8:
        raise PackError("active map-set pack count is invalid")
    packs: list[str] = []
    total_spans = 0
    for _ in range(count):
        pack_id = read_text("pack ID", 31, True)
        if pack_id in packs:
            raise PackError("duplicate active map-set pack")
        packs.append(pack_id)
        if version == 3:
            continue
        if position + 2 > end:
            raise PackError("active map-set span count is truncated")
        span_count = struct.unpack("<H", data[position:position + 2])[0]
        position += 2
        if span_count == 0 or span_count > MAX_ROW_SPANS or total_spans > MAX_ROW_SPANS - span_count \
                or position + span_count * ROW_SPAN_SIZE > end:
            raise PackError("active map-set row-span count is invalid")
        total_spans += span_count
        position += span_count * ROW_SPAN_SIZE
    if position != end:
        raise PackError("active map-set record has trailing data")
    return {"format_version": version, "generation": generation, "map_set_id": map_set_id,
            "attribution": attribution, "packs": packs}


def _validate_pack_fd(pack_fd: int, label: Path, max_bytes: int) -> dict[str, object]:
    parsed = parse_manifest(_read_file_at(pack_fd, "manifest.pmp", MAX_MANIFEST_BYTES))
    tiles_fd, _ = _open_child_directory(pack_fd, "tiles", f"{label}/tiles")
    try:
        tile_count = cast(int, parsed["tile_count"])
        tiles = _discover_tiles_fd(tiles_fd, label / "tiles", tile_count, max_bytes)
    finally:
        os.close(tiles_fd)
    if len(tiles) != tile_count:
        raise PackError("emitted tile tree does not match manifest")
    if parsed["format_version"] == INDEXLESS_FORMAT_VERSION:
        zooms = sorted({tile.zoom for tile in tiles})
        if (cast(int, parsed["min_zoom"]), cast(int, parsed["max_zoom"])) != (zooms[0], zooms[-1]) \
                or zooms != list(range(zooms[0], zooms[-1] + 1)):
            raise PackError("emitted tile tree does not match indexless manifest")
        return parsed
    if parsed["format_version"] == FORMAT_VERSION:
        expected_extents = cast(list[ZoomExtent], parsed["extents"])
        extents = calculate_extents(tiles, {item.zoom for item in expected_extents if len(item.intervals) == 2})
        if extents != expected_extents:
            raise PackError("emitted tile tree does not match manifest")
    else:
        expected_spans = cast(list[RowSpan], parsed["row_spans"])
        if calculate_row_spans(tiles) != expected_spans:
            raise PackError("emitted tile tree does not match sparse manifest")
    return parsed


def validate_pack(pack_directory: Path, max_bytes: int = DEFAULT_MAX_BYTES) -> dict[str, object]:
    descriptor = _open_path(Path(pack_directory))
    try:
        return _validate_pack_fd(descriptor, Path(pack_directory), max_bytes)
    finally:
        os.close(descriptor)


def _existing_pack_fd(packs_fd: int, pack_id: str) -> int | None:
    """Open a published pack directory if it exists; None otherwise."""
    try:
        return os.open(pack_id, _DIRECTORY_FLAGS, dir_fd=packs_fd)
    except FileNotFoundError:
        return None
    except OSError as exc:
        raise PackError(f"existing pack is not a real directory: {pack_id}: {exc}") from exc


def _verify_existing_pack(source_fd: int, pack_fd: int, pack_id: str,
                          expected_manifest: bytes, tiles: list[Tile],
                          max_bytes: int) -> None:
    """Verify a published pack byte-for-byte against the source tiles.

    Mirrors the web flasher's verifyExistingPack: a pack that already
    exists may only be resumed when its manifest and every tile match the
    input exactly, so a resume never republishes different content under
    an existing name.
    """
    try:
        raw_manifest = _read_file_at(pack_fd, "manifest.pmp", MAX_MANIFEST_BYTES)
    except FileNotFoundError:
        raise PackError(f"existing pack {pack_id} is incomplete: manifest.pmp missing") from None
    if raw_manifest != expected_manifest:
        raise PackError(f"existing pack {pack_id} does not match this source and "
                        "cannot be resumed")
    tiles_fd, _ = _open_child_directory(pack_fd, "tiles", f"{pack_id}/tiles")
    try:
        actual = _discover_tiles_fd(tiles_fd, Path(pack_id) / "tiles", len(tiles), max_bytes)
    finally:
        os.close(tiles_fd)
    if len(actual) != len(tiles):
        raise PackError(f"existing pack {pack_id} tile count does not match this source")
    input_by_key = {(tile.zoom, tile.x, tile.y): tile for tile in tiles}
    for actual_tile in actual:
        key = (actual_tile.zoom, actual_tile.x, actual_tile.y)
        source_tile = input_by_key.get(key)
        if source_tile is None:
            raise PackError(f"existing pack {pack_id} contains a tile this source does not")
        existing_bytes = _read_file_at(pack_fd, f"tiles/{actual_tile.zoom}/"
                                                f"{actual_tile.x}/{actual_tile.y}.png", max_bytes)
        input_fd = _open_verified_tile(source_fd, source_tile)
        try:
            input_bytes = _read_exact(input_fd, source_tile.size, str(source_tile.path))
        finally:
            os.close(input_fd)
        if existing_bytes != input_bytes:
            raise PackError(f"existing pack {pack_id} tile {actual_tile.zoom}/"
                            f"{actual_tile.x}/{actual_tile.y}.png differs from this source")


STYLE_POLICIES: dict[str, dict[str, str]] = {
    "osm-bright": {
        "attribution": "(c) OpenMapTiles (c) OpenStreetMap contributors",
        "source": "Oxed's Map Tile Downloader (OSM Bright)",
        "license": "OSM ODbL; style CC-BY-4.0/BSD-3-Clause",
    },
    "dark-matter": {
        "attribution": "(c) OpenMapTiles (c) OpenStreetMap contributors; style (c) CARTO",
        "source": "Oxed's Map Tile Downloader (Dark Matter)",
        "license": "OSM ODbL; style CC-BY-4.0/BSD-3-Clause (CARTO CC-BY-3.0)",
    },
    "positron": {
        "attribution": "(c) OpenMapTiles (c) OpenStreetMap contributors; style (c) CARTO",
        "source": "Oxed's Map Tile Downloader (Positron)",
        "license": "OSM ODbL; style CC-BY-4.0/BSD-3-Clause (CARTO CC-BY-3.0)",
    },
    "toner": {
        "attribution": "(c) MapTiler (c) OpenStreetMap contributors",
        "source": "Oxed's Map Tile Downloader (Toner)",
        "license": "OSM ODbL; style CC-BY-4.0/BSD-3-Clause (Stamen ISC)",
    },
}


def activate_map_set(pyxis_fd: int, *, pack_id: str, map_set_id: str, attribution: str,
                     marker_token: str | None = None) -> tuple[str, list[str]]:
    """Publish the v3 active map-set record, mirroring the web flasher.

    pyxis_fd is the open pyxis-map/ directory. Slot and style-record
    selection follows the flasher's prepareActiveMapSet/activateMapSet:
    read both slots, reject conflicting same-generation records, resolve
    the pack list from the installed style record (falling back to the
    highest-generation slot of the same style), then publish the record to
    the style record under map-sets/ first and the chosen active-pack slot
    second. Both writes go through _write_atomic_at (exclusive temp file,
    fsync, rename, read-back) so an interrupted activation never truncates
    or leaves a partial record: a crash mid-write leaves the previous
    record intact and a retry (or the device's own style re-activation)
    completes the activation. The style record is written first because the
    firmware's device-side style activation (MapStyleCatalog::activate)
    resequences the style record's composition into the active slot, so the
    style record must never be older than the slot -- otherwise a stale
    style record would reseed the slot and drop a newly installed pack.
    The final pack list is validated against the firmware's pack limit
    before any byte is written, so a rejected activation cannot orphan a
    published pack.

    marker_token is the caller's cross-producer install-marker token (see
    _acquire_install_marker). When given, the raw slot and style-record
    bytes are captured when they are first read and re-read immediately
    before the record writes: if another installer (the web flasher holds
    a Web Locks name, which this process's flock cannot see) committed new
    activation state -- or holds a fresh install marker that is not ours --
    in the meantime, the publication is aborted before any byte is written.
    The already-published pack is kept; a retry re-derives from the new
    state and converges, and a pack that no record names is device-harmless
    (MapTilePack only reads packs enumerated in the active selection).
    Returns (slot_name, enabled_packs).
    """
    slot_names = ("active-pack.0", "active-pack.1")
    slots: list[dict[str, object] | None] = [None, None]
    slot_raw: list[bytes | None] = [None, None]
    for index, slot_name in enumerate(slot_names):
        raw = _read_selection_at(pyxis_fd, slot_name)
        slot_raw[index] = raw
        if raw is not None:
            slots[index] = decode_active_selection(raw)
    if slots[0] is not None and slots[1] is not None \
            and slots[0]["generation"] == slots[1]["generation"] and slots[0] != slots[1]:
        raise PackError("conflicting active map-set records")
    valid = [slot for slot in slots if slot is not None]
    highest = max([0] + [cast(int, slot["generation"]) for slot in valid])
    if highest == 0xFFFFFFFF:
        raise PackError("active selection generation is exhausted")
    current = max(valid, key=lambda slot: cast(int, slot["generation"])) if valid else None

    map_sets_fd, _ = _mkdir_open(pyxis_fd, "map-sets")
    try:
        style_name = f"{map_set_id}.pmas"
        installed: dict[str, object] | None = None
        installed_raw = _read_selection_at(map_sets_fd, style_name)
        if installed_raw is not None:
            installed = decode_active_selection(installed_raw)
            if installed["format_version"] not in (2, 3) \
                    or installed["map_set_id"] != map_set_id \
                    or installed["attribution"] != attribution:
                raise PackError("installed style record does not match selected map set")
        composition: dict[str, object] | None = installed
        if composition is None and current is not None \
                and current["format_version"] in (2, 3) and current["map_set_id"] == map_set_id:
            composition = current
        packs: list[str] = []
        if composition is not None:
            if composition["attribution"] != attribution:
                raise PackError("installed map set attribution does not match")
            packs = [value for value in cast(list[str], composition["packs"]) if value != pack_id]
        packs.insert(0, pack_id)
        if len(packs) > MAX_ACTIVE_PACKS:
            raise PackError(f"active map-set pack limit exceeded: {len(packs)} > "
                            f"{MAX_ACTIVE_PACKS}; remove an installed pack before activating")
        record = encode_active_map_set(generation=highest + 1, map_set_id=map_set_id,
                                       attribution=attribution, pack_ids=packs)
        if slots[0] is None:
            target = slot_names[0]
        elif slots[1] is None:
            target = slot_names[1]
        else:
            target = slot_names[0] if cast(int, slots[0]["generation"]) <= \
                    cast(int, slots[1]["generation"]) else slot_names[1]
        # Commit-time revalidation: the slot/style records were derived
        # above; a concurrent cross-producer installer (the web flasher's
        # Web Locks name is invisible to our flock) may have committed new
        # activation state in the meantime. Re-reading the raw bytes here
        # and aborting on any change guarantees this publication never
        # overwrites a newer record with one derived from stale state.
        # installed_raw is the style record's bytes as first read above.
        _verify_activation_state_at(pyxis_fd, map_sets_fd, slot_raw=slot_raw,
                                    style_raw=installed_raw, style_name=style_name,
                                    marker_token=marker_token)
        # Style record first, active slot second (the web flasher's order).
        # The firmware's device-side style activation
        # (MapStyleCatalog::activate) reads the style record as the source
        # of truth and resequences its composition into the active slot.
        # If the style record were ever older than the slot, that path
        # would reseed the slot from the stale composition and drop a
        # newly installed pack. Writing the style record first guarantees
        # the style record is never older than the slot, so every recovery
        # path (CLI retry, device-side style re-activation) converges
        # forward to the newest composition. The worst interruption is
        # "slot one build stale, style record current" -- the map still
        # serves from the previous pack and the next style cycle or
        # builder retry completes the new activation. Both records carry
        # the identical PMAS payload.
        _write_atomic_at(map_sets_fd, style_name, record)
        _write_atomic_at(pyxis_fd, target, record)
    finally:
        os.close(map_sets_fd)
    return target, packs


def _write_atomic_at(parent_fd: int, name: str, data: bytes) -> None:
    """Atomically replace ``name`` with ``data`` using an exclusive temp file.

    Writes to a per-process temp name, fsyncs it, then renameat()s it over
    the destination. Because rename is atomic, a crash at any point leaves
    either the old record (temp not yet renamed) or the new record (rename
    done) in place -- never a truncated or partial one. The temp file is
    created with O_EXCL so concurrent activations cannot clobber each
    other's staging. Activation correctness depends on atomicity, so an
    unsupported host fails closed.
    """
    if _renameat2 is None:
        raise PackError("atomic record replacement unsupported on this host")
    marker = os.getpid()
    for attempt in range(256):
        temp_name = f".{name}.tmp-{marker:x}-{attempt:02x}"
        try:
            descriptor = os.open(temp_name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC,
                                 0o600, dir_fd=parent_fd)
        except FileExistsError:
            continue
        except OSError as exc:
            raise PackError(f"cannot write {name}: {exc}") from exc
        break
    else:
        raise PackError(f"cannot allocate temporary record file for {name}")
    try:
        _write_all(descriptor, data, name)
        os.fsync(descriptor)
    except Exception as exc:
        os.close(descriptor)
        _unlink_quietly_at(parent_fd, temp_name)
        raise PackError(f"cannot write {name}: {exc}") from exc
    os.close(descriptor)
    try:
        result = _renameat2(parent_fd, os.fsencode(temp_name), parent_fd, os.fsencode(name), 0)
        if result != 0:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error), name)
        _fsync_directory_fd(parent_fd)
    except Exception as exc:
        _unlink_quietly_at(parent_fd, temp_name)
        raise PackError(f"cannot atomically replace {name}: {exc}") from exc
    actual = _read_file_at(parent_fd, name, len(data))
    if actual != data:
        raise PackError(f"read-back verification failed for {name}")


def _unlink_quietly_at(parent_fd: int, name: str) -> None:
    try:
        os.unlink(name, dir_fd=parent_fd)
    except OSError:
        pass


_INSTALL_LOCK_NAME = ".pyxis-install.lock"


@contextlib.contextmanager
def _install_lock_at(pyxis_fd: int):
    """Hold the exclusive on-disk install lock across preflight-to-activation.

    The web flasher serializes concurrent installers through the Web Locks
    API; this CLI needs an equivalent because two concurrent --activate
    builds could otherwise both pass the preflight and each clobber the
    other's records.

    The lock is an advisory fcntl.flock(LOCK_EX | LOCK_NB) on a single
    well-known file in pyxis-map/, so every builder on the card serializes
    on the same inode. flock is the race-free primitive here:
      * acquisition is a single atomic kernel operation, so there is no
        create-then-write-PID window for a second builder to observe an
        empty/partial lock and classify it as stale;
      * the kernel releases the lock when the holder exits or is killed,
        so a crashed builder cannot wedge the card -- there is no stale
        lock to reclaim, and no pid-reuse window;
      * the file is deliberately persistent and never unlinked: unlinking a
        flock'd file is the classic inode race (a second process would flock
        a *new* inode and not conflict with the first's held inode). The
        lock file is invisible to the firmware and flasher, which only read
        fixed named paths under pyxis-map/ (never a root directory listing).
    """
    try:
        descriptor = os.open(_INSTALL_LOCK_NAME,
                             os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o644,
                             dir_fd=pyxis_fd)
    except OSError as exc:
        raise PackError(f"cannot open the map-install lock: {exc}") from exc
    try:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            if exc.errno in (errno.EAGAIN, errno.EACCES, errno.EWOULDBLOCK):
                raise PackError("another map installer is already running on "
                                "this card; wait for it to finish and retry") from None
            raise PackError(f"cannot acquire the map-install lock: {exc}") from exc
        yield _INSTALL_LOCK_NAME
    finally:
        # Closing releases the flock. Do NOT unlink: see the docstring.
        try:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        except OSError:
            pass
        os.close(descriptor)


# Cross-producer install marker.
#
# The web flasher holds a Web Locks name and this CLI holds an advisory
# flock: two unrelated locking namespaces, so each lock is invisible to
# the other producer. The on-disk marker below is the one coordination
# primitive both producers share -- the flasher writes the same file with
# the same token format (docs/flasher/js/map-installer.js). A marker is a
# small file containing: PYXI 1 <owner> <epoch_ms>.

_INSTALL_MARKER_NAME = ".pyxis-installing"
# A marker written within this window is a live installer. Installations
# longer than this are treated as abandoned (crashed builder, closed tab)
# and reclaimed. A future epoch (writer clock ahead of ours) counts as
# fresh: refusing is the safe direction.
_INSTALL_MARKER_TTL_SECONDS = 900


def _marker_token(owner: str) -> str:
    return f"PYXI 1 {owner} {int(time.time() * 1000)}"


def _parse_marker(raw: bytes) -> tuple[str, int] | None:
    """Parse a marker token into (owner, epoch_ms); None if malformed."""
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError:
        return None
    parts = text.strip().split(" ")
    if len(parts) != 4 or parts[0] != "PYXI" or parts[1] != "1":
        return None
    if not parts[2] or len(parts[2]) > 64 or not parts[3].isdigit():
        return None
    return parts[2], int(parts[3])


def _marker_is_fresh(epoch_ms: int) -> bool:
    now_ms = int(time.time() * 1000)
    if epoch_ms > now_ms:
        return True
    return (now_ms - epoch_ms) / 1000.0 <= _INSTALL_MARKER_TTL_SECONDS


def _acquire_install_marker(pyxis_fd: int) -> str:
    """Claim the cross-producer install marker; return our token.

    Protocol (shared with the web flasher):
      * no marker              -> create it;
      * fresh foreign marker   -> refuse: another installer is live;
      * stale or malformed     -> reclaim (unlink, then create).
    After creation the file is read back and compared to our token: a
    foreign content means we lost a simultaneous acquire race and we
    abort. The marker is best-effort coordination -- the safety net is
    the commit-time revalidation in activate_map_set, which refuses to
    publish a record over activation state that moved since it was
    derived.
    """
    owner = f"cli-{os.getpid():x}"
    token = _marker_token(owner)
    raw = _read_selection_at(pyxis_fd, _INSTALL_MARKER_NAME)
    if raw is not None:
        parsed = _parse_marker(raw)
        if parsed is not None and _marker_is_fresh(parsed[1]):
            raise PackError("another map installer is already running on "
                            "this card; wait for it to finish and retry")
        # Stale or malformed: reclaim. A malformed marker may be a live
        # writer mid-write, but the read-back below fails loudly if so,
        # and commit-time revalidation covers the rest.
        _unlink_quietly_at(pyxis_fd, _INSTALL_MARKER_NAME)
    try:
        descriptor = os.open(_INSTALL_MARKER_NAME,
                             os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC,
                             0o644, dir_fd=pyxis_fd)
    except FileExistsError:
        raise PackError("another map installer is already running on "
                        "this card; wait for it to finish and retry") from None
    except OSError as exc:
        raise PackError(f"cannot write the map-install marker: {exc}") from exc
    try:
        _write_all(descriptor, token.encode("ascii"), _INSTALL_MARKER_NAME)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    actual = _read_selection_at(pyxis_fd, _INSTALL_MARKER_NAME)
    if actual != token.encode("ascii"):
        # Someone overwrote (or removed) our marker while we acquired it.
        # Never delete a foreign marker: our claim is void, abort.
        raise PackError("the map-install marker was contested during "
                        "acquisition; wait and retry")
    _fsync_directory_fd(pyxis_fd)
    return token


def _renew_install_marker(pyxis_fd: int, owner: str) -> None:
    """Renew our marker's epoch during a long installation (heartbeat).

    A publication can outlive _INSTALL_MARKER_TTL_SECONDS (a full world
    pack on slow SD storage); without renewals the marker would age out
    and a second producer could reclaim our live claim. The rewrite
    keeps our owner identity and advances the epoch; the release path
    and the commit-time check compare the owner, so renewed markers are
    still recognized as ours. Best effort: a failed renewal is retried
    on the next tile and the commit-time marker check is the backstop.
    """
    try:
        token = _marker_token(owner)
        descriptor = os.open(_INSTALL_MARKER_NAME,
                             os.O_WRONLY | os.O_TRUNC | os.O_CLOEXEC,
                             dir_fd=pyxis_fd)
        try:
            _write_all(descriptor, token.encode("ascii"), _INSTALL_MARKER_NAME)
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
    except OSError:
        pass


def _release_install_marker(pyxis_fd: int, token: str) -> None:
    """Remove our install marker -- and only ours.

    Comparison is by owner identity (the token's owner field), not the
    full token: the epoch changes as we renew (heartbeat), so an exact
    match would fail once a long install has renewed its marker. If the
    marker now carries a DIFFERENT owner (a later installer reclaimed
    it after our marker aged out, or a crashed run left a different
    claim), deleting it would drop that installer's live claim. Leave
    it; the TTL reclaims it.
    """
    our_owner = _parse_marker(token.encode("ascii"))
    our_owner = our_owner[0] if our_owner is not None else None
    raw = _read_selection_at(pyxis_fd, _INSTALL_MARKER_NAME)
    if raw is not None:
        current = _parse_marker(raw)
        if current is not None and current[0] == our_owner:
            try:
                os.unlink(_INSTALL_MARKER_NAME, dir_fd=pyxis_fd)
            except OSError:
                pass


def _verify_activation_state_at(pyxis_fd: int, map_sets_fd: int, *,
                                slot_raw: list[bytes | None],
                                style_raw: bytes | None, style_name: str,
                                marker_token: str | None) -> None:
    """Commit-time revalidation against a concurrent installer.

    Re-reads the install marker and the raw activation records
    immediately before the record writes. If a cross-producer installer
    (web flasher or another builder) committed since the records were
    derived -- or holds a fresh install marker that is not ours -- the
    activation is aborted before any byte is written. The published pack
    is kept; a retry re-derives from the new state and converges (the
    pack is device-harmless while it is not named by any record: the
    firmware only reads packs enumerated in the active selection).
    """
    marker_raw = _read_selection_at(pyxis_fd, _INSTALL_MARKER_NAME)
    if marker_raw is not None:
        # Compare by owner, not the full token: we renew the marker's
        # epoch during publication (heartbeat), so our own marker may
        # carry a newer epoch than the one acquired at install start.
        own_owner = None
        if marker_token is not None:
            parsed_own = _parse_marker(marker_token.encode("ascii"))
            own_owner = parsed_own[0] if parsed_own is not None else None
        parsed = _parse_marker(marker_raw)
        if parsed is not None and _marker_is_fresh(parsed[1]) \
                and parsed[0] != own_owner:
            raise PackError("another map installer is running on this card; "
                            "wait for it to finish and retry")
    for index, slot_name in enumerate(("active-pack.0", "active-pack.1")):
        actual = _read_selection_at(pyxis_fd, slot_name)
        if actual != slot_raw[index]:
            raise PackError("active map-set records changed during "
                            "installation; wait for the other installer "
                            "to finish and retry")
    actual_style = _read_selection_at(map_sets_fd, style_name)
    if actual_style != style_raw:
        raise PackError("installed style record changed during installation; "
                        "wait for the other installer to finish and retry")


def _remove_tree_at(parent_fd: int, name: str) -> None:
    try:
        descriptor = os.open(name, _DIRECTORY_FLAGS, dir_fd=parent_fd)
    except FileNotFoundError:
        return
    try:
        for entry in os.scandir(descriptor):
            info = os.stat(entry.name, dir_fd=descriptor, follow_symlinks=False)
            if stat.S_ISDIR(info.st_mode):
                _remove_tree_at(descriptor, entry.name)
            else:
                os.unlink(entry.name, dir_fd=descriptor)
    finally:
        os.close(descriptor)
    os.rmdir(name, dir_fd=parent_fd)


def _temporary_directory(packs_fd: int, pack_id: str) -> tuple[str, int]:
    for attempt in range(256):
        name = f".{pack_id}.tmp-{os.getpid():x}-{attempt:02x}"
        try:
            os.mkdir(name, 0o700, dir_fd=packs_fd)
            descriptor = os.open(name, _DIRECTORY_FLAGS, dir_fd=packs_fd)
            return name, descriptor
        except FileExistsError:
            continue
    raise PackError("cannot allocate temporary output directory")


def _rename_noreplace(parent_fd: int, source: str, destination: str, target: Path) -> None:
    if _renameat2 is None:
        raise PackError("atomic no-replace unsupported on this host")
    result = _renameat2(parent_fd, os.fsencode(source), parent_fd, os.fsencode(destination), 1)
    if result == 0:
        return
    error = ctypes.get_errno()
    if error == errno.EEXIST:
        raise PackError(f"output already exists: {target}")
    if error in (errno.ENOSYS, errno.EINVAL, errno.ENOTSUP, errno.EOPNOTSUPP):
        raise PackError("atomic no-replace unsupported on this filesystem")
    raise OSError(error, os.strerror(error), target)


def _verify_output_chain(output_root: Path, root_fd: int, pyxis_fd: int, pyxis_identity: tuple[int, ...],
                         packs_identity: tuple[int, ...]) -> None:
    check_fd = _open_path(output_root)
    try:
        if _identity(os.fstat(check_fd)) != _identity(os.fstat(root_fd)):
            raise PackError("output directory changed before publication")
    finally:
        os.close(check_fd)
    try:
        pyxis_info = os.stat("pyxis-map", dir_fd=root_fd, follow_symlinks=False)
        packs_info = os.stat("packs", dir_fd=pyxis_fd, follow_symlinks=False)
    except OSError as exc:
        raise PackError(f"output directory changed before publication: {exc}") from exc
    if _identity(pyxis_info)[:3] != pyxis_identity[:3] or _identity(packs_info)[:3] != packs_identity[:3]:
        raise PackError("output directory changed before publication")


def _preflight_activation(pyxis_fd: int, *, pack_id: str, map_set_id: str, attribution: str) -> None:
    """Reject an activation whose inherited composition exceeds the limit.

    Runs before any tile is staged or the pack is published, so a rejected
    activation leaves the card exactly as it was found -- no published pack,
    no rewritten record. Mirrors the composition rules in activate_map_set.
    """
    slot_names = ("active-pack.0", "active-pack.1")
    slots: list[dict[str, object] | None] = [None, None]
    for index, slot_name in enumerate(slot_names):
        raw = _read_selection_at(pyxis_fd, slot_name)
        if raw is not None:
            slots[index] = decode_active_selection(raw)
    if slots[0] is not None and slots[1] is not None \
            and slots[0]["generation"] == slots[1]["generation"] and slots[0] != slots[1]:
        raise PackError("conflicting active map-set records")
    valid = [slot for slot in slots if slot is not None]
    highest = max([0] + [cast(int, slot["generation"]) for slot in valid])
    if highest == 0xFFFFFFFF:
        raise PackError("active selection generation is exhausted")
    map_sets_fd, _ = _mkdir_open(pyxis_fd, "map-sets")
    try:
        style_name = f"{map_set_id}.pmas"
        installed_raw = _read_selection_at(map_sets_fd, style_name)
        installed = decode_active_selection(installed_raw) if installed_raw is not None else None
        if installed is not None:
            if installed["format_version"] not in (2, 3) or installed["map_set_id"] != map_set_id \
                    or installed["attribution"] != attribution:
                raise PackError("installed style record does not match selected map set")
        composition = installed
        if composition is None and valid:
            current = max(valid, key=lambda slot: cast(int, slot["generation"]))
            if current["format_version"] in (2, 3) and current["map_set_id"] == map_set_id:
                composition = current
        if composition is not None:
            if composition["attribution"] != attribution:
                raise PackError("installed map set attribution does not match")
            packs = [value for value in cast(list[str], composition["packs"]) if value != pack_id]
            packs.insert(0, pack_id)
            if len(packs) > MAX_ACTIVE_PACKS:
                raise PackError(f"active map-set pack limit exceeded: {len(packs)} > "
                                f"{MAX_ACTIVE_PACKS}; remove an installed pack before activating")
    finally:
        os.close(map_sets_fd)


def build_map_pack(source_directory: Path, output_root: Path, *, pack_id: str, name: str,
                   attribution: str, source: str, license: str,
                   max_tiles: int = DEFAULT_MAX_TILES, max_bytes: int = DEFAULT_MAX_BYTES,
                   antimeridian_zooms: set[int] | None = None,
                   sparse: bool = False, style: str | None = None,
                   activate: bool = False) -> Path:
    if not sys.platform.startswith("linux"):
        raise PackError("secure importer supports Linux hosts only")
    _validate_metadata(pack_id, name, attribution, source, license)
    antimeridian_zooms = set() if antimeridian_zooms is None else set(antimeridian_zooms)
    if any(zoom < 0 or zoom > MAX_ZOOM for zoom in antimeridian_zooms):
        raise PackError("antimeridian zoom is out of range")
    if style is not None:
        if style not in STYLE_POLICIES:
            raise PackError(f"unsupported map style: {style}")
        policy = STYLE_POLICIES[style]
        if (attribution, source, license) != (policy["attribution"], policy["source"], policy["license"]):
            raise PackError("metadata does not match the selected style policy")
    if activate and style is None:
        raise PackError("--activate requires --style so the map-set ID is a firmware style policy")
    source_directory = Path(source_directory)
    output_root = Path(output_root)
    target = output_root / "pyxis-map" / "packs" / pack_id
    source_fd = _open_path(source_directory)
    output_fd = pyxis_fd = packs_fd = stage_fd = pack_fd = -1
    temporary = ""
    committed = False
    try:
        tiles = _discover_tiles_fd(source_fd, source_directory, max_tiles, max_bytes)
        zooms = sorted({tile.zoom for tile in tiles})
        if style is not None:
            # Style installs always publish indexless v3 packs, mirroring
            # the web flasher: no row spans, no coverage caps.
            manifest = serialize_indexless_manifest(pack_id=pack_id, name=name,
                                                    attribution=attribution, source=source,
                                                    license=license, min_zoom=zooms[0],
                                                    max_zoom=zooms[-1], tile_count=len(tiles))
        else:
            try:
                extents = calculate_extents(tiles, antimeridian_zooms)
            except PackError as exc:
                if not sparse or antimeridian_zooms or not str(exc).startswith("incomplete rectangle"):
                    raise
                try:
                    row_spans = calculate_row_spans(tiles)
                    manifest = serialize_sparse_manifest(pack_id=pack_id, name=name,
                                                         attribution=attribution, source=source,
                                                         license=license, row_spans=row_spans,
                                                         tile_count=len(tiles))
                except PackError as span_exc:
                    if "row-span limit" not in str(span_exc):
                        raise
                    # Sparse coverage beyond the v2 span cap falls back to
                    # an indexless v3 pack.
                    manifest = serialize_indexless_manifest(pack_id=pack_id, name=name,
                                                            attribution=attribution, source=source,
                                                            license=license, min_zoom=zooms[0],
                                                            max_zoom=zooms[-1], tile_count=len(tiles))
            else:
                manifest = serialize_manifest(pack_id=pack_id, name=name, attribution=attribution, source=source,
                                              license=license, extents=extents, tile_count=len(tiles))
        output_fd = _open_path(output_root, create=True)
        pyxis_fd, pyxis_identity = _mkdir_open(output_fd, "pyxis-map")
        packs_fd, packs_identity = _mkdir_open(pyxis_fd, "packs")
        existing = _existing_pack_fd(packs_fd, pack_id)
        resumed = False
        if existing is not None:
            if not activate:
                # Without --activate a resume would leave the card exactly
                # as it was, so an existing pack is simply refused.
                if existing >= 0:
                    os.close(existing)
                    existing = -1
                raise PackError(f"output already exists: {target}")
            # The pack already exists on the card. This is either a retry
            # of a run interrupted between pack publication and activation
            # (the style/slot records may be stale or split), or an exact
            # duplicate run. Mirroring the web flasher, resume is allowed
            # only when the published pack is byte-identical to this
            # source; then re-activating (or activating for the first
            # time) converges the records to this run's generation.
            _verify_existing_pack(source_fd, existing, pack_id, manifest, tiles, max_bytes)
            pack_fd = existing
            resumed = True
        if activate:
            assert style is not None
            # Hold the install lock from the preflight through activation:
            # two concurrent builders could otherwise both pass the
            # preflight and each clobber the other's records. The cross-
            # producer install marker (shared with the web flasher, which
            # holds a Web Locks name this flock cannot see) is claimed for
            # the same span and released on every exit path.
            with _install_lock_at(pyxis_fd):
                # Claim the cross-producer install marker FIRST, before any
                # staging or publication: a live web-flasher install (which
                # holds a Web Locks name this flock cannot see) is announced
                # by its marker, and refusing up front avoids the wasted
                # tile copies and -- more importantly -- avoids publishing a
                # pack that activation would then have to abandon. Stale or
                # malformed markers (crashed builder, closed tab) are
                # reclaimed here; ours is released on every exit path.
                marker_token = _acquire_install_marker(pyxis_fd)
                try:
                    _preflight_activation(pyxis_fd, pack_id=pack_id, map_set_id=style,
                                          attribution=attribution)
                    if not resumed:
                        temporary, stage_fd = _temporary_directory(packs_fd, pack_id)
                        copied = 0
                        # Heartbeat: renew our marker during long
                        # publications so a full world pack on slow SD
                        # storage cannot age out and be reclaimed by a
                        # second producer mid-install (Greptile round 8).
                        _marker_owner = _parse_marker(marker_token.encode("ascii"))
                        _marker_owner = _marker_owner[0] if _marker_owner is not None else None
                        for tile_index, tile in enumerate(tiles):
                            copy_tile(source_fd, tile, stage_fd, max_bytes - copied)
                            copied += tile.size
                            if _marker_owner is not None and tile_index % 25 == 24:
                                _renew_install_marker(pyxis_fd, _marker_owner)
                        if tiles and _marker_owner is not None:
                            _renew_install_marker(pyxis_fd, _marker_owner)
                        manifest_fd = os.open("manifest.pmp", os.O_WRONLY | os.O_CREAT | os.O_EXCL
                                               | os.O_CLOEXEC, 0o644, dir_fd=stage_fd)
                        try:
                            _write_all(manifest_fd, manifest, "manifest.pmp")
                            os.fsync(manifest_fd)
                        finally:
                            os.close(manifest_fd)
                        _fsync_directory_fd(stage_fd)
                        _validate_pack_fd(stage_fd, target, max_bytes)
                        _verify_output_chain(output_root, output_fd, pyxis_fd, pyxis_identity,
                                             packs_identity)
                        _rename_noreplace(packs_fd, temporary, pack_id, target)
                        committed = True
                        try:
                            _verify_output_chain(output_root, output_fd, pyxis_fd,
                                                 pyxis_identity, packs_identity)
                        except BaseException as exc:
                            try:
                                _remove_tree_at(packs_fd, pack_id)
                                _fsync_directory_fd(packs_fd)
                                committed = False
                            except BaseException as cleanup_error:
                                raise PublishedDurabilityError(target, OSError(
                                    f"publication location changed and rollback failed: "
                                    f"{cleanup_error}")) from exc
                            raise PackError("output directory changed during publication; "
                                            "rolled back") from exc
                        try:
                            os.close(stage_fd)
                        except OSError as exc:
                            stage_fd = -1
                            raise PublishedDurabilityError(target, exc) from exc
                        stage_fd = -1
                    slot_name, _enabled = activate_map_set(pyxis_fd, pack_id=pack_id,
                                                           map_set_id=style,
                                                           attribution=attribution,
                                                           marker_token=marker_token)
                finally:
                    _release_install_marker(pyxis_fd, marker_token)
        else:
            temporary, stage_fd = _temporary_directory(packs_fd, pack_id)
            copied = 0
            for tile in tiles:
                copy_tile(source_fd, tile, stage_fd, max_bytes - copied)
                copied += tile.size
            manifest_fd = os.open("manifest.pmp", os.O_WRONLY | os.O_CREAT | os.O_EXCL
                                   | os.O_CLOEXEC, 0o644, dir_fd=stage_fd)
            try:
                _write_all(manifest_fd, manifest, "manifest.pmp")
                os.fsync(manifest_fd)
            finally:
                os.close(manifest_fd)
            _fsync_directory_fd(stage_fd)
            _validate_pack_fd(stage_fd, target, max_bytes)
            _verify_output_chain(output_root, output_fd, pyxis_fd, pyxis_identity,
                                 packs_identity)
            _rename_noreplace(packs_fd, temporary, pack_id, target)
            committed = True
            try:
                _verify_output_chain(output_root, output_fd, pyxis_fd, pyxis_identity,
                                     packs_identity)
            except BaseException as exc:
                try:
                    _remove_tree_at(packs_fd, pack_id)
                    _fsync_directory_fd(packs_fd)
                    committed = False
                except BaseException as cleanup_error:
                    raise PublishedDurabilityError(target, OSError(
                        f"publication location changed and rollback failed: "
                        f"{cleanup_error}")) from exc
                raise PackError("output directory changed during publication; "
                                "rolled back") from exc
            try:
                os.close(stage_fd)
            except OSError as exc:
                stage_fd = -1
                raise PublishedDurabilityError(target, exc) from exc
            stage_fd = -1
        try:
            _fsync_directory_fd(packs_fd)
        except OSError as exc:
            raise PublishedDurabilityError(target, exc) from exc
        if resumed and pack_fd >= 0:
            os.close(pack_fd)
            pack_fd = -1
        return target
    finally:
        active_exception = sys.exc_info()[0] is not None
        close_error: OSError | None = None
        if stage_fd >= 0:
            try:
                os.close(stage_fd)
            except OSError as exc:
                close_error = exc
        if temporary and not committed and packs_fd >= 0:
            _remove_tree_at(packs_fd, temporary)
        for descriptor in (pack_fd, packs_fd, pyxis_fd, output_fd, source_fd):
            if descriptor >= 0:
                try:
                    os.close(descriptor)
                except OSError as exc:
                    if close_error is None:
                        close_error = exc
        if committed and close_error is not None and not active_exception:
            raise PublishedDurabilityError(target, close_error)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xyz_directory", type=Path)
    parser.add_argument("output_root", type=Path, help="SD-card root under which pyxis-map/packs is created")
    parser.add_argument("--pack-id", required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--attribution")
    parser.add_argument("--source")
    parser.add_argument("--license")
    parser.add_argument("--max-tiles", type=int, default=DEFAULT_MAX_TILES)
    parser.add_argument("--max-bytes", type=int, default=DEFAULT_MAX_BYTES)
    parser.add_argument("--antimeridian-zoom", action="append", type=int, default=[])
    parser.add_argument("--sparse", action="store_true",
                        help="explicitly admit sparse state/polygon coverage and emit PMPK v2")
    parser.add_argument("--style", choices=sorted(STYLE_POLICIES),
                        help="firmware style policy; supplies canonical attribution/source/license, "
                             "emits an indexless PMPK v3 pack")
    parser.add_argument("--activate", action="store_true",
                        help="also publish the v3 active map-set record (active-pack slot + map-sets/<style>.pmas)")
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    if arguments.style is not None:
        policy = STYLE_POLICIES[arguments.style]
        attribution = arguments.attribution or policy["attribution"]
        source = arguments.source or policy["source"]
        license = arguments.license or policy["license"]
    else:
        if not (arguments.attribution and arguments.source and arguments.license):
            print("error: --attribution, --source, and --license are required unless --style is given",
                  file=sys.stderr)
            return 2
        attribution, source, license = arguments.attribution, arguments.source, arguments.license
    try:
        target = build_map_pack(arguments.xyz_directory, arguments.output_root,
                                pack_id=arguments.pack_id, name=arguments.name,
                                attribution=attribution, source=source,
                                license=license, max_tiles=arguments.max_tiles,
                                max_bytes=arguments.max_bytes,
                                antimeridian_zooms=set(arguments.antimeridian_zoom),
                                sparse=arguments.sparse, style=arguments.style,
                                activate=arguments.activate)
    except PublishedDurabilityError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 3
    except (PackError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    print(target)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
