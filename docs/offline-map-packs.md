# Offline map packs

Pyxis can read prebuilt raster map packs from an SD card. The host importer accepts only a **local, user-supplied XYZ PNG directory** and never downloads tiles. Before importing data, verify that its provider and license permit offline storage and use.

## Input requirements

The input must contain only canonical XYZ paths:

```text
<xyz-directory>/<z>/<x>/<y>.png
```

- `z`, `x`, and `y` are canonical unsigned decimal integers: `0`, not `00` or `+0`.
- Zoom is 0 through 22; X and Y are each in `0..2^z-1`.
- Every PNG must be a valid, fully decodable, non-interlaced 256×256 PNG. Critical-chunk ordering, CRCs, palette state, deflate termination, exact scanline length, and row filters are validated; interlaced PNGs are intentionally unsupported.
- Symlinks and unexpected files or directory levels are rejected.
- Zoom levels must be contiguous. At each zoom, tiles must form a complete X/Y rectangle.
- A rectangle crossing the antimeridian is accepted only with an explicit `--antimeridian-zoom Z`. Its X coordinates must form exactly two intervals, one beginning at X=0 and one ending at `2^Z-1`. The importer never guesses that a gap means an antimeridian crossing.

The importer requires non-empty printable-ASCII display name, attribution, source, and license fields. These values are stored in the manifest; they do not grant rights or replace compliance with the provider's terms.

## Build a pack

Run from the repository root:

```sh
python3 tools/maps/build_map_pack.py ./my-xyz /media/$USER/SDCARD \
  --pack-id regional-map \
  --name "Regional Map" \
  --attribution "Map data (c) Example contributors" \
  --source "Local export supplied by the user" \
  --license "CC-BY-4.0"
```

For explicitly known antimeridian coverage, repeat the option for each affected zoom:

```sh
  --antimeridian-zoom 8 --antimeridian-zoom 9
```

The pack ID must match `[a-z0-9_-]{1,31}`. The destination is:

```text
<output-root>/pyxis-map/packs/<pack-id>/manifest.pmp
<output-root>/pyxis-map/packs/<pack-id>/tiles/<z>/<x>/<y>.png
```

The output root is normally the mounted SD-card root. The importer refuses to replace an existing pack.

## Resource limits and safe publication

Defaults bound an import to 100,000 tiles and 8 GiB of tile bytes. Set stricter limits when appropriate:

```sh
  --max-tiles 25000 --max-bytes 2147483648
```

All input is validated before publication. On Linux, the importer pins source and output directories with descriptor-relative, no-follow operations; rejects changed files and directory identities; copies under the caller's quotas; writes the deterministic manifest in a private sibling directory; flushes files and directories; and independently validates the copied tree. Publication requires Linux `renameat2(RENAME_NOREPLACE)` and fails closed when atomic no-replace is unavailable. A pre-commit failure removes its temporary directory. If the post-rename parent-directory sync fails, the CLI reports exit status 3 and explicitly says the pack is published but durability is uncertain.

The manifest is the firmware's `PMPK` version 1 wire format: a 16-byte little-endian header, five length-prefixed printable-ASCII strings, zoom/count fields, fixed 26-byte per-zoom extents, and a final IEEE CRC-32. The Python serializer is fixture-tested byte-for-byte against the committed 153-byte C++ sample.

## Network policy

Pack creation is deliberately local-files-only. The importer contains no network client, URL support, provider endpoint, or tile-fetch mode. Obtain tiles separately under terms that explicitly allow offline use; do not use this tool to bulk-fetch from a live tile service.
