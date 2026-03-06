// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#include "TileDownloader.h"

#ifdef ARDUINO

#include "SDAccess.h"
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Log.h>
#include <mbedtls/platform.h>
#include <esp_heap_caps.h>

// ESP-IDF's default mbedtls allocator only uses internal SRAM (~36KB free),
// which is too small for TLS handshake (~40-50KB). Redirect to PSRAM.
// ESP32-S3's unified cache makes PSRAM safe for crypto operations.
static void* psram_calloc(size_t n, size_t size) {
    void* ptr = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = calloc(n, size);  // Fallback to default allocator
    }
    return ptr;
}

static bool _tls_allocator_set = false;

static void ensure_psram_tls() {
    if (_tls_allocator_set) return;
    mbedtls_platform_set_calloc_free(psram_calloc, free);
    _tls_allocator_set = true;
}

namespace Hardware {
namespace TDeck {

String TileDownloader::_url_template = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";

void TileDownloader::set_tile_url(const String& url_template) {
    _url_template = url_template;
}

String TileDownloader::build_url(int z, int x, int y) {
    String url = _url_template;
    url.replace("{z}", String(z));
    url.replace("{x}", String(x));
    url.replace("{y}", String(y));
    return url;
}

String TileDownloader::build_sd_path(int z, int x, int y) {
    char path[64];
    snprintf(path, sizeof(path), "/tiles/%d/%d/%d.png", z, x, y);
    return String(path);
}

bool TileDownloader::tile_exists(int z, int x, int y) {
    String path = build_sd_path(z, x, y);

    if (!SDAccess::acquire_bus(200)) {
        return false;
    }
    bool exists = SD.exists(path);
    SDAccess::release_bus();
    return exists;
}

bool TileDownloader::create_tile_dirs(int z, int x) {
    char dir[48];

    snprintf(dir, sizeof(dir), "/tiles");
    if (!SD.exists(dir)) SD.mkdir(dir);

    snprintf(dir, sizeof(dir), "/tiles/%d", z);
    if (!SD.exists(dir)) SD.mkdir(dir);

    snprintf(dir, sizeof(dir), "/tiles/%d/%d", z, x);
    if (!SD.exists(dir)) SD.mkdir(dir);

    return true;
}

bool TileDownloader::download_tile(int z, int x, int y) {
    char log_buf[128];

    if (WiFi.status() != WL_CONNECTED) {
        WARNING("TileDownloader: WiFi not connected, skipping download");
        return false;
    }

    if (!SDAccess::is_ready()) {
        WARNING("TileDownloader: SD card not ready, skipping download");
        return false;
    }

    String url = build_url(z, x, y);
    String sd_path = build_sd_path(z, x, y);

    snprintf(log_buf, sizeof(log_buf), "TileDownloader: Downloading tile %d/%d/%d", z, x, y);
    INFO(log_buf);

    // Route mbedtls allocations to PSRAM (one-time, persists for all future TLS)
    ensure_psram_tls();

    WiFiClientSecure secureClient;
    secureClient.setInsecure();
    secureClient.setHandshakeTimeout(10);

    HTTPClient http;
    http.setUserAgent("Pyxis/1.0 (ESP32; LXMF messenger)");
    http.setTimeout(10000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(secureClient, url)) {
        WARNING("TileDownloader: Failed to begin HTTP connection");
        return false;
    }

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        snprintf(log_buf, sizeof(log_buf), "TileDownloader: HTTP %d for tile %d/%d/%d", httpCode, z, x, y);
        WARNING(log_buf);
        http.end();
        return false;
    }

    int contentLen = http.getSize();
    if (contentLen <= 0 || contentLen > 100000) {
        snprintf(log_buf, sizeof(log_buf), "TileDownloader: Bad content length %d", contentLen);
        WARNING(log_buf);
        http.end();
        return false;
    }

    // Read response into PSRAM buffer
    uint8_t* buf = (uint8_t*)heap_caps_malloc(contentLen, MALLOC_CAP_SPIRAM);
    if (!buf) {
        WARNING("TileDownloader: PSRAM alloc failed");
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    int bytesRead = 0;
    uint32_t start = millis();
    while (bytesRead < contentLen && (millis() - start) < 10000) {
        int avail = stream->available();
        if (avail > 0) {
            int toRead = min(avail, contentLen - bytesRead);
            int got = stream->read(buf + bytesRead, toRead);
            if (got > 0) bytesRead += got;
        } else {
            delay(10);
        }
    }
    http.end();

    if (bytesRead != contentLen) {
        snprintf(log_buf, sizeof(log_buf), "TileDownloader: Short read %d/%d", bytesRead, contentLen);
        WARNING(log_buf);
        heap_caps_free(buf);
        return false;
    }

    // Write to SD card
    if (!SDAccess::acquire_bus(2000)) {
        heap_caps_free(buf);
        return false;
    }

    create_tile_dirs(z, x);

    File f = SD.open(sd_path, FILE_WRITE);
    if (!f) {
        WARNING("TileDownloader: Failed to open file for write");
        SDAccess::release_bus();
        heap_caps_free(buf);
        return false;
    }

    size_t written = f.write(buf, bytesRead);
    f.close();
    SDAccess::release_bus();
    heap_caps_free(buf);

    if ((int)written != bytesRead) {
        WARNING("TileDownloader: Short write to SD");
        return false;
    }

    snprintf(log_buf, sizeof(log_buf), "TileDownloader: Saved tile %d/%d/%d (%d bytes)", z, x, y, bytesRead);
    INFO(log_buf);
    return true;
}

bool TileDownloader::ensure_tile(int z, int x, int y) {
    if (tile_exists(z, x, y)) {
        return true;
    }
    return download_tile(z, x, y);
}

} // namespace TDeck
} // namespace Hardware

#endif // ARDUINO
