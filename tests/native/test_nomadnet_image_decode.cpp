// Host test: bounded WebP -> RGB565 decoder (NomadNetImageDecoder).
// Exercises the real vendored libwebp decode path against:
//   - the authoritative upstream fixture demo.webp (RNS-synced reference
//     e1e8ab8, nomadnet/ui/textui/images/demo.webp, 256x256 RGBA,
//     sha256 e7b8bd740d8fbf9d88480bc4e4fe42559373065ad6de2e8e114cce7c64740c06);
//   - synthetic fixtures for the exact decode cap, over-cap, transparency,
//     and non-WebP inputs.
// All bounds must hold: no allocation before WebPGetInfo, dimension cap
// enforced before decode, staging freed on every failure path.

#include "NomadNetImageDecoder.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace UI::LXMF::NomadNet;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static std::vector<std::uint8_t> read_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<std::uint8_t> data(size > 0 ? (std::size_t)size : 0);
    if (size > 0 && std::fread(data.data(), 1, data.size(), f) != data.size()) {
        std::fclose(f);
        return {};
    }
    std::fclose(f);
    return data;
}

int main(int argc, char** argv) {
    std::string fixture_dir = argc > 1 ? argv[1] : "fixtures/nomadnet_image";

    // 1. Authoritative fixture: demo.webp decodes to 256x256; known pixels
    //    (verified with PIL at the reference checkout):
    //      (0,0)     = (0,0,0,0)     -> transparent, RGB565 black
    //      (10,10)   = (0,0,0,0)     -> transparent, RGB565 black
    //      (128,128) = (5,5,5,255)   -> opaque near-black
    {
        const std::vector<std::uint8_t> data =
            read_file(fixture_dir + "/demo.webp");
        CHECK(!data.empty());
        DecodedImage image;
        const ImageDecodeResult result =
            decode_webp_rgb565(data.data(), data.size(), 0, image);
        CHECK(result == ImageDecodeResult::OK);
        CHECK(image.width == 256);
        CHECK(image.height == 256);
        CHECK(image.pixels != nullptr);
        if (result == ImageDecodeResult::OK) {
            const std::uint16_t* px = image.pixels;
            // (0,0): fully transparent -> ARGB 0 -> RGB565 0x0000
            CHECK(px[0] == 0x0000);
            // (10,10): transparent
            CHECK(px[10] == 0x0000);
            // (128,128): (5,5,5,255) -> r=5>>3=0, g=5>>2=1, b=5>>3=0
            //   RGB565 = (0<<11)|(1<<5)|0 = 0x0020
            CHECK(px[128 * 256 + 128] == 0x0020);
        }
        release_decoded_image(image);
        CHECK(image.pixels == nullptr);
    }

    // 2. Exact-cap fixture: solid 640x480 (255,128,64) RGB565.
    //    r=255>>3=31, g=128>>2=32, b=64>>3=8 -> (31<<11)|(32<<5)|8 = 0xFC08
    //    (value cross-checked against PIL's decode of the same fixture).
    {
        const std::vector<std::uint8_t> data =
            read_file(fixture_dir + "/solid_640x480.webp");
        CHECK(!data.empty());
        DecodedImage image;
        const ImageDecodeResult result =
            decode_webp_rgb565(data.data(), data.size(), 0, image);
        CHECK(result == ImageDecodeResult::OK);
        CHECK(image.width == 640 && image.height == 480);
        if (result == ImageDecodeResult::OK) {
            for (std::size_t i = 0; i < 16; ++i)
                CHECK(image.pixels[i] == 0xFC08);
            const std::size_t last = (std::size_t)640 * 480 - 1;
            CHECK(image.pixels[last] == 0xFC08);
        }
        release_decoded_image(image);
    }

    // 3. Over-cap: 700x500 with max_dimension=640 -> DIMENSIONS, no pixels.
    {
        const std::vector<std::uint8_t> data =
            read_file(fixture_dir + "/overcap_700x500.webp");
        CHECK(!data.empty());
        DecodedImage image;
        CHECK(decode_webp_rgb565(data.data(), data.size(), 0, image) ==
              ImageDecodeResult::DIMENSIONS);
        CHECK(image.pixels == nullptr);
        // A larger authored budget would admit it (cap is a parameter).
        CHECK(decode_webp_rgb565(data.data(), data.size(), 700, image) ==
              ImageDecodeResult::OK);
        if (image.pixels) { CHECK(image.width == 700 && image.height == 500); release_decoded_image(image); }
    }

    // 4. Non-WebP bytes -> NOT_IMAGE (magic validation).
    {
        const std::uint8_t junk[64] = {};
        DecodedImage image;
        CHECK(decode_webp_rgb565(junk, sizeof(junk), 0, image) ==
              ImageDecodeResult::NOT_IMAGE);
        CHECK(image.pixels == nullptr);
    }

    // 5. Truncated WebP (first 16 bytes of demo) -> NOT_IMAGE or
    //    DECODE_FAILED, never OK, never leaks a buffer.
    {
        const std::vector<std::uint8_t> data =
            read_file(fixture_dir + "/demo.webp");
        CHECK(data.size() > 16);
        DecodedImage image;
        const ImageDecodeResult result =
            decode_webp_rgb565(data.data(), 16, 0, image);
        CHECK(result == ImageDecodeResult::NOT_IMAGE ||
              result == ImageDecodeResult::DECODE_FAILED);
        CHECK(image.pixels == nullptr);
    }

    // 6. Transparent 2x2 -> all-zero RGB565; half-alpha 2x2 -> same RGB.
    {
        const std::vector<std::uint8_t> t =
            read_file(fixture_dir + "/transparent_2x2.webp");
        const std::vector<std::uint8_t> h =
            read_file(fixture_dir + "/halfalpha_2x2.webp");
        CHECK(!t.empty() && !h.empty());
        DecodedImage image;
        CHECK(decode_webp_rgb565(t.data(), t.size(), 0, image) ==
              ImageDecodeResult::OK);
        if (image.pixels) {
            for (std::size_t i = 0; i < 4; ++i) CHECK(image.pixels[i] == 0x0000);
            release_decoded_image(image);
        }
        CHECK(decode_webp_rgb565(h.data(), h.size(), 0, image) ==
              ImageDecodeResult::OK);
        // (255,0,0,128) -> RGB565 red = 31<<11 = 0xF800
        if (image.pixels) {
            for (std::size_t i = 0; i < 4; ++i) CHECK(image.pixels[i] == 0xF800);
            release_decoded_image(image);
        }
    }

    if (failures == 0) std::printf("ALL IMAGE DECODER TESTS PASSED\n");
    return failures == 0 ? 0 : 1;
}
