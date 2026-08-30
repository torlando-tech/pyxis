#!/usr/bin/env python3
# Copyright (c) 2026 Pyxis contributors
# SPDX-License-Identifier: MIT
"""Build a Pyxis offline map pack exclusively from a local XYZ PNG tree.

Security-sensitive filesystem operations intentionally support Linux only. PNG
support is intentionally limited to non-interlaced images.
"""

from __future__ import annotations

import argparse
import ctypes
from dataclasses import dataclass
import errno
import fcntl
import os
from pathlib import Path
import re
import stat
import struct
import sys
from typing import Any, cast, Iterable
import zlib

MAGIC = b"PMPK"
ACTIVE_MAGIC = b"PMAS"
FORMAT_VERSION = 1
SPARSE_FORMAT_VERSION = 2
INDEXLESS_FORMAT_VERSION = 3
HEADER_SIZE = 16
EXTENT_SIZE = 26
ROW_SPAN_SIZE = 13
MAX_ROW_SPANS = 512
MAX_ZOOM = 22
DEFAULT_MAX_TILES = 100_000
DEFAULT_MAX_BYTES = 8 * 1024 * 1024 * 1024
MAX_VISITED_ENTRIES_BASE = 64
PACK_ID_RE = re.compile(r"[a-z0-9_-]{1,31}\Z")
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
_STRING_LIMITS = {"pack_id": 31, "name": 63, "attribution": 127, "source": 127, "license": 63}
MAX_MANIFEST_BYTES = 7100
MAX_ACTIVE_SELECTION_BYTES = 7105
MAX_ACTIVE_PACKS = 8
MAX_GENERATION = 0xFFFFFFFF
INSTALL_LOCK_NAME = ".install.lock"
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
    output = bytearray()
    previous = bytearray(row_bytes)
    stride = row_bytes + 1
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


def serialize_indexless_manifest(*, pack_id: str, name: str, attribution: str, source: str,
                                 license: str, minimum_zoom: int, maximum_zoom: int,
                                 tile_count: int) -> bytes:
    _validate_metadata(pack_id, name, attribution, source, license)
    if not 0 <= minimum_zoom <= maximum_zoom <= MAX_ZOOM:
        raise PackError("invalid indexless zoom range")
    if tile_count <= 0 or tile_count > 0xFFFFFFFF:
        raise PackError("invalid indexless tile count")
    payload = bytearray()
    for field, value in (("pack_id", pack_id), ("name", name), ("attribution", attribution),
                         ("source", source), ("license", license)):
        encoded = _checked_text(field, value)
        payload.append(len(encoded))
        payload.extend(encoded)
    payload.extend((minimum_zoom, maximum_zoom))
    payload.extend(struct.pack("<I", tile_count))
    length = HEADER_SIZE + len(payload) + 4
    result = bytearray(struct.pack("<4sBBHII", MAGIC, INDEXLESS_FORMAT_VERSION, 0, HEADER_SIZE, length, 0))
    result.extend(payload)
    result.extend(struct.pack("<I", zlib.crc32(result)))
    return bytes(result)


def parse_manifest(data: bytes) -> dict[str, object]:
    if len(data) < 20 or len(data) > MAX_MANIFEST_BYTES:
        raise PackError("manifest is truncated or oversized")
    magic, version, reserved, header_size, length, reserved_word = struct.unpack("<4sBBHII", data[:16])
    if magic != MAGIC or version not in (FORMAT_VERSION, SPARSE_FORMAT_VERSION, INDEXLESS_FORMAT_VERSION) or \
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
                      tile_count=tile_count, extents=extents, row_spans=[], indexless=False)
    elif version == INDEXLESS_FORMAT_VERSION:
        if position + 6 > len(data) - 4:
            raise PackError("manifest indexless fields are truncated")
        minimum, maximum = data[position:position + 2]
        position += 2
        tile_count = struct.unpack("<I", data[position:position + 4])[0]
        position += 4
        if position != len(data) - 4:
            raise PackError("manifest indexless record has trailing bytes")
        if minimum > maximum or maximum > MAX_ZOOM:
            raise PackError("invalid manifest zoom range")
        if tile_count == 0:
            raise PackError("invalid manifest tile count")
        values.update(format_version=version, min_zoom=minimum, max_zoom=maximum,
                      tile_count=tile_count, extents=[], row_spans=[], indexless=True)
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
                      tile_count=tile_count, extents=[], row_spans=row_spans, indexless=False)
    return values


