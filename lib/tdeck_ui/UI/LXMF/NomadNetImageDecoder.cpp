// Bounded WebP -> RGB565 decoder implementation. See NomadNetImageDecoder.h
// for the bounds contract. Vendor source: lib/webp (libwebp v1.3.2
// decode-only subset, VENDOR_MANIFEST.json).
#include "NomadNetImageDecoder.h"

// Vendored copy of libwebp v1.3.2 src/webp/decode.h, placed next to this
// file because PlatformIO exposes the webp library's C sources to dependent
// translation units but not its nested src/webp include prefix.
#include "webp_decode_vendored.h"

#include <cstdlib>
#include <cstring>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
// MapScreen convention: large UI buffers go to PSRAM (8 MB OPI on the
// T-Deck Plus) so they do not compete with the internal heap.
#define PYXIS_IMAGE_MALLOC(size) \
    static_cast<std::uint8_t*>(heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
#define PYXIS_IMAGE_FREE(ptr) heap_caps_free(ptr)
#else
#define PYXIS_IMAGE_MALLOC(size) static_cast<std::uint8_t*>(std::malloc((size)))
#define PYXIS_IMAGE_FREE(ptr) std::free(ptr)
#endif

namespace UI::LXMF::NomadNet {
namespace {

// Convert one packed RGBA pixel as read little-endian (memory order R,G,B,A,
// so r = bits 0-7, g = 8-15, b = 16-23) to 16-bit RGB565 (R:11 G:10 B:5).
// Each 8-bit channel is truncated to its high 5/6 bits (>>3, >>2, >>3).
// C++11-safe: single-expression constexpr body.
constexpr std::uint16_t rgba_to_rgb565(std::uint32_t rgba) {
    return static_cast<std::uint16_t>(
        (((rgba & 0xffU) >> 3) << 11) |
        ((((rgba >> 8) & 0xffU) >> 2) << 5) |
        (((rgba >> 16) & 0xffU) >> 3));
}

// The firmware display (ST7789, T-Deck) runs with LV_COLOR_16_SWAP == 1
// (lib/lv_conf.h): LVGL's in-memory pixel byte order is the byte-swap of the
// mathematical RGB565 value (the proven map-tile path stores pixels via
// lv_color_make, which honors the swap). Pixel buffers handed to
// lv_draw_img_decoded must match that layout, so byte-swap the 16-bit value
// on the device. (A pre-swap buffer renders with red and blue swapped on the
// physical display.) The host decode-test build keeps native order: it has no
// LVGL config and asserts against mathematical RGB565.
constexpr std::uint16_t to_display_pixel(std::uint16_t rgb565) {
#if defined(ARDUINO_ARCH_ESP32)
    return static_cast<std::uint16_t>(
        ((rgb565 & 0x00ffU) << 8) | ((rgb565 & 0xff00U) >> 8));
#else
    return rgb565;
#endif
}

// Staging bound: RGBA bytes for the largest decodable image (640x640).
constexpr long long MAX_STAGING_BYTES =
    (long long)MAX_DECODABLE_DIMENSION * (long long)MAX_DECODABLE_DIMENSION * 4LL;

} // namespace

ImageDecodeResult decode_webp_rgb565(const std::uint8_t* data, std::size_t bytes,
                                     std::uint16_t max_dimension,
                                     DecodedImage& out) {
    out = DecodedImage{};
    if (data == nullptr || bytes == 0) return ImageDecodeResult::NOT_IMAGE;
    if (bytes > MAX_IMAGE_INPUT_BYTES) return ImageDecodeResult::INPUT_TOO_LARGE;
    if (max_dimension == 0) max_dimension = MAX_DECODABLE_DIMENSION;

    // 1. Validate the container and enforce the dimension cap before any
    //    decode staging is allocated.
    int width = 0;
    int height = 0;
    if (!WebPGetInfo(data, bytes, &width, &height))
        return ImageDecodeResult::NOT_IMAGE;
    if (width <= 0 || height <= 0) return ImageDecodeResult::NOT_IMAGE;
    const long long dim_max = (long long)width > (long long)height ? width : height;
    if (dim_max > max_dimension) return ImageDecodeResult::DIMENSIONS;

    const long long pixel_bytes = (long long)width * height * 4;
    if (pixel_bytes > MAX_STAGING_BYTES) return ImageDecodeResult::DIMENSIONS;

    // 2. Allocate RGBA staging (PSRAM on device) and decode directly into it.
    //    WebPDecodeRGBAInto writes RGBA: R,G,B,A per pixel, stride in bytes.
    std::uint8_t* staging = PYXIS_IMAGE_MALLOC(static_cast<std::size_t>(pixel_bytes));
    if (staging == nullptr) return ImageDecodeResult::ALLOC_FAILED;
    const uint8_t* result =
        WebPDecodeRGBAInto(data, bytes, staging,
                           static_cast<size_t>(pixel_bytes),
                           static_cast<int>(width * 4));
    if (result == nullptr) {
        PYXIS_IMAGE_FREE(staging);
        return ImageDecodeResult::DECODE_FAILED;
    }

    // 3. Convert RGBA -> RGB565 in place, forward. Source pixel k spans bytes
    //    [4k, 4k+4); the already-written destination pixels 0..k-1 occupy
    //    bytes [0, 2k). Since 4k >= 2k for all k, the source read for pixel k
    //    is strictly above every prior destination write, so no read touches
    //    a byte already converted. (Right-to-left would corrupt pixel 0, whose
    //    source bytes 2..3 sit under dst[1].) No scratch buffer needed, so no
    //    added stack pressure for the FreeRTOS/UI task.
    for (int y = 0; y < height; ++y) {
        const std::size_t row = (std::size_t)y * (std::size_t)width;
        const std::uint8_t* src = staging + row * 4;
        std::uint16_t* dst = reinterpret_cast<std::uint16_t*>(staging) + row;
        for (int x = 0; x < width; ++x) {
            const std::size_t o = (std::size_t)x * 4;
            const std::uint32_t rgba =
                static_cast<std::uint32_t>(src[o]) |
                (static_cast<std::uint32_t>(src[o + 1]) << 8) |
                (static_cast<std::uint32_t>(src[o + 2]) << 16) |
                (static_cast<std::uint32_t>(src[o + 3]) << 24);
            dst[x] = to_display_pixel(rgba_to_rgb565(rgba));
        }
    }

    out.width = (std::uint32_t)width;
    out.height = (std::uint32_t)height;
    out.pixels = reinterpret_cast<std::uint16_t*>(staging);
    return ImageDecodeResult::OK;
}

void release_decoded_image(DecodedImage& image) {
    if (image.pixels != nullptr) PYXIS_IMAGE_FREE(image.pixels);
    image = DecodedImage{};
}

} // namespace UI::LXMF::NomadNet
