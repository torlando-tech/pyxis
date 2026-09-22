// Host test: NomadNet page-image wire contract (slice 3).
// Verifies the /media request encoder, url parsing, WebP validation,
// bounded response normalization, and cache key — all against the reference
// semantics (Browser.__get_image_request_data, parse_url, Node.serve_media,
// image_cache_path) from the RNS-synced NomadNet at e1e8ab8 / v1.4.3.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "NomadNetImageProtocol.h"

using namespace UI::LXMF::NomadNet;

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); ++failures; } \
} while (0)

static bool bytes_equal(const std::vector<uint8_t>& a, const char* hex) {
    std::size_t i = 0;
    if (a.size() * 2 != std::string(hex).size()) return false;
    for (const auto b : a) {
        char buf[3] = {};
        std::snprintf(buf, sizeof buf, "%02x", b);
        if (std::string(hex + i, 2) != buf) return false;
        i += 2;
    }
    return true;
}

static void test_request_data() {
    // {"path": "/media/demo.webp", "key": None}
    //   0x82 0xa4 "path" 0xb0 "/media/demo.webp" 0xa3 "key" 0xc0
    {
        const auto d = media_request_data("/media/demo.webp");
        CHECK(bytes_equal(d, "82a470617468b02f6d656469612f64656d6f2e77656270a36b6579c0"),
              "demo.webp request-data bytes");
    }
    // Short path (single char) still fixstr.
    {
        const auto d = media_request_data("/x");
        CHECK(bytes_equal(d, "82a470617468a22f78a36b6579c0"), "short path request-data bytes");
    }
    // Overlong path (40 bytes) uses str8 (0xd9).
    {
        const std::string p(40, 'a');
        const auto d = media_request_data(p);
        // 82 a4 "path" d9 28 <40x a> a3 "key" c0  => 1+5+2+40+4+1 = 53
        CHECK(d.size() == 53, "str8 path length");
        CHECK(d[6] == 0xd9 && d[7] == 40, "str8 header");
        CHECK(d.back() == 0xc0, "trailing nil");
    }
    // The last byte must ALWAYS be the explicit nil (the skill's "nil not
    // empty span" rule), and byte 0 the 2-entry fixmap.
    {
        // Named temporaries: the c_str() pointers are only valid for their
        // lifetime, so they must outlive the loop that uses them.
        const std::string path_31(31, 'b');
        const std::string path_200(200, 'c');
        for (const char* p : {"/", "/a", path_31.c_str(), path_200.c_str()}) {
            const auto d = media_request_data(p);
            CHECK(!d.empty() && d[0] == 0x82, "fixmap header present");
            CHECK(!d.empty() && d.back() == 0xc0, "explicit nil terminator present");
        }
    }
}

static void test_parse_url() {
    // ":/media/demo.webp" -> same destination, path preserved.
    {
        const auto u = parse_image_url(":/media/demo.webp");
        CHECK(u.valid && u.same_destination, "leading-colon same-destination");
        CHECK(u.destination_hex.empty(), "same-destination has no hex");
        CHECK(u.path == "/media/demo.webp", "same-destination path preserved");
    }
    // "aabb... (32 hex):/media/x.webp" -> remote destination.
    {
        const std::string hex(32, 'a');
        const auto u = parse_image_url(hex + ":/media/x.webp");
        CHECK(u.valid && !u.same_destination, "remote destination parsed");
        CHECK(u.destination_hex == hex, "remote destination hex preserved");
        CHECK(u.path == "/media/x.webp", "remote path preserved");
    }
    // Malformed: short first component.
    {
        const auto u = parse_image_url("zz:/media/x.webp");
        CHECK(!u.valid, "short non-hex destination rejected");
    }
    // Single bare destination -> default path.
    {
        const std::string hex(32, 'b');
        const auto u = parse_image_url(hex);
        CHECK(u.valid && u.path == "/", "bare destination default path");
    }
    // Empty string -> invalid.
    CHECK(!parse_image_url("").valid, "empty url invalid");
}