def _validate_indexless_selection(generation: int, map_set_id: str, attribution: str,
                                  pack_ids: list[str]) -> None:
    if not 1 <= generation <= MAX_GENERATION:
        raise PackError("active selection generation must be 1..0xFFFFFFFF")
    if not PACK_ID_RE.fullmatch(map_set_id):
        raise PackError("map set ID must match [a-z0-9_-]{1,31}")
    _checked_text("attribution", attribution)
    if not 1 <= len(pack_ids) <= MAX_ACTIVE_PACKS:
        raise PackError("active selection must reference 1..8 packs")
    seen: set[str] = set()
    for pack_id in pack_ids:
        if not PACK_ID_RE.fullmatch(pack_id):
            raise PackError("pack ID must match [a-z0-9_-]{1,31}")
        if pack_id in seen:
            raise PackError("duplicate pack ID in active selection")
        seen.add(pack_id)


def encode_active_map_set(*, generation: int, map_set_id: str, attribution: str,
                          pack_ids: list[str]) -> bytes:
    _validate_indexless_selection(generation, map_set_id, attribution, pack_ids)
    body = bytearray(struct.pack("<4sBBHI", ACTIVE_MAGIC, INDEXLESS_FORMAT_VERSION, 0, 0, generation))
    for field, value in (("pack_id", map_set_id), ("attribution", attribution)):
        encoded = _checked_text(field, value)
        body.append(len(encoded))
        body.extend(encoded)
    body.append(len(pack_ids))
    for pack_id in pack_ids:
        encoded = pack_id.encode("ascii")
        body.append(len(encoded))
        body.extend(encoded)
    total_length = len(body) + 4
    header = bytes(body[:6]) + struct.pack("<H", total_length) + bytes(body[8:])
    return header + struct.pack("<I", zlib.crc32(header))


def _decode_pmas_string(data: bytes, position: int, end: int, *, capacity: int, identifier: bool) -> tuple[str, int]:
    if position >= end:
        raise PackError("active selection string is truncated")
    size = data[position]
    position += 1
    if size == 0 or size >= capacity or size > end - position:
        raise PackError("invalid active selection string")
    try:
        value = data[position:position + size].decode("ascii")
    except UnicodeDecodeError as exc:
        raise PackError("invalid active selection string") from exc
    if not all(0x20 <= byte <= 0x7E for byte in data[position:position + size]):
        raise PackError("invalid active selection string")
    if identifier and not PACK_ID_RE.fullmatch(value):
        raise PackError("invalid active selection identifier")
    return value, position + size


