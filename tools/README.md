# Map tile prefetch script

`prefetch_map_tiles.py` downloads raster XYZ map tiles in the same layout Pyxis expects on the SD card:

```text
tiles/{z}/{x}/{y}.png
```

## How it works

- Uses a center point and radius in kilometers.
- Computes all map tiles that intersect that area for the selected zoom range.
- Downloads each tile as a PNG.
- Saves the files into the Pyxis-compatible `tiles/` folder structure.

## Tile provider requirement

You must provide a tile URL template for a provider that explicitly permits bulk and offline downloads.

Do not use `tile.openstreetmap.org` for this script. OpenStreetMap's public tile service prohibits bulk downloading and offline prefetching under the [OSM Tile Usage Policy](https://operations.osmfoundation.org/policies/tiles/).

Use one of these instead:

- a self-hosted tile server
- a commercial provider and plan that explicitly allows offline or bulk prefetching

The default center point comes from the coordinates shown in the provided screenshot:

- latitude: `43.978093`
- longitude: `-66.143359`
- radius: `200 km`

## Basic usage

Dry run:

```bash
python tools/prefetch_map_tiles.py --dry-run
```

Download tiles:

```bash
python tools/prefetch_map_tiles.py --url-template "https://tiles.example.com/{z}/{x}/{y}.png" --min-zoom 8 --max-zoom 14
```

Custom area:

```bash
python tools/prefetch_map_tiles.py --url-template "https://tiles.example.com/{z}/{x}/{y}.png" --lat 44.0 --lon -66.1 --radius-km 100 --min-zoom 10 --max-zoom 15
```

## Copy to SD card

After the download finishes, copy the generated `tiles/` directory to the SD card root so Pyxis can load files like:

```text
S:tiles/14/4823/6160.png
```

## Notes

- Existing files are skipped unless `--overwrite` is used.
- `--url-template` is required for actual downloads.
- Bounding boxes that cross the antimeridian are split into two tile ranges so dateline-adjacent areas work correctly.
