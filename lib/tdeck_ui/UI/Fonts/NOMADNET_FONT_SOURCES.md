# NomadNet embedded font sources

The generated `nomadnet_font_*.c` assets use real JetBrains Mono NL faces for
all glyphs available in JetBrains Mono NL. Noto Sans supplies only the 23
codepoints in `NomadNetFontCoverage.h` that JetBrains Mono NL 2.304 does not
contain. Ordinary text, box drawing, block elements, headings, bold, italic,
and bold italic therefore use JetBrains Mono NL.

## JetBrains Mono NL

Source: official JetBrains Mono v2.304 release archive:

- URL: `https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip`
- Archive SHA-256: `6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf`
- License: `LICENSE-JetBrains-Mono-OFL-1.1.txt`

Faces:

- Regular: `fb3b2575d7b0657359707993288f12a7360344d39387bb26050e276d61f6bd2a`
- Bold: `0198e841824025f8876e5c297f0b9b497ee8d6eb9969710a3328e1303f996ec3`
- Italic: `c7392a134293e1af1d36fbf04940dd844f632afbf97f8325d1591d3af5096cb8`
- Bold Italic: `6c2716f2e85101f4109a8c16916061eb38176b3b10342834858d6221ec2fda1b`

Columba's bundled `jetbrains_mono_nl_regular.ttf` has the same Regular-face
SHA-256 as the official v2.304 release.

## Noto Sans fallback donors

Source: Google Fonts commit
`2984c575fdce412ee02b2baaba67672b9a9434d8`, `ofl/notosans`, Noto Sans
v2.015 variable fonts. Static 400/700 instances were generated with fontTools
4.63.0 at width 100.

- Upright variable source SHA-256: `bfb7bb691513f12e734dc346c03a03f784912432d7e3fa8e56efcf906fe86b3d`
- Italic variable source SHA-256: `58e6e0ebd1931b29a365aa2d3e2ee9a9e831a3af7cf3ad1462d4e72154f0b291`
- License: `LICENSE-Noto-Sans-OFL-1.1.txt`

The generator derives Regular 400, Bold 700, Italic 400, and Bold Italic
700 instances. It validates each instance's weight, italic flag, and donor
coverage. Derived TTF hashes are intentionally not used because OpenType
timestamps can change serialization without changing glyph data.

The 23 donor codepoints are:

`U+0132 U+0133 U+2007 U+2008 U+2009 U+200A U+2012 U+2015 U+2052 U+20A1 U+20A3 U+20A4 U+20A6 U+20A7 U+20A9 U+20AD U+20B1 U+20B2 U+20B5 U+20B8 U+20B9 U+20BA U+20BC`

## Conversion

All eight assets were generated with `lv_font_conv` 1.5.3 using:

- sizes 12 and 16 pixels;
- 4 bits per pixel;
- LVGL C output;
- RLE compression and prefiltering enabled;
- kerning omitted;
- exact 385-codepoint coverage from `NomadNetFontCoverage.h`;
- JetBrains Mono NL first, with Noto Sans receiving only the 23 missing symbols.

Every generated font contains 385 reachable glyphs plus reserved descriptor 0.
All four 12px faces have line height 19 and baseline 5. All four 16px faces
have line height 26 and baseline 7.

Regenerate with Python `fontTools==4.63.0` and `lv_font_conv==1.5.3`:

`python tools/fonts/generate_nomadnet_fonts.py --converter /path/to/lv_font_conv`

The generator downloads immutable/checksummed upstream inputs, validates the
real weight/style metadata and exact donor coverage, and fails closed on tool
version or source drift.
