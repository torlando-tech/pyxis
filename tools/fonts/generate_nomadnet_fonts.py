#!/usr/bin/env python3
"""Regenerate the exact-coverage JetBrains Mono NL NomadNet LVGL fonts."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import tempfile
import urllib.request
from pathlib import Path
from zipfile import ZipFile

import fontTools
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = ROOT / "lib/tdeck_ui/UI/Fonts"
JETBRAINS_ARCHIVE_URL = (
    "https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/"
    "JetBrainsMono-2.304.zip"
)
NOTO_UPRIGHT_URL = (
    "https://raw.githubusercontent.com/google/fonts/"
    "2984c575fdce412ee02b2baaba67672b9a9434d8/ofl/notosans/"
    "NotoSans%5Bwdth,wght%5D.ttf"
)
NOTO_ITALIC_URL = (
    "https://raw.githubusercontent.com/google/fonts/"
    "2984c575fdce412ee02b2baaba67672b9a9434d8/ofl/notosans/"
    "NotoSans-Italic%5Bwdth,wght%5D.ttf"
)
SOURCE_SHA256 = {
    "JetBrainsMono-2.304.zip": "6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf",
    "NotoSans-Upright-VF.ttf": "bfb7bb691513f12e734dc346c03a03f784912432d7e3fa8e56efcf906fe86b3d",
    "NotoSans-Italic-VF.ttf": "58e6e0ebd1931b29a365aa2d3e2ee9a9e831a3af7cf3ad1462d4e72154f0b291",
}
FACE_SHA256 = {
    "JetBrainsMonoNL-Regular.ttf": "fb3b2575d7b0657359707993288f12a7360344d39387bb26050e276d61f6bd2a",
    "JetBrainsMonoNL-Bold.ttf": "0198e841824025f8876e5c297f0b9b497ee8d6eb9969710a3328e1303f996ec3",
    "JetBrainsMonoNL-Italic.ttf": "c7392a134293e1af1d36fbf04940dd844f632afbf97f8325d1591d3af5096cb8",
    "JetBrainsMonoNL-BoldItalic.ttf": "6c2716f2e85101f4109a8c16916061eb38176b3b10342834858d6221ec2fda1b",
}
REQUIRED = (
    set(range(0x20, 0x7F))
    | set(range(0x00A0, 0x0180))
    | set(range(0x2190, 0x219A))
    | {
        0x2007, 0x2008, 0x2009, 0x200A, 0x200B,
        0x2010, 0x2012, 0x2013, 0x2014, 0x2015,
        0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E,
        0x2020, 0x2021, 0x2022, 0x2026,
        0x2030, 0x2032, 0x2033, 0x2039, 0x203A,
        0x2044, 0x2052,
        0x20A1, 0x20A3, 0x20A4, 0x20A6, 0x20A7,
        0x20A9, 0x20AB, 0x20AC, 0x20AD, 0x20AE,
        0x20B1, 0x20B2, 0x20B4, 0x20B5, 0x20B8,
        0x20B9, 0x20BA, 0x20BC, 0x20BD,
        0x2500, 0x2550, 0x2551, 0x2554, 0x2557,
        0x255A, 0x255D, 0x2588, 0x2594, 0x25A0,
    }
)
FACES = {
    "regular": ("Regular", "", 400, False),
    "bold": ("Bold", "_bold", 700, False),
    "italic": ("Italic", "_italic", 400, True),
    "bold_italic": ("BoldItalic", "_bold_italic", 700, True),
}
JETBRAINS_MISSING = {
    0x0132, 0x0133, 0x2007, 0x2008, 0x2009, 0x200A, 0x2012, 0x2015,
    0x2052, 0x20A1, 0x20A3, 0x20A4, 0x20A6, 0x20A7, 0x20A9, 0x20AD,
    0x20B1, 0x20B2, 0x20B5, 0x20B8, 0x20B9, 0x20BA, 0x20BC,
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require_digest(path: Path, expected: str) -> None:
    actual = digest(path)
    if actual != expected:
        raise SystemExit(f"checksum mismatch for {path.name}: {actual}")


def download(url: str, destination: Path) -> None:
    with urllib.request.urlopen(url) as response, destination.open("wb") as output:
        shutil.copyfileobj(response, output)


def unicode_cmap(path: Path) -> set[int]:
    font = TTFont(path)
    return set().union(*(table.cmap.keys() for table in font["cmap"].tables if table.isUnicode()))


def make_noto_instance(source: Path, output: Path, weight: int) -> None:
    font = TTFont(source, recalcTimestamp=False)
    instantiateVariableFont(font, {"wght": weight, "wdth": 100}, inplace=True)
    font.recalcTimestamp = False
    font.save(output)


def require_noto_instance(path: Path, weight: int, italic: bool) -> None:
    font = TTFont(path)
    if getattr(font["OS/2"], "usWeightClass") != weight:
        raise SystemExit(f"weight mismatch for {path.name}")
    if bool(getattr(font["head"], "macStyle") & 2) != italic:
        raise SystemExit(f"style mismatch for {path.name}")
    if JETBRAINS_MISSING - unicode_cmap(path):
        raise SystemExit(f"coverage mismatch for {path.name}")


def adapter(symbol: str) -> str:
    return f'''/* Preserve original UTF-8 while mapping unsupported codepoints to ASCII
 * question mark instead of LVGL's missing-glyph rectangle. */
static bool {symbol}_get_glyph_dsc(const lv_font_t * font,
        lv_font_glyph_dsc_t * dsc, uint32_t letter, uint32_t letter_next)
{{
    if(nomadnet_font_has_codepoint(letter) &&
       lv_font_get_glyph_dsc_fmt_txt(font, dsc, letter, letter_next)) return true;
    return lv_font_get_glyph_dsc_fmt_txt(font, dsc, '?', 0);
}}

static const uint8_t * {symbol}_get_glyph_bitmap(
        const lv_font_t * font, uint32_t letter)
{{
    lv_font_glyph_dsc_t dsc;
    if(!nomadnet_font_has_codepoint(letter) ||
       !lv_font_get_glyph_dsc_fmt_txt(font, &dsc, letter, 0)) letter = '?';
    return lv_font_get_bitmap_fmt_txt(font, letter);
}}

'''


def generate(converter: Path, sources: Path, output_dir: Path, size: int,
             style: str, face: str, suffix: str) -> None:
    symbol = f"nomadnet_font_{size}{suffix}"
    jetbrains = sources / f"JetBrainsMonoNL-{face}.ttf"
    noto = sources / f"NotoSans-{face}.ttf"
    primary = REQUIRED & unicode_cmap(jetbrains)
    donor = REQUIRED - primary
    if len(primary) != 362 or len(donor) != 23 or donor - unicode_cmap(noto):
        raise SystemExit(f"coverage drift for {face}")

    output = output_dir / f"{symbol}.c"
    subprocess.run([
        str(converter), "--size", str(size), "--bpp", "4", "--format", "lvgl",
        "--no-kerning",
        "--font", str(jetbrains), "--symbols", "".join(chr(cp) for cp in sorted(primary)),
        "--font", str(noto), "--symbols", "".join(chr(cp) for cp in sorted(donor)),
        "--lv-font-name", symbol, "-o", str(output),
    ], check=True)
    text = output.read_text()
    if text.count("/* U+") != len(REQUIRED):
        raise SystemExit(f"generated glyph inventory mismatch for {symbol}")

    header_end = text.index(" ******************************************************************************/")
    provenance = f'''/*******************************************************************************
 * Bounded NomadNet font, {size} px, 4 bpp.
 * Primary source: JetBrains Mono NL {style.replace('_', ' ').title()}, Version 2.304.
 * Missing-codepoint donor: Noto Sans {style.replace('_', ' ').title()}, Version 2.015
 * (23 glyphs only). Both use SIL OFL 1.1; see
 * LICENSE-JetBrains-Mono-OFL-1.1.txt and LICENSE-Noto-Sans-OFL-1.1.txt.
 * Generated with lv_font_conv 1.5.3; kerning omitted, RLE compressed.
 * Exact coverage is defined by NomadNetFontCoverage.h.
 ******************************************************************************/'''
    text = provenance + text[header_end + len(" ******************************************************************************/"):]
    text = text.replace('#include "lvgl/lvgl.h"', '#include "lvgl.h"')
    include_end = text.index('#endif', text.index('#ifdef LV_LVGL_H_INCLUDE_SIMPLE')) + len('#endif')
    text = text[:include_end] + '''
#include "NomadNetFontCoverage.h"

#if !LV_USE_FONT_COMPRESSED
#error "NomadNet fonts require LV_USE_FONT_COMPRESSED=1"
#endif''' + text[include_end:]
    marker = "/*Initialize a public general font descriptor*/"
    text = text.replace(marker, adapter(symbol) + marker, 1)
    text = text.replace(
        ".get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/",
        f".get_glyph_dsc = {symbol}_get_glyph_dsc,", 1,
    ).replace(
        ".get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/",
        f".get_glyph_bitmap = {symbol}_get_glyph_bitmap,", 1,
    )
    output.write_text(text.rstrip() + "\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--converter", type=Path, required=True,
                        help="path to lv_font_conv 1.5.3")
    args = parser.parse_args()
    if not args.converter.is_file():
        raise SystemExit(f"missing converter: {args.converter}")
    if fontTools.__version__ != "4.63.0":
        raise SystemExit(
            f"fontTools 4.63.0 required, found {fontTools.__version__}"
        )
    version = subprocess.run([str(args.converter), "--version"], check=True,
                             text=True, capture_output=True).stdout.strip()
    if version != "1.5.3":
        raise SystemExit(f"lv_font_conv 1.5.3 required, found {version}")
    args.output.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="pyxis-nomadnet-fonts-") as temporary:
        sources = Path(temporary)
        archive = sources / "JetBrainsMono-2.304.zip"
        upright = sources / "NotoSans-Upright-VF.ttf"
        italic = sources / "NotoSans-Italic-VF.ttf"
        download(JETBRAINS_ARCHIVE_URL, archive)
        download(NOTO_UPRIGHT_URL, upright)
        download(NOTO_ITALIC_URL, italic)
        for path in (archive, upright, italic):
            require_digest(path, SOURCE_SHA256[path.name])
        with ZipFile(archive) as zipped:
            for face, _, _, _ in FACES.values():
                name = f"JetBrainsMonoNL-{face}.ttf"
                with zipped.open(f"fonts/ttf/{name}") as source, (sources / name).open("wb") as output:
                    shutil.copyfileobj(source, output)
        for face, _, weight, is_italic in FACES.values():
            make_noto_instance(italic if is_italic else upright,
                               sources / f"NotoSans-{face}.ttf", weight)
        for name, expected in FACE_SHA256.items():
            require_digest(sources / name, expected)
        for face, _, weight, is_italic in FACES.values():
            require_noto_instance(sources / f"NotoSans-{face}.ttf", weight, is_italic)
        for size in (12, 16):
            for style, (face, suffix, _, _) in FACES.items():
                generate(args.converter, sources, args.output, size, style, face, suffix)

    print("generated 8 exact-coverage JetBrains Mono NL NomadNet fonts")


if __name__ == "__main__":
    main()
