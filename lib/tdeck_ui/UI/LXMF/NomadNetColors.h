#pragma once

#include <cstdint>

#include "NomadNetCompactPage.h"

namespace UI::LXMF::NomadNet {

inline uint32_t heading_foreground(uint8_t depth) {
    switch (heading_display_level(depth)) {
        case 1: return 0x222222;
        case 2: return 0x111111;
        default: return 0x000000;
    }
}

inline uint32_t heading_background(uint8_t depth) {
    switch (heading_display_level(depth)) {
        case 1: return 0xbbbbbb;
        case 2: return 0x999999;
        default: return 0x777777;
    }
}

inline uint32_t resolve_foreground(const CompactPage& page,
                                   const CompactPage::RunRecord& run,
                                   uint32_t fallback) {
    if (run.style & CompactPage::HAS_FOREGROUND) return run.foreground;
    if (page.has_foreground()) return page.foreground();
    return fallback;
}

inline uint32_t resolve_background(const CompactPage& page,
                                    const CompactPage::RunRecord& run,
                                    uint32_t fallback) {
    if (run.style & CompactPage::HAS_BACKGROUND) return run.background;
    if (page.has_background()) return page.background();
    return fallback;
}

inline uint32_t resolve_effective_foreground(const CompactPage& page,
                                              const CompactPage::RunRecord& run,
                                              uint8_t heading_level,
                                              uint32_t fallback) {
    if (run.style & CompactPage::HAS_FOREGROUND) return run.foreground;
    if (heading_level != 0) return heading_foreground(heading_level);
    return resolve_foreground(page, run, fallback);
}

inline uint32_t resolve_effective_background(const CompactPage& page,
                                              const CompactPage::RunRecord& run,
                                              uint8_t heading_level,
                                              uint32_t fallback) {
    if (run.style & CompactPage::HAS_BACKGROUND) return run.background;
    if (heading_level != 0) return heading_background(heading_level);
    return resolve_background(page, run, fallback);
}

// IEC 61966-2-1 sRGB channel values, linearised and scaled to 0..10000.
// A lookup avoids floating-point and pow() work in the LVGL draw path.
static constexpr uint16_t SRGB_LINEAR_10000[256] = {
    0, 3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 37, 40, 44, 48,
    52, 56, 60, 65, 70, 75, 80, 86, 91, 97, 103, 110, 116, 123, 130, 137,
    144, 152, 160, 168, 176, 185, 194, 203, 212, 222, 232, 242, 252, 262, 273, 284,
    296, 307, 319, 331, 343, 356, 369, 382, 395, 409, 423, 437, 452, 467, 482, 497,
    513, 529, 545, 561, 578, 595, 612, 630, 648, 666, 685, 704, 723, 742, 762, 782,
    802, 823, 844, 865, 887, 908, 931, 953, 976, 999, 1022, 1046, 1070, 1095, 1119, 1144,
    1170, 1195, 1221, 1248, 1274, 1301, 1329, 1356, 1384, 1413, 1441, 1470, 1500, 1529, 1559, 1590,
    1620, 1651, 1683, 1714, 1746, 1779, 1812, 1845, 1878, 1912, 1946, 1981, 2016, 2051, 2086, 2122,
    2159, 2195, 2232, 2270, 2307, 2346, 2384, 2423, 2462, 2502, 2542, 2582, 2623, 2664, 2705, 2747,
    2789, 2831, 2874, 2918, 2961, 3005, 3050, 3095, 3140, 3185, 3231, 3278, 3325, 3372, 3419, 3467,
    3515, 3564, 3613, 3663, 3712, 3763, 3813, 3864, 3916, 3968, 4020, 4072, 4125, 4179, 4233, 4287,
    4342, 4397, 4452, 4508, 4564, 4621, 4678, 4735, 4793, 4851, 4910, 4969, 5029, 5089, 5149, 5210,
    5271, 5333, 5395, 5457, 5520, 5583, 5647, 5711, 5776, 5841, 5906, 5972, 6038, 6105, 6172, 6240,
    6308, 6376, 6445, 6514, 6584, 6654, 6724, 6795, 6867, 6939, 7011, 7084, 7157, 7231, 7305, 7379,
    7454, 7529, 7605, 7682, 7758, 7835, 7913, 7991, 8070, 8148, 8228, 8308, 8388, 8469, 8550, 8632,
    8714, 8796, 8879, 8963, 9047, 9131, 9216, 9301, 9387, 9473, 9560, 9647, 9734, 9823, 9911, 10000,
};

inline uint32_t resolve_focus_border(const CompactPage& page,
                                      const CompactPage::RunRecord& run,
                                      uint32_t fallback_background,
                                      uint8_t heading_level = 0) {
    const uint32_t background = resolve_effective_background(
        page, run, heading_level, fallback_background);
    const uint32_t red = SRGB_LINEAR_10000[(background >> 16) & 0xff];
    const uint32_t green = SRGB_LINEAR_10000[(background >> 8) & 0xff];
    const uint32_t blue = SRGB_LINEAR_10000[background & 0xff];
    const uint32_t luminance = 2126 * red + 7152 * green + 722 * blue;
    // Black and white have equal WCAG contrast at relative luminance 0.17913.
    return luminance >= 17913000 ? 0x000000 : 0xffffff;
}

} // namespace UI::LXMF::NomadNet