def decode_active_selection(data: bytes) -> dict[str, object]:
    if len(data) < 16 or len(data) > MAX_ACTIVE_SELECTION_BYTES:
        raise PackError("active selection is truncated or oversized")
    if data[:4] != ACTIVE_MAGIC or data[5] != 0:
        raise PackError("invalid active selection header")
    version = data[4]
    if version not in (1, SPARSE_FORMAT_VERSION, INDEXLESS_FORMAT_VERSION):
        raise PackError("unsupported active selection version")
    if zlib.crc32(data[:-4]) != struct.unpack("<I", data[-4:])[0]:
        raise PackError("invalid active selection CRC")
    if version == 1:
        if len(data) != 48:
            raise PackError("legacy active selection must be 48 bytes")
        if struct.unpack("<H", data[6:8])[0] != 48:
            raise PackError("invalid legacy active selection header")
        generation = struct.unpack("<I", data[8:12])[0]
        if generation == 0:
            raise PackError("invalid legacy active selection generation")
        pack_id_size = data[12]
        pack_id, = (data[13:13 + pack_id_size].decode("ascii"),)
        if pack_id_size == 0 or pack_id_size >= 32 or not PACK_ID_RE.fullmatch(pack_id):
            raise PackError("invalid legacy active selection pack ID")
        if any(byte != 0 for byte in data[13 + pack_id_size:44]):
            raise PackError("legacy active selection must be zero padded")
        return {"format_version": 1, "generation": generation, "map_set_id": pack_id,
                "attribution": "", "pack_ids": [pack_id], "row_spans": {}}
    generation = struct.unpack("<I", data[8:12])[0]
    if generation == 0:
        raise PackError("invalid active selection generation")
    if struct.unpack("<H", data[6:8])[0] != len(data):
        raise PackError("invalid active selection length")
    end = len(data) - 4
    position = 12
    map_set_id, position = _decode_pmas_string(data, position, end, capacity=32, identifier=True)
    attribution, position = _decode_pmas_string(data, position, end, capacity=128, identifier=False)
    if position >= end:
        raise PackError("active selection pack count is truncated")
    pack_count = data[position]
    position += 1
    if not 1 <= pack_count <= MAX_ACTIVE_PACKS:
        raise PackError("invalid active selection pack count")
    pack_ids: list[str] = []
    row_spans: dict[str, list[tuple[int, int, int, int]]] = {}
    for _ in range(pack_count):
        pack_id, position = _decode_pmas_string(data, position, end, capacity=32, identifier=True)
        if pack_id in pack_ids:
            raise PackError("duplicate pack ID in active selection")
        pack_ids.append(pack_id)
        if version == SPARSE_FORMAT_VERSION:
            if position + 2 > end:
                raise PackError("active selection span count is truncated")
            span_count = struct.unpack("<H", data[position:position + 2])[0]
            position += 2
            if span_count == 0 or span_count > MAX_ROW_SPANS:
                raise PackError("invalid active selection span count")
            if span_count * ROW_SPAN_SIZE > end - position:
                raise PackError("active selection spans are truncated")
            spans: list[tuple[int, int, int, int]] = []
            for _span in range(span_count):
                zoom, y, x_minimum, x_maximum = struct.unpack(
                    "<BIII", data[position:position + ROW_SPAN_SIZE])
                position += ROW_SPAN_SIZE
                spans.append((zoom, y, x_minimum, x_maximum))
            row_spans[pack_id] = spans
    if position != end:
        raise PackError("active selection has trailing bytes")
    return {"format_version": version, "generation": generation, "map_set_id": map_set_id,
            "attribution": attribution, "pack_ids": pack_ids, "row_spans": row_spans}


def acquire_install_lock(pyxis_fd: int) -> int:
    """Acquire the persistent CLI-to-CLI install lock.

    The lock file lives inside the mounted pyxis-map directory and is never
    unlinked: it exists only to carry an fcntl.flock that serializes CLI
    processes. Returns the lock descriptor; closing it releases the lock.
    """
    flags = os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC
    try:
        lock_fd = os.open(INSTALL_LOCK_NAME, flags, 0o644, dir_fd=pyxis_fd)
    except OSError as exc:
        raise PackError(f"cannot open CLI install lock: {exc}") from exc
    try:
        fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as exc:
        os.close(lock_fd)
        raise PackError("another CLI map install is running; retry after it finishes") from exc
    return lock_fd


def release_install_lock(lock_fd: int) -> None:
    os.close(lock_fd)


@dataclass(frozen=True)
class ActivationPlan:
    style_name: str
    target_slot: str
    generation: int
    pack_ids: tuple[str, ...]
    record: bytes


def _slot_state(raw: bytes | None) -> tuple[str, dict[str, object] | None]:
    """Classify one raw active-slot record without any filesystem access."""
    if raw is None:
        return "missing", None
    try:
        values = decode_active_selection(raw)
    except PackError:
        return "invalid", None
    return "present", values


