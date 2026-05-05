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

namespace {

constexpr int kMaxTileBytes = 100000;

// GlobalSign root bundle for tile.openstreetmap.org.
// Source: GlobalSign official root certificate page.
// - Root R3: current Atlas R3 DV TLS chain observed for tile.openstreetmap.org
// - Root R46: announced RSA successor for newer Atlas TLS rotations
constexpr const char kTileServerRootCerts[] =
R"CERT(-----BEGIN CERTIFICATE-----
MIIDXzCCAkegAwIBAgILBAAAAAABIVhTCKIwDQYJKoZIhvcNAQELBQAwTDEgMB4
GA1UECxMXR2xvYmFsU2lnbiBSb290IENBIC0gUjMxEzARBgNVBAoTCkdsb2JhbF
NpZ24xEzARBgNVBAMTCkdsb2JhbFNpZ24wHhcNMDkwMzE4MTAwMDAwWhcNMjkwM
zE4MTAwMDAwWjBMMSAwHgYDVQQLExdHbG9iYWxTaWduIFJvb3QgQ0EgLSBSMzET
MBEGA1UEChMKR2xvYmFsU2lnbjETMBEGA1UEAxMKR2xvYmFsU2lnbjCCASIwDQY
JKoZIhvcNAQEBBQADggEPADCCAQoCggEBAMwldpB5BngiFvXAg7aEyiie/QV2Ec
WtiHL8RgJDx7KKnQRfJMsuS+FggkbhUqsMgUdwbN1k0ev1LKMPgj0MK66X17YUh
hB5uzsTgHeMCOFJ0mpiLx9e+pZo34knlTifBtc+ycsmWQ1z3rDI6SYOgxXG71uL
0gRgykmmKPZpO/bLyCiR5Z2KYVc3rHQU3HTgOu5yLy6c+9C7v/U9AOEGM+iCK65
TpjoWc4zdQQ4gOsC0p6Hpsk+QLjJg6VfLuQSSaGjlOCZgdbKfd/+RFO+uIEn8rU
AVSNECMWEZXriX7613t2Saer9fwRPvm2L7DWzgVGkWqQPabumDk3F2xmmFghcCA
wEAAaNCMEAwDgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0O
BBYEFI/wS3+oLkUkrk1Q+mOai97i3Ru8MA0GCSqGSIb3DQEBCwUAA4IBAQBLQNv
AUKr+yAzv95ZURUm7lgAJQayzE4aGKAczymvmdLm6AC2upArT9fHxD4q/c2dKg8
dEe3jgr25sbwMpjjM5RcOO5LlXbKr8EpbsU8Yt5CRsuZRj+9xTaGdWPoO4zzUhw
8lo/s7awlOqzJCK6fBdRoyV3XpYKBovHd7NADdBj+1EbddTKJd+82cEHhXXipa0
095MJ6RMG3NzdvQXmcIfeg7jLQitChws/zyrVQ4PkX4268NXSb7hLi18YIvDQVE
TI53O9zJrlAGomecsMx86OyXShkDOOyyGeMlhLxS67ttVb9+E7gUJTb0o2HLO02
JQZR7rkpeDMdmztcpHWD9f
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIFWjCCA0KgAwIBAgISEdK7udcjGJ5AXwqdLdDfJWfRMA0GCSqGSIb3DQEBDAU
AMEYxCzAJBgNVBAYTAkJFMRkwFwYDVQQKExBHbG9iYWxTaWduIG52LXNhMRwwGg
YDVQQDExNHbG9iYWxTaWduIFJvb3QgUjQ2MB4XDTE5MDMyMDAwMDAwMFoXDTQ2M
DMyMDAwMDAwMFowRjELMAkGA1UEBhMCQkUxGTAXBgNVBAoTEEdsb2JhbFNpZ24g
bnYtc2ExHDAaBgNVBAMTE0dsb2JhbFNpZ24gUm9vdCBSNDYwggIiMA0GCSqGSIb
3DQEBAQUAA4ICDwAwggIKAoICAQCsrHQy6LNl5brtQyYdpokNRbopiLKkHWPd08
EsCVeJOaFV6Wc0dwxu5FUdUiXSE2te4R2pt32JMl8Nnp8semNgQB+msLZ4j5lUl
ghYruQGvGIFAha/r6gjA7aUD7xubMLL1aa7DOn2wQL7Id5m3RerdELv8HQvJfTq
a1VbkNud316HCkD7rRlr+/fKYIje2sGP1q7Vf9Q8g+7XFkyDRTNrJ9CG0Bwta/O
rffGFqfUo0q3v84RLHIf8E6M6cqJaESvWJ3En7YEtbWaBkoe0G1h6zD8K+kZPT
Xhc+CtI4wSEy132tGqzZfxCnlEmIyDLPRT5ge1lFgBPGmSXZgjPjHvjK8Cd+RTy
G/FWaha/LIWFzXg4mutCagI0GIMXTpRW+LaCtfOW3T3zvn8gdz57GSNrLNRyc0N
XfeD412lPFzYE+cCQYDdF3uYM2HSNrpyibXRdQr4G9dlkbgIQrImwTDsHTUB+JM
WKmIJ5jqSngiCNI/onccnfxkF0oE32kRbcRoxfKWMxWXEM2G/CtjJ9++ZdU6Z+F
fy7dXxd7Pj2Fxzsx2sZy/N78CsHpdlseVR2bJ0cpm4O6XkMqCNqo98bMDGfsVR7
/mrLZqrcZdCinkqaByFrgY/bxFn63iLABJzjqls2k+g9vXqhnQt2sQvHnf3PmKg
Gwvgqo6GDoLclcqUC4wIDAQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQ
H/BAUwAwEB/zAdBgNVHQ4EFgQUA1yrc4GHqMywptWU4jaWSf8FmSwwDQYJKoZIh
vcNAQEMBQADggIBAHx47PYCLLtbfpIrXTncvtgdokIzTfnvpCo7RGkerNlFo048
p9gkUbJUHJNOxO97k4VgJuoJSOD1u8fpaNK7ajFxzHmuEajwmf3lH7wvqMxX63b
EIaZHU1VNaL8FpO7XJqti2kM3S+LGteWygxk6x9PbTZ4IevPuzz5i+6zoYMzRx6
Fcg0XERczzF2sUyQQCPtIkpnnpHs6i58FZFZ8d4kuaPp92CC1r2LpXFNqD6v6MV
enQTqnMdzGxRBF6XLE+0xRFFRhiJBPSy03OXIPBNvIQtQ6IbbjhVp+J3pZmOUdk
LG5NrmJ7v2B0GbhWrJKsFjLtrWhV/pi60zTe9Mlhww6G9kuEYO4Ne7UyWHmRVSy
BQ7N0H3qqJZ4d16GLuc1CLgSkZoNNiTW2bKg2SnkheCLQQrzRQDGQob4Ez8pn7f
XwgNNgyYMqIgXQBztSvwyeqiv5u+YfjyW6hY0XHgL+XVAEV8/+LbzvXMAaq7afJ
Mbfc2hIkCwU9D9SGuTSyxTDYWnP4vkYxboznxSjBF25cfe1lNj2M8FawTSLfJvd
kzrnE6JwYZ+vj+vYxXX4M2bUdGc6N3ec592kD3ZDZopD8p/7DEJ4Y9HiD2971KE
9dJeFt0g5QdYg/NA6s/rob8SKunE3vouXsXgxT7PntgMTzlSdriVZzH81Xwj3QE
UxeCp6
-----END CERTIFICATE-----
)CERT";

}  // namespace

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
    secureClient.setCACert(kTileServerRootCerts);
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
    const bool has_known_length = contentLen >= 0;
    if (has_known_length && (contentLen == 0 || contentLen > kMaxTileBytes)) {
        snprintf(log_buf, sizeof(log_buf), "TileDownloader: Bad content length %d", contentLen);
        WARNING(log_buf);
        http.end();
        return false;
    }

    const int buffer_capacity = has_known_length ? contentLen : kMaxTileBytes;

    // Read response into PSRAM buffer.
    // For chunked responses with unknown length, cap the download to 100 KB.
    uint8_t* buf = (uint8_t*)heap_caps_malloc(buffer_capacity, MALLOC_CAP_SPIRAM);
    if (!buf) {
        WARNING("TileDownloader: PSRAM alloc failed");
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    int bytesRead = 0;
    uint32_t start = millis();
    bool overflow = false;
    while ((millis() - start) < 10000) {
        int avail = stream->available();
        if (avail > 0) {
            int remaining = buffer_capacity - bytesRead;
            if (remaining <= 0) {
                overflow = true;
                break;
            }
            int toRead = min(avail, remaining);
            int got = stream->read(buf + bytesRead, toRead);
            if (got > 0) {
                bytesRead += got;
                continue;
            }
        } else if (!stream->connected()) {
            break;
        } else {
            delay(10);
        }
    }
    http.end();

    if (overflow) {
        snprintf(log_buf, sizeof(log_buf), "TileDownloader: Tile exceeded %d byte cap", kMaxTileBytes);
        WARNING(log_buf);
        heap_caps_free(buf);
        return false;
    }

    if (has_known_length && bytesRead != contentLen) {
        snprintf(log_buf, sizeof(log_buf), "TileDownloader: Short read %d/%d", bytesRead, contentLen);
        WARNING(log_buf);
        heap_caps_free(buf);
        return false;
    }

    if (!has_known_length && bytesRead <= 0) {
        WARNING("TileDownloader: Empty chunked response");
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
