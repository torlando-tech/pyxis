#!/usr/bin/env python3
"""
Prefetch XYZ map tiles for Pyxis offline use.

The firmware expects tiles on SD in this layout:
    tiles/{z}/{x}/{y}.png

This script downloads tiles for a circular area around a center point and
stores them in the same directory structure so the folder can be copied
directly to the SD card.

Default center:
    lat=43.978093
    lon=-66.143359

Default radius:
    200 km
"""

from __future__ import annotations

import argparse
import math
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


DEFAULT_LAT = 43.978093
DEFAULT_LON = -66.143359
DEFAULT_RADIUS_KM = 200.0
DEFAULT_MIN_ZOOM = 8
DEFAULT_MAX_ZOOM = 14
DEFAULT_USER_AGENT = "PyxisOfflinePrefetch/1.0"
EARTH_RADIUS_KM = 6371.0088


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Download Pyxis-compatible XYZ tiles into tiles/{z}/{x}/{y}.png. "
            "You must provide a tile URL template for a provider that explicitly "
            "permits bulk and offline downloads."
        )
    )
    parser.add_argument("--lat", type=float, default=DEFAULT_LAT, help="Center latitude.")
    parser.add_argument("--lon", type=float, default=DEFAULT_LON, help="Center longitude.")
    parser.add_argument(
        "--radius-km",
        type=float,
        default=DEFAULT_RADIUS_KM,
        help="Download radius in kilometers.",
    )
    parser.add_argument(
        "--min-zoom",
        type=int,
        default=DEFAULT_MIN_ZOOM,
        help="Lowest zoom level to fetch.",
    )
    parser.add_argument(
        "--max-zoom",
        type=int,
        default=DEFAULT_MAX_ZOOM,
        help="Highest zoom level to fetch.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("tiles"),
        help="Directory where the tiles/ hierarchy is created.",
    )
    parser.add_argument(
        "--url-template",
        help=(
            "Required XYZ tile URL template with {z}, {x}, {y} placeholders. "
            "Do not use OpenStreetMap's public tile service for offline prefetching."
        ),
    )
    parser.add_argument(
        "--user-agent",
        default=DEFAULT_USER_AGENT,
        help="HTTP User-Agent header.",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=20.0,
        help="HTTP timeout in seconds.",
    )
    parser.add_argument(
        "--delay",
        type=float,
        default=0.25,
        help="Delay between downloads in seconds.",
    )
    parser.add_argument(
        "--retries",
        type=int,
        default=2,
        help="Retry count per tile after the initial attempt.",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Re-download tiles even if they already exist locally.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the tile count without downloading.",
    )
    return parser.parse_args()


def clamp_lat(lat: float) -> float:
    return max(min(lat, 85.05112878), -85.05112878)


def normalize_lon(lon: float) -> float:
    while lon < -180.0:
        lon += 360.0
    while lon >= 180.0:
        lon -= 360.0
    return lon


def latlon_to_tile(lat: float, lon: float, zoom: int) -> tuple[int, int]:
    lat = clamp_lat(lat)
    lon = normalize_lon(lon)
    n = 1 << zoom
    x = (lon + 180.0) / 360.0 * n
    lat_rad = math.radians(lat)
    y = (1.0 - math.log(math.tan(lat_rad) + 1.0 / math.cos(lat_rad)) / math.pi) / 2.0 * n
    return int(math.floor(x)), int(math.floor(y))


def tile_bounds(tile_x: int, tile_y: int, zoom: int) -> tuple[float, float, float, float]:
    n = 1 << zoom
    west = tile_x / n * 360.0 - 180.0
    east = (tile_x + 1) / n * 360.0 - 180.0
    north_rad = math.atan(math.sinh(math.pi * (1.0 - 2.0 * tile_y / n)))
    south_rad = math.atan(math.sinh(math.pi * (1.0 - 2.0 * (tile_y + 1) / n)))
    north = math.degrees(north_rad)
    south = math.degrees(south_rad)
    return south, west, north, east