def plan_activation(*, slot_0: bytes | None, slot_1: bytes | None,
                    style_record: bytes | None = None,
                    new_pack_id: str, style_id: str, attribution: str) -> ActivationPlan:
    """Derive a candidate PMAS v3 record from a raw snapshot. No filesystem I/O."""
    if style_id not in STYLE_POLICIES:
        raise PackError(f"unsupported map style: {style_id}")
    policy = STYLE_POLICIES[style_id]
    if attribution != policy["attribution"]:
        raise PackError("--style requires attribution exactly matching the firmware style policy")
    if not PACK_ID_RE.fullmatch(new_pack_id):
        raise PackError("pack ID must match [a-z0-9_-]{1,31}")
    states = ((0, _slot_state(slot_0)), (1, _slot_state(slot_1)))
    present: list[tuple[int, dict[str, object]]] = []
    for index, (state, values) in states:
        if state == "present" and values is not None:
            present.append((index, values))
    if len(present) == 2:
        first = present[0][1]
        second = present[1][1]
        if first["generation"] == second["generation"] and first != second:
            raise PackError("active slots disagree at equal generation; restore a known-good card state first")
    style_values: dict[str, object] | None = None
    if style_record is not None:
        try:
            decoded = decode_active_selection(style_record)
        except PackError:
            decoded = None
        if decoded is not None and decoded["map_set_id"] == style_id \
                and decoded["attribution"] == policy["attribution"]:
            style_values = decoded
    max_generation = max(
        (cast(int, values["generation"]) for _, values in present),
        default=0,
    )
    if style_values is not None:
        max_generation = max(max_generation, cast(int, style_values["generation"]))
    generation = max_generation + 1
    if generation > MAX_GENERATION:
        raise PackError("active selection generation is exhausted")

    if style_values is not None:
        composition = list(cast(list[str], style_values["pack_ids"]))
    else:
        composition: list[str] = []
        for _index, values in sorted(present, key=lambda item: cast(int, item[1]["generation"]), reverse=True):
            if values["map_set_id"] == style_id:
                composition = list(cast(list[str], values["pack_ids"]))
                break

    if new_pack_id in composition:
        composition.remove(new_pack_id)
    composition.insert(0, new_pack_id)
    if len(composition) > MAX_ACTIVE_PACKS:
        raise PackError("active selection exceeds the 8-pack limit")
    if len(set(composition)) != len(composition):
        raise PackError("duplicate pack ID in candidate map set")

    missing_or_invalid = [index for index, (state, _values) in states if state != "present"]
    if missing_or_invalid:
        target_slot = f"active-pack.{missing_or_invalid[0]}"
    else:
        lower = min(present, key=lambda item: (cast(int, item[1]["generation"]), item[0]))
        target_slot = f"active-pack.{lower[0]}"
    record = encode_active_map_set(generation=generation, map_set_id=style_id,
                                   attribution=attribution, pack_ids=composition)
    return ActivationPlan(style_name=style_id, target_slot=target_slot, generation=generation,
                          pack_ids=tuple(composition), record=record)


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
    format_version = parsed["format_version"]
    if format_version == FORMAT_VERSION:
        expected_extents = cast(list[ZoomExtent], parsed["extents"])
        extents = calculate_extents(tiles, {item.zoom for item in expected_extents if len(item.intervals) == 2})
        if extents != expected_extents:
            raise PackError("emitted tile tree does not match manifest")
    elif format_version == INDEXLESS_FORMAT_VERSION:
        if tiles[0].zoom != parsed["min_zoom"] or tiles[-1].zoom != parsed["max_zoom"]:
            raise PackError("emitted tile tree does not match indexless manifest")
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


