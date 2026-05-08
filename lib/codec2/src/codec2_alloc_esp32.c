/*
 * codec2_malloc / codec2_free wrappers for ESP32.
 *
 * Codec2 v1.2 expects these to be provided by the consuming firmware
 * when __EMBEDDED__ is defined. The __EMBEDDED__ flag is what gates
 * the codebook .c files into `static const float[]` (PROGMEM/flash)
 * rather than RAM-resident `static float[]` — without it codec2's
 * codebooks add ~127KB to BSS and the LVGL task fails to start.
 *
 * The wrappers themselves are trivial: ESP-IDF malloc/free already
 * pull from internal RAM by default, which is what codec2 needs for
 * its working state (FFT plans, LSP buffers, etc — not the static
 * codebooks).
 */

#include <stdlib.h>
#include <stddef.h>

void *codec2_malloc(size_t size) {
    return malloc(size);
}

void *codec2_calloc(size_t nmemb, size_t size) {
    return calloc(nmemb, size);
}

void codec2_free(void *ptr) {
    free(ptr);
}