def haversine_km(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    lat1_rad = math.radians(lat1)
    lon1_rad = math.radians(lon1)
    lat2_rad = math.radians(lat2)
    lon2_rad = math.radians(lon2)
    dlat = lat2_rad - lat1_rad
    dlon = lon2_rad - lon1_rad
    a = (
        math.sin(dlat / 2.0) ** 2
        + math.cos(lat1_rad) * math.cos(lat2_rad) * math.sin(dlon / 2.0) ** 2
    )
    return 2.0 * EARTH_RADIUS_KM * math.asin(math.sqrt(a))


def tile_intersects_radius(
    center_lat: float,
    center_lon: float,
    radius_km: float,
    tile_x: int,
    tile_y: int,
    zoom: int,
) -> bool:
    south, west, north, east = tile_bounds(tile_x, tile_y, zoom)
    clamped_lat = min(max(center_lat, south), north)

    for shifted_center_lon in (center_lon, center_lon - 360.0, center_lon + 360.0):
        clamped_lon = min(max(shifted_center_lon, west), east)
        if haversine_km(center_lat, shifted_center_lon, clamped_lat, clamped_lon) <= radius_km:
            return True

    return False


def bounding_box(center_lat: float, center_lon: float, radius_km: float) -> tuple[float, float, float, float]:
    lat_delta = math.degrees(radius_km / EARTH_RADIUS_KM)
    cos_lat = math.cos(math.radians(center_lat))
    if abs(cos_lat) < 1e-9:
        lon_delta = 180.0
    else:
        lon_delta = math.degrees(radius_km / (EARTH_RADIUS_KM * cos_lat))
    south = clamp_lat(center_lat - lat_delta)
    north = clamp_lat(center_lat + lat_delta)
    west = normalize_lon(center_lon - lon_delta)
    east = normalize_lon(center_lon + lon_delta)
    return south, west, north, east


def tile_x_ranges(west: float, east: float, zoom: int) -> list[tuple[int, int]]:
    max_index = (1 << zoom) - 1

    if west <= east:
        x_start, _ = latlon_to_tile(0.0, west, zoom)
        x_end, _ = latlon_to_tile(0.0, east, zoom)
        return [(max(0, x_start), min(max_index, x_end))]

    x_wrap_start, _ = latlon_to_tile(0.0, west, zoom)
    x_wrap_end, _ = latlon_to_tile(0.0, east, zoom)
    return [
        (max(0, x_wrap_start), max_index),
        (0, min(max_index, x_wrap_end)),
    ]


def enumerate_tiles(center_lat: float, center_lon: float, radius_km: float, zoom: int) -> list[tuple[int, int]]:
    south, west, north, east = bounding_box(center_lat, center_lon, radius_km)
    _, y1 = latlon_to_tile(north, west, zoom)
    _, y0 = latlon_to_tile(south, east, zoom)
    max_index = (1 << zoom) - 1
    y_start = max(0, min(y0, y1))
    y_end = min(max_index, max(y0, y1))

    tiles: list[tuple[int, int]] = []
    for x_start, x_end in tile_x_ranges(west, east, zoom):
        for tile_x in range(x_start, x_end + 1):
            for tile_y in range(y_start, y_end + 1):
                if tile_intersects_radius(center_lat, center_lon, radius_km, tile_x, tile_y, zoom):
                    tiles.append((tile_x, tile_y))
    return tiles


def build_url(url_template: str, zoom: int, tile_x: int, tile_y: int) -> str:
    return (
        url_template.replace("{z}", str(zoom))
        .replace("{x}", str(tile_x))
        .replace("{y}", str(tile_y))
    )


def output_path(output_dir: Path, zoom: int, tile_x: int, tile_y: int) -> Path:
    return output_dir / str(zoom) / str(tile_x) / f"{tile_y}.png"


def download_tile(
    url: str,
    destination: Path,
    user_agent: str,
    timeout: float,
    retries: int,
) -> bool:
    request = urllib.request.Request(url, headers={"User-Agent": user_agent})

    attempts = retries + 1
    for attempt in range(1, attempts + 1):
        try:
            destination.parent.mkdir(parents=True, exist_ok=True)
            with urllib.request.urlopen(request, timeout=timeout) as response:
                status = getattr(response, "status", 200)
                if status != 200:
                    raise urllib.error.HTTPError(url, status, f"HTTP {status}", response.headers, None)
                data = response.read()
            destination.write_bytes(data)
            return True
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, OSError) as exc:
            if destination.exists():
                destination.unlink()
            if attempt >= attempts:
                print(f"FAIL {url} -> {destination} ({exc})", file=sys.stderr)
                return False
            time.sleep(min(2.0, 0.5 * attempt))
    return False


def main() -> int:
    args = parse_args()

    if args.radius_km <= 0:
        print("radius-km must be > 0", file=sys.stderr)
        return 2
    if args.min_zoom < 0 or args.max_zoom < 0 or args.min_zoom > args.max_zoom:
        print("invalid zoom range", file=sys.stderr)
        return 2
    if not args.dry_run and not args.url_template:
        print(
            "--url-template is required for downloads. Use a provider that explicitly permits offline/bulk access.",
            file=sys.stderr,
        )
        return 2
    if args.url_template and not all(token in args.url_template for token in ("{z}", "{x}", "{y}")):
        print("--url-template must contain {z}, {x}, and {y} placeholders.", file=sys.stderr)
        return 2

    center_lat = clamp_lat(args.lat)
    center_lon = normalize_lon(args.lon)
    output_dir = args.output_dir

    zoom_to_tiles: dict[int, list[tuple[int, int]]] = {}
    total_tiles = 0
    for zoom in range(args.min_zoom, args.max_zoom + 1):
        tiles = enumerate_tiles(center_lat, center_lon, args.radius_km, zoom)
        zoom_to_tiles[zoom] = tiles
        total_tiles += len(tiles)

    print(
        f"Center: lat={center_lat:.6f}, lon={center_lon:.6f}, radius={args.radius_km:.1f} km"
    )
    print(f"Zooms: {args.min_zoom}..{args.max_zoom}")
    print(f"Output: {output_dir}")
    for zoom in range(args.min_zoom, args.max_zoom + 1):
        print(f"  z{zoom}: {len(zoom_to_tiles[zoom])} tiles")
    print(f"Total tiles: {total_tiles}")

    if args.dry_run:
        return 0

    downloaded = 0
    skipped = 0
    failed = 0

    for zoom in range(args.min_zoom, args.max_zoom + 1):
        for tile_x, tile_y in zoom_to_tiles[zoom]:
            dest = output_path(output_dir, zoom, tile_x, tile_y)
            if dest.exists() and not args.overwrite:
                skipped += 1
                continue

            url = build_url(args.url_template, zoom, tile_x, tile_y)
            ok = download_tile(
                url=url,
                destination=dest,
                user_agent=args.user_agent,
                timeout=args.timeout,
                retries=args.retries,
            )
            if ok:
                downloaded += 1
            else:
                failed += 1

            if args.delay > 0:
                time.sleep(args.delay)

    print(f"Downloaded: {downloaded}")
    print(f"Skipped: {skipped}")
    print(f"Failed: {failed}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