def build_map_pack(source_directory: Path, output_root: Path, *, pack_id: str, name: str,
                   attribution: str, source: str, license: str,
                   max_tiles: int = DEFAULT_MAX_TILES, max_bytes: int = DEFAULT_MAX_BYTES,
                   antimeridian_zooms: set[int] | None = None,
                   sparse: bool = False,
                   style: str | None = None) -> Path:
    if not sys.platform.startswith("linux"):
        raise PackError("secure importer supports Linux hosts only")
    if style is not None and style not in STYLE_POLICIES:
        raise PackError(f"unsupported map style: {style}")
    _validate_metadata(pack_id, name, attribution, source, license)
    if style is not None:
        policy = STYLE_POLICIES[style]
        for field, expected in policy.items():
            if locals()[field] != expected:
                raise PackError(
                    f"--style {style} requires {field} exactly matching the firmware style policy")
    antimeridian_zooms = set() if antimeridian_zooms is None else set(antimeridian_zooms)
    if any(zoom < 0 or zoom > MAX_ZOOM for zoom in antimeridian_zooms):
        raise PackError("antimeridian zoom is out of range")
    source_directory = Path(source_directory)
    output_root = Path(output_root)
    target = output_root / "pyxis-map" / "packs" / pack_id
    source_fd = _open_path(source_directory)
    output_fd = pyxis_fd = packs_fd = stage_fd = -1
    temporary = ""
    committed = False
    try:
        tiles = _discover_tiles_fd(source_fd, source_directory, max_tiles, max_bytes)
        if style is not None:
            manifest = serialize_indexless_manifest(
                pack_id=pack_id, name=name, attribution=attribution, source=source,
                license=license, minimum_zoom=tiles[0].zoom, maximum_zoom=tiles[-1].zoom,
                tile_count=len(tiles))
        else:
            try:
                extents = calculate_extents(tiles, antimeridian_zooms)
            except PackError as exc:
                if not sparse or antimeridian_zooms or not str(exc).startswith("incomplete rectangle"):
                    raise
                row_spans = calculate_row_spans(tiles)
                manifest = serialize_sparse_manifest(pack_id=pack_id, name=name, attribution=attribution,
                                                     source=source, license=license,
                                                     row_spans=row_spans, tile_count=len(tiles))
            else:
                manifest = serialize_manifest(pack_id=pack_id, name=name, attribution=attribution, source=source,
                                              license=license, extents=extents, tile_count=len(tiles))
        output_fd = _open_path(output_root, create=True)
        pyxis_fd, pyxis_identity = _mkdir_open(output_fd, "pyxis-map")
        packs_fd, packs_identity = _mkdir_open(pyxis_fd, "packs")
        temporary, stage_fd = _temporary_directory(packs_fd, pack_id)
        copied = 0
        for tile in tiles:
            copy_tile(source_fd, tile, stage_fd, max_bytes - copied)
            copied += tile.size
        manifest_fd = os.open("manifest.pmp", os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC,
                              0o644, dir_fd=stage_fd)
        try:
            _write_all(manifest_fd, manifest, "manifest.pmp")
            os.fsync(manifest_fd)
        finally:
            os.close(manifest_fd)
        _fsync_directory_fd(stage_fd)
        _validate_pack_fd(stage_fd, target, max_bytes)
        _verify_output_chain(output_root, output_fd, pyxis_fd, pyxis_identity, packs_identity)
        _rename_noreplace(packs_fd, temporary, pack_id, target)
        committed = True
        try:
            _verify_output_chain(output_root, output_fd, pyxis_fd, pyxis_identity, packs_identity)
        except BaseException as exc:
            try:
                _remove_tree_at(packs_fd, pack_id)
                _fsync_directory_fd(packs_fd)
                committed = False
            except BaseException as cleanup_error:
                raise PublishedDurabilityError(target, OSError(
                    f"publication location changed and rollback failed: {cleanup_error}")) from exc
            raise PackError("output directory changed during publication; rolled back") from exc
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
        for descriptor in (packs_fd, pyxis_fd, output_fd, source_fd):
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
    parser.add_argument("--attribution", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--license", required=True)
    parser.add_argument("--max-tiles", type=int, default=DEFAULT_MAX_TILES)
    parser.add_argument("--max-bytes", type=int, default=DEFAULT_MAX_BYTES)
    parser.add_argument("--antimeridian-zoom", action="append", type=int, default=[])
    parser.add_argument("--sparse", action="store_true",
                        help="explicitly admit sparse state/polygon coverage and emit PMPK v2")
    parser.add_argument("--style", choices=sorted(STYLE_POLICIES),
                        help="firmware map style; metadata must exactly match the style policy and PMPK v3 is emitted")
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        target = build_map_pack(arguments.xyz_directory, arguments.output_root,
                                pack_id=arguments.pack_id, name=arguments.name,
                                attribution=arguments.attribution, source=arguments.source,
                                license=arguments.license, max_tiles=arguments.max_tiles,
                                max_bytes=arguments.max_bytes,
                                antimeridian_zooms=set(arguments.antimeridian_zoom),
                                sparse=arguments.sparse, style=arguments.style)
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
