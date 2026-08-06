#pragma once

#include <stdbool.h>
#include <stdint.h>

// Exact codepoint contract shared by the generated LVGL font adapters and the
// host-testable UTF-8 display sanitizer. Keeping this separate prevents LVGL's
// inclusive range-length check from treating the first codepoint after a
// contiguous cmap as a valid (and potentially out-of-bounds) glyph.
static inline bool nomadnet_font_has_codepoint(uint32_t codepoint) {
    if (codepoint >= 0x20 && codepoint <= 0x7e) return true;
    if (codepoint >= 0x00a0 && codepoint <= 0x017f) return true;
    if (codepoint >= 0x2190 && codepoint <= 0x2199) return true;

    switch (codepoint) {
        case 0x2007: case 0x2008: case 0x2009: case 0x200a: case 0x200b:
        case 0x2010: case 0x2012: case 0x2013: case 0x2014: case 0x2015:
        case 0x2018: case 0x2019: case 0x201a: case 0x201c: case 0x201d:
        case 0x201e: case 0x2020: case 0x2021: case 0x2022: case 0x2026:
        case 0x2030: case 0x2032: case 0x2033: case 0x2039: case 0x203a:
        case 0x2044: case 0x2052:
        case 0x20a1: case 0x20a3: case 0x20a4: case 0x20a6: case 0x20a7:
        case 0x20a9: case 0x20ab: case 0x20ac: case 0x20ad: case 0x20ae:
        case 0x20b1: case 0x20b2: case 0x20b4: case 0x20b5: case 0x20b8:
        case 0x20b9: case 0x20ba: case 0x20bc: case 0x20bd:
        case 0x2500: case 0x2550: case 0x2551: case 0x2554: case 0x2557:
        case 0x255a: case 0x255d: case 0x2588: case 0x2594: case 0x25a0:
            return true;
        default:
            return false;
    }
}
