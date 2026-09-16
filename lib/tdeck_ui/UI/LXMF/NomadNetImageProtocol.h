// NomadNet page-image wire contract (upstream e1e8ab8 / v1.4.3).
//
// Reference facts (read from the authoritative RNS-synced source):
//  - Browser.__get_image_request_data: request data is exactly
//    {"path": <parsed path>, "key": None} on the registered path "/media".
//    The "key" field must be an EXPLICIT MessagePack nil (0xc0); a missing
//    field or an empty span makes the reference server (Node.serve_media)
//    reject the request.
//  - Browser.parse_url: image urls keep the leading ":" in the parsed
//    document record. A leading empty component means the image lives on
//    the CURRENT page destination (same-destination reuse of the page's
//    Link); a 32-hex component names a remote destination. serve_media
//    jails the path to the page node's pages path, so in practice page
//    images are same-destination.
//  - Node.serve_media converts every non-WebP on-disk format to WebP
//    before serving, so the wire bytes are always WebP (RIFF....WEBP).
//  - Browser.image_cache_path: disk cache key = hash of
//    "{hexrep(destination_hash)}:{path}".
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "NomadNetMemory.h"

namespace UI::LXMF::NomadNet {

// Registered Reticulum request path for page media.
inline const char* MEDIA_PATH() { return "/media"; }

// Hard cap on fetched image bytes before decode. The decoder bounds the
// result to 640x640 RGB565 anyway, so this only protects transport and the
// PSRAM staging allocation. Upstream has no client-side per-file cap (its
// node clamps conversion to 1200 px); 512 KiB of WebP comfortably covers
// anything worth showing on a 320x240 display.
inline std::size_t max_image_response_bytes() {
    return 512 * 1024;
}

struct ImageUrl {
    bool valid = false;
    bool same_destination = false;
    std::string destination_hex; // 32 lowercase hex chars, or empty
    std::string path;            // e.g. "/media/demo.webp"
};

// Parse an image url exactly like the reference Browser.parse_url: split on
// ":". One component: 32-hex destination, default page path. Two components:
// either a 32-hex destination + path, or an empty first component (the
// parsed record keeps the leading ":") meaning the current page
// destination. Anything else is malformed.
inline ImageUrl parse_image_url(const std::string& url) {
    ImageUrl result;
    const std::size_t colon = url.find(':');
    const std::string first = colon == std::string::npos ? url : url.substr(0, colon);
    if (colon == std::string::npos) {
        if (first.size() == 32) {
            result.valid = true;
            result.destination_hex = first;
            result.path = "/";
        }
        return result;
    }
    const std::string rest = url.substr(colon + 1);
    if (first.empty()) {
        // ":/media/x.webp" -> same destination as the current page.
        if (rest.empty()) return result;
        result.valid = true;
        result.same_destination = true;
        result.path = rest;
        return result;
    }
    if (first.size() == 32) {
        result.valid = true;
        result.destination_hex = first;
        result.path = rest.empty() ? std::string("/") : rest;
    }
    return result;
}

// Encode the exact reference request-data map:
//   fixmap(2) str("path") <path bytes> str("key") nil
// The trailing 0xc0 is protocol-level nil, matching RNS's
// data={"path": path, "key": None}. The path uses the same string encoding
// widths as the form encoder (fixstr, then str8/str16 for longer values).
inline std::vector<uint8_t> media_request_data(const std::string& path) {
    std::vector<uint8_t> output;
    output.reserve(path.size() + 16);
    output.push_back(0x82); // fixmap, 2 elements
    // First key: "path"
    output.push_back(0xa4);
    output.insert(output.end(), {'p', 'a', 't', 'h'});
    // First value: the path string.
    if (path.size() <= 31) {
        output.push_back(static_cast<uint8_t>(0xa0 | path.size()));
    } else if (path.size() <= 0xff) {
        output.push_back(0xd9);
        output.push_back(static_cast<uint8_t>(path.size()));
    } else {
        output.push_back(0xda);
        output.push_back(static_cast<uint8_t>(path.size() >> 8));
        output.push_back(static_cast<uint8_t>(path.size()));
    }
    output.insert(output.end(), path.begin(), path.end());
    // Second key: "key"
    output.push_back(0xa3);
    output.insert(output.end(), {'k', 'e', 'y'});
    output.push_back(0xc0); // explicit nil (value for "key")
    return output;
}

// WebP RIFF magic: bytes 0-3 "RIFF", 8-11 "WEBP".
inline bool looks_like_webp(const std::uint8_t* data, std::size_t size) {
    if (size < 12) return false;
    return data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
           data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P';
}

enum class ImageResponseResult {
    OK,
    EMPTY,
    OVERSIZE,
    NOT_WEBP,
    ALLOCATION_FAILED,
};

// Bound and validate a fetched page-image response. Packet responses arrive
// as raw WebP bytes; Resource responses are already file bytes at the same
// level, so both take the identical path here (no MessagePack bin envelope:
// /media is not a value-carrying response).
inline ImageResponseResult normalize_image_response(
        const std::uint8_t* data, std::size_t size,
        ExternalVector<std::uint8_t>& output, std::size_t max_bytes = 0) {
    if (max_bytes == 0) max_bytes = max_image_response_bytes();
    if (data == nullptr || size == 0) return ImageResponseResult::EMPTY;
    if (size > max_bytes) return ImageResponseResult::OVERSIZE;
    if (!looks_like_webp(data, size)) return ImageResponseResult::NOT_WEBP;
    output.clear();
    try {
        output.assign(data, data + size);
    } catch (const std::bad_alloc&) {
        output.clear();
        return ImageResponseResult::ALLOCATION_FAILED;
    }
    return ImageResponseResult::OK;
}

// Cache key for a fetched image, mirroring the reference:
// digest of "{destination hex}:{path}" using the same FNV-1a double hash the
// page cache uses (32 lowercase hex chars). The destination hex is the
// resolved destination (page destination for same-destination urls).
inline std::string image_cache_key(const std::string& destination_hex, const std::string& path) {
    // Identical to NomadNetCache::hash (NomadNetCache.cpp); kept inline so
    // the image protocol stays free of the SD-cache dependency for host
    // tests. If the cache hash ever changes, this must change with it.
    auto fnv = [](const std::uint8_t* data, std::size_t size, std::uint64_t seed) {
        std::uint64_t value = seed;
        for (std::size_t i = 0; i < size; ++i) {
            value ^= data[i];
            value *= 1099511628211ULL;
        }
        return value;
    };
    const std::string canonical = destination_hex + ":" + path;
    const auto first = fnv(reinterpret_cast<const std::uint8_t*>(canonical.data()),
                           canonical.size(), 1469598103934665603ULL);
    const auto second = fnv(reinterpret_cast<const std::uint8_t*>(canonical.data()),
                            canonical.size(), 1099511628211ULL);
    char output[33] = {};
    std::snprintf(output, sizeof(output), "%016llx%016llx",
                  static_cast<unsigned long long>(first),
                  static_cast<unsigned long long>(second));
    return output;
}

} // namespace UI::LXMF::NomadNet
