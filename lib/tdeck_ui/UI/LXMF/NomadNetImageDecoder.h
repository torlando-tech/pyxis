// Bounded WebP image decoder for NomadNet inline page images.
//
// Upstream NomadNet nodes serve page media as WebP (Node.serve_media converts
// PNG/JPG/etc. on disk to WebP before the wire; reference pinned to
// markqvist/NomadNet e1e8ab8, v1.4.3). This module decodes that WebP into an
// RGB565 buffer matching the T-Deck framebuffer depth (LV_COLOR_DEPTH 16),
// mirroring the proven MapScreen lodepng -> PSRAM RGB565 pattern.
//
// Bounds (enforced before any allocation):
//   - input bytes  <= MAX_IMAGE_INPUT_BYTES  (reject, no decode)
//   - width/height <= max_dimension          (reject, no decode; the caller
//     passes DocumentParser::MAX_IMAGE_PIXEL_BUDGET as the authored budget
//     cap when the tag authored one, else MAX_DECODABLE_DIMENSION)
// Decoding uses the vendored decode-only libwebp (lib/webp). No threads.
// The output buffer is PSRAM-backed on device (heap_caps_malloc) and plain
// malloc on the host (see the ARDUINO_ARCH_ESP32 seam, matching MapScreen).
#pragma once

#include <cstddef>
#include <cstdint>

namespace UI::LXMF::NomadNet {

enum class ImageDecodeResult {
    OK,
    NOT_IMAGE,       // bytes are not a valid WebP container
    DIMENSIONS,      // container dimensions exceed the requested cap
    INPUT_TOO_LARGE, // input exceeds MAX_IMAGE_INPUT_BYTES
    DECODE_FAILED,   // valid container, but bitstream decode failed
    ALLOC_FAILED,    // PSRAM/heap allocation failed
};

// Decoded RGB565 image (packed little-endian 16bpp, row-major, no padding).
// `pixels` is owned by the module allocation; release with
// release_decoded_image(). width*height*2 bytes total.
struct DecodedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::uint16_t* pixels = nullptr;
};

// Maximum accepted input size. Upstream has no client-side cap (its node
// clamps dimensions to 1200 at conversion); the device caps decode at
// MAX_DECODABLE_DIMENSION, so the input cap only governs the pre-decode
// validation window. 512 KiB admits realistic node-converted media.
static constexpr std::size_t MAX_IMAGE_INPUT_BYTES = 512u * 1024u;

// Hard decode dimension cap. A 640x480 RGBA staging buffer is 1.2 MiB and the
// RGB565 output 614 KiB in PSRAM; larger media degrades to the alt-text
// placeholder rather than risking the PSRAM budget shared with LVGL and the
// map tile caches.
static constexpr std::uint16_t MAX_DECODABLE_DIMENSION = 640;

// Decode `data` (a WebP bitstream) into RGB565. `max_dimension` caps both
// axes (use MAX_DECODABLE_DIMENSION, or the tag's authored pixel budget when
// smaller). On OK, *out owns a buffer to release with release_decoded_image.
// Never throws; all failures are returned via the result code.
ImageDecodeResult decode_webp_rgb565(const std::uint8_t* data, std::size_t bytes,
                                     std::uint16_t max_dimension,
                                     DecodedImage& out);

// Release the staging/output buffers held by `image` (no-op if empty).
void release_decoded_image(DecodedImage& image);

} // namespace UI::LXMF::NomadNet
