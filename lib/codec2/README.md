# Vendored codec2 v1.2.0

This is a local vendor of [drowe67/codec2 v1.2.0](https://github.com/drowe67/codec2/tree/1.2.0)
source files, replacing the `sh123/esp32_codec2_arduino@1.0.7` PlatformIO
dependency.

## Why?

`sh123/esp32_codec2_arduino@1.0.7` bundles **codec2 v0.9.2**. The Mac-side
`pycodec2` Python wrapper (used by Sideband, Columba python clients,
and the LXST harness) links **libcodec2 v1.2.0**. The version gap
introduces a real audio quality regression on pyxis-decoded speech:

| Test signal                     | pycodec2 round-trip RMS | pyxis decode RMS |
|---------------------------------|-------------------------|------------------|
| 1kHz pure sine (amp 0.5)        | 1394                    | 6357             |
| 3-formant voice approx (amp 0.5)| 5749                    | 4378             |
| Real TTS speech (`say` output)  | 5796                    | **170**          |

The 30× drop on real speech doesn't reproduce on tones or simple
formants — it's content-specific. Decoder behavior is consistent
(decode_fail = 0 in every case) but the amplitude collapses. Local
pycodec2 vs the v0.9 esp32_codec2 lib using the same encoded bytes
diverges, so the bug is in the v0.9 decoder code path.

The `c2dec`/`c2enc`/cli tools are excluded via `srcFilter` in
`library.json` since pyxis only needs codec2 itself (no FreeDV / OFDM /
FSK / LDPC etc).

## Codebooks

`codebook*.c` files are the LSP/energy/etc. quantization tables. v0.9
codebook content matched v1.2's canonical text-form input (verified
with diff against `src/codebook/dlsp1.txt` etc), so the existing
generated `.c` files were carried over from `sh123/esp32_codec2_arduino`'s
src/. The `version.h` was hand-written from `cmake/version.h.in` since
codec2 v1.2's CMake-driven generation isn't running here.

## Updating

To pull a newer codec2 release: download
`https://api.github.com/repos/drowe67/codec2/tarball/refs/tags/<version>`,
extract, and replace `lib/codec2/src/*.c` and `*.h` with the contents
of the upstream `src/`. Don't overwrite the codebook `.c` files
unless you regenerate them via the upstream CMake build (codebook
content rarely changes).
