# Offline map packs

Pyxis can read prebuilt raster map packs from an SD card. Importers accept only **local, user-supplied map files** and never download tiles. Before importing data, verify that its provider and license permit offline storage and use.

## Browser installer (recommended)

Open the Pyxis web flasher in current Chrome or Edge and use **Install Offline Maps**:

1. Download a Coalition/MUI XYZ ZIP separately.
2. Turn the T-Deck off, remove its SD card, and mount the card on the computer.
3. Choose the ZIP, enter a map name and pack ID, and wait for local validation.
4. Click **Install and Enable**, then choose the SD-card root.
5. After the verified completion message, safely eject the card and return it to the T-Deck.

The browser accepts stored ZIP entries rooted at either `<z>/<x>/<y>.png` or
`maps/<style>/<z>/<x>/<y>.png`. It rejects mixed styles, traversal, duplicate
paths or tile keys, symlinks, encrypted or unsupported compression, malformed
ZIP records, unsafe PNGs, quota violations, and noncanonical XYZ paths. Tiles
are validated and read back one at a time; the complete archive is not loaded
into JavaScript memory.

Installation refuses to overwrite an existing pack. A verified pack from an
earlier activation failure can be reselected with the exact same ZIP and
metadata. Tiles are published first, `manifest.pmp` is published last, and only
then is a CRC-protected redundant active-map-set slot updated and read back. An
interruption therefore leaves the previous map set usable. Unrelated SD-card
files are untouched; cleanup removes only receipt-owned installer paths.

## Packs and map sets

A **pack** is an independent installation, update, removal, provenance, and
attribution unit. A **map set** is an ordered collection of compatible packs
that Pyxis uses together. Geographic packs do not require manual switching.

For example, `regional-overview-z0-z9` and `local-detail-z0-z16` can both
belong to the `osm-bright` map set. Pyxis checks the local-detail pack first and
then the regional-overview pack for every requested XYZ key. The detail pack
supplies its z10-z16 area while the overview pack supplies broader z0-z9
coverage. Outside the detail area at z10-z16, the key is uncovered. If a
higher-priority pack declares a tile but its file is missing, Pyxis safely tries
the next compatible pack.

The browser currently imports Coalition/MUI OSM Bright data into the bounded
`osm-bright` map set. Newly installed packs take priority over earlier packs
where their exact row-span coverage overlaps. Different styles or providers
must use a different map set rather than being blended implicitly.

## Input requirements

The input must contain only canonical XYZ paths:

```text
<xyz-directory>/<z>/<x>/<y>.png
```

- `z`, `x`, and `y` are canonical unsigned decimal integers: `0`, not `00` or `+0`.
- Zoom is 0 through 22; X and Y are each in `0..2^z-1`.
- Every PNG must be a valid, fully decodable, non-interlaced 256×256 PNG with at most 8 bits per sample. Critical-chunk ordering, supported ancillary semantics, CRCs, palette state, bounded deflate termination, exact scanline length, and row filters are validated; interlaced and 16-bit PNGs are intentionally unsupported by the browser importer.
- Symlinks and unexpected files or directory levels are rejected.
- Zoom levels must be contiguous. Complete rectangles use the compact version-1 manifest; sparse state or polygon coverage uses exact bounded row spans in version 2.
- A complete rectangle crossing the antimeridian can use `--antimeridian-zoom Z` for the compact version-1 representation. With explicit `--sparse`, disjoint coverage is represented exactly as row spans rather than guessed to be an antimeridian rectangle.

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

For an intentionally sparse state or polygon export, explicitly add `--sparse`.
The default CLI mode continues to reject incomplete rectangles so an accidental
missing tile cannot silently become declared sparse coverage.

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

## Legacy single-pack selection from the CLI

With the SD card still mounted on the host and not in use by Pyxis, write the
pack ID to the bounded selection marker without a trailing newline:

```sh
printf '%s' 'regional-map' > /media/$USER/SDCARD/pyxis-map/active-pack
sync /media/$USER/SDCARD
```

Safely unmount the card before inserting it into the T-Deck. Pyxis validates
the marker, manifest, declared coverage, and canonical tile path before use.
This marker selects one legacy pack only; it does not build a composited map
set. The browser's redundant map-set records take precedence when present.
Opening the map refreshes the selection. A missing or malformed pack is shown
as unavailable; firmware never formats, repairs, deletes, or writes an
immutable pack.

## Resource limits and safe publication

Defaults bound an import to 100,000 tiles and 8 GiB of tile bytes. Set stricter limits when appropriate:

```sh
  --max-tiles 25000 --max-bytes 2147483648
```

All input is validated before publication. On Linux, the importer pins source and output directories with descriptor-relative, no-follow operations; rejects changed files and directory identities; copies under the caller's quotas; writes the deterministic manifest in a private sibling directory; flushes files and directories; and independently validates the copied tree. Publication requires Linux `renameat2(RENAME_NOREPLACE)` and fails closed when atomic no-replace is unavailable. A pre-commit failure removes its temporary directory. If the post-rename parent-directory sync fails, the CLI reports exit status 3 and explicitly says the pack is published but durability is uncertain.

The firmware accepts two bounded `PMPK` wire formats. Version 1 retains fixed
26-byte per-zoom rectangular extents. Version 2 stores up to 512 canonical,
sorted 13-byte `(zoom, y, x-minimum, x-maximum)` row spans and verifies that
their exact tile total matches the manifest. Both use a 16-byte little-endian
header, five bounded printable-ASCII strings, and a final IEEE CRC-32.

The browser enables packs through redundant bounded `active-pack.0` and
`active-pack.1` records with generations and CRC-32. Map-set record version 2
stores the map-set ID, shared attribution, up to eight ordered pack IDs, and up
to 512 total canonical row spans. Firmware selects the newest valid record,
resolves each tile in pack-priority order, and ignores a torn or malformed peer.
Legacy 48-byte version-1 records and the plain `active-pack` marker remain
backward-compatible single-pack fallbacks.

## Network policy

Pack creation is deliberately local-files-only. The importer contains no network client, URL support, provider endpoint, or tile-fetch mode. Obtain tiles separately under terms that explicitly allow offline use; do not use this tool to bulk-fetch from a live tile service.

The production map screen is also SD-only. It does not initiate DNS, HTTP, or
TLS and has no online-download setting. Its lookup order is the decoded PSRAM
cache, the enabled immutable packs in deterministic priority order, and then the
bounded legacy SD tile cache.