static void test_magic_and_normalize() {
    // RIFF....WEBP is valid.
    {
        std::vector<uint8_t> v(12, 0);
        const char riff[] = "RIFF";
        const char webp[] = "WEBP";
        std::memcpy(v.data(), riff, 4);
        std::memcpy(v.data() + 8, webp, 4);
        CHECK(looks_like_webp(v.data(), v.size()), "RIFF/WEBP magic detected");
        ExternalVector<uint8_t> out;
        CHECK(normalize_image_response(v.data(), v.size(), out) == ImageResponseResult::OK,
              "valid webp normalized");
        CHECK(out.size() == v.size(), "normalized preserves bytes");
    }
    // Not webp.
    {
        const std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0, 0, 0, 0, 0, 0, 0, 0};
        CHECK(!looks_like_webp(png.data(), png.size()), "PNG rejected by magic");
        ExternalVector<uint8_t> out;
        CHECK(normalize_image_response(png.data(), png.size(), out) ==
              ImageResponseResult::NOT_WEBP, "PNG response rejected");
        CHECK(out.empty(), "rejected output left empty");
    }
    // Too short for a RIFF header.
    {
        const std::uint8_t tiny[] = {'R', 'I'};
        CHECK(!looks_like_webp(tiny, 2), "short buffer rejected");
    }
    // Oversize: exactly at the cap is OK; one over is OVERSIZE.
    {
        std::vector<uint8_t> at_cap(max_image_response_bytes(), 0);
        const char riff[] = "RIFF";
        const char webp[] = "WEBP";
        std::memcpy(at_cap.data(), riff, 4);
        std::memcpy(at_cap.data() + 8, webp, 4);
        ExternalVector<uint8_t> out;
        CHECK(normalize_image_response(at_cap.data(), at_cap.size(), out) ==
              ImageResponseResult::OK, "exactly-at-cap webp accepted");
        CHECK(out.size() == max_image_response_bytes(), "at-cap output size");
    }
    {
        std::vector<uint8_t> over = std::vector<uint8_t>(max_image_response_bytes() + 1, 0);
        const char riff[] = "RIFF";
        const char webp[] = "WEBP";
        std::memcpy(over.data(), riff, 4);
        std::memcpy(over.data() + 8, webp, 4);
        ExternalVector<uint8_t> out;
        CHECK(normalize_image_response(over.data(), over.size(), out) ==
              ImageResponseResult::OVERSIZE, "over-cap rejected");
    }
    // Empty / null.
    {
        ExternalVector<uint8_t> out;
        CHECK(normalize_image_response(nullptr, 0, out) == ImageResponseResult::EMPTY,
              "null/zero -> EMPTY");
        const std::uint8_t one = 0;
        CHECK(normalize_image_response(&one, 1, out) == ImageResponseResult::NOT_WEBP,
              "1-byte payload -> NOT_WEBP (non-empty, wrong magic)");
    }
}

static void test_cache_key() {
    // Deterministic, 32 lowercase hex chars, differs by destination and path.
    const std::string hex(32, 'd');
    const auto k1 = image_cache_key(hex, "/media/demo.webp");
    CHECK(k1.size() == 32, "cache key length 32");
    for (const char c : k1) {
        const bool hexp = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        CHECK(hexp, "cache key is lowercase hex");
        if (!hexp) break;
    }
    CHECK(image_cache_key(hex, "/media/demo.webp") == k1, "cache key deterministic");
    CHECK(image_cache_key(hex, "/media/other.webp") != k1, "cache key path-sensitive");
    const std::string hex2(32, 'e');
    CHECK(image_cache_key(hex2, "/media/demo.webp") != k1, "cache key destination-sensitive");
}

int main() {
    test_request_data();
    test_parse_url();
    test_magic_and_normalize();
    test_cache_key();
    if (failures == 0) {
        std::printf("ALL IMAGE WIRE-CONTRACT TESTS PASSED\n");
        return 0;
    }
    std::printf("%d failures\n", failures);
    return 1;
}
