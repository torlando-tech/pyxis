/*
 * DisplayGraphics.h - Bitmap graphics for microReticulum display
 *
 * Contains the μRNS logo and status icons stored in PROGMEM.
 * Bitmaps are in XBM format (1 bit per pixel, LSB first).
 */

#ifndef DISPLAY_GRAPHICS_H
#define DISPLAY_GRAPHICS_H

#include <stdint.h>

#ifdef ARDUINO
    #ifdef ESP32
        #include <pgmspace.h>
    #elif defined(__AVR__)
        #include <avr/pgmspace.h>
    #else
        #define PROGMEM
    #endif
#else
    #define PROGMEM
#endif

namespace RNS {
namespace Graphics {

// μRNS Logo - 40x12 pixels
// Displays "μRNS" text
static const uint8_t LOGO_WIDTH = 40;
static const uint8_t LOGO_HEIGHT = 12;
static const uint8_t logo_urns[] PROGMEM = {
    // Row 0
    0x00, 0x00, 0x00, 0x00, 0x00,
    // Row 1 - top of letters
    0x00, 0x00, 0xFC, 0x0F, 0x00,
    // Row 2
    0x66, 0x1E, 0x86, 0x19, 0x3E,
    // Row 3
    0x66, 0x33, 0x83, 0x31, 0x33,
    // Row 4
    0x66, 0x33, 0x83, 0x31, 0x03,
    // Row 5
    0x66, 0x3F, 0x83, 0x31, 0x1E,
    // Row 6
    0x66, 0x33, 0x83, 0x31, 0x30,
    // Row 7
    0x66, 0x33, 0x83, 0x31, 0x30,
    // Row 8
    0x3C, 0x33, 0x86, 0x19, 0x33,
    // Row 9
    0x18, 0x33, 0xFC, 0x0F, 0x1E,
    // Row 10
    0x18, 0x00, 0x00, 0x00, 0x00,
    // Row 11
    0x00, 0x00, 0x00, 0x00, 0x00,
};

// Alternate larger logo - 48x16 pixels
// More visible "μRNS" with better spacing
static const uint8_t LOGO_LARGE_WIDTH = 48;
static const uint8_t LOGO_LARGE_HEIGHT = 16;
static const uint8_t logo_urns_large[] PROGMEM = {
    // Row 0
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // Row 1
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // Row 2 - μ starts, R N S top
    0x00, 0x00, 0xF8, 0x1F, 0x00, 0x00,
    // Row 3
    0xC6, 0x38, 0x0C, 0x30, 0xF8, 0x00,
    // Row 4
    0xC6, 0x6C, 0x06, 0x60, 0xCC, 0x00,
    // Row 5
    0xC6, 0x6C, 0x06, 0x60, 0x0C, 0x00,
    // Row 6
    0xC6, 0x7C, 0x06, 0x60, 0x0C, 0x00,
    // Row 7
    0xC6, 0x6C, 0x06, 0x60, 0x78, 0x00,
    // Row 8
    0xC6, 0x6C, 0x06, 0x60, 0xC0, 0x00,
    // Row 9
    0xC6, 0x6C, 0x06, 0x60, 0xC0, 0x00,
    // Row 10
    0x7C, 0x6C, 0x0C, 0x30, 0xCC, 0x00,
    // Row 11
    0x38, 0x6C, 0xF8, 0x1F, 0x78, 0x00,
    // Row 12
    0x30, 0x00, 0x00, 0x00, 0x00, 0x00,
    // Row 13
    0x30, 0x00, 0x00, 0x00, 0x00, 0x00,
    // Row 14
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // Row 15
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// Signal strength bars - 12x8 pixels each
// 5 states: 0 bars (offline), 1-4 bars (signal strength)
static const uint8_t SIGNAL_WIDTH = 12;
static const uint8_t SIGNAL_HEIGHT = 8;

// Signal bars - 0 (offline/no signal)
static const uint8_t signal_0[] PROGMEM = {
    0x00, 0x00,  // Row 0
    0x00, 0x00,  // Row 1
    0x00, 0x00,  // Row 2
    0x00, 0x00,  // Row 3
    0x00, 0x00,  // Row 4
    0x00, 0x00,  // Row 5
    0x00, 0x00,  // Row 6
    0x00, 0x00,  // Row 7
};

// Signal bars - 1 bar (weak)
static const uint8_t signal_1[] PROGMEM = {
    0x00, 0x00,  // Row 0
    0x00, 0x00,  // Row 1
    0x00, 0x00,  // Row 2
    0x00, 0x00,  // Row 3
    0x00, 0x00,  // Row 4
    0x00, 0x00,  // Row 5
    0x03, 0x00,  // Row 6 - 1 bar
    0x03, 0x00,  // Row 7
};

// Signal bars - 2 bars (fair)
static const uint8_t signal_2[] PROGMEM = {
    0x00, 0x00,  // Row 0
    0x00, 0x00,  // Row 1
    0x00, 0x00,  // Row 2
    0x00, 0x00,  // Row 3
    0x0C, 0x00,  // Row 4 - 2nd bar
    0x0C, 0x00,  // Row 5
    0x0F, 0x00,  // Row 6 - both bars
    0x0F, 0x00,  // Row 7
};

// Signal bars - 3 bars (good)
static const uint8_t signal_3[] PROGMEM = {
    0x00, 0x00,  // Row 0
    0x00, 0x00,  // Row 1
    0x30, 0x00,  // Row 2 - 3rd bar
    0x30, 0x00,  // Row 3
    0x3C, 0x00,  // Row 4 - bars 2+3
    0x3C, 0x00,  // Row 5
    0x3F, 0x00,  // Row 6 - all 3 bars
    0x3F, 0x00,  // Row 7
};

// Signal bars - 4 bars (excellent)
static const uint8_t signal_4[] PROGMEM = {
    0xC0, 0x00,  // Row 0 - 4th bar
    0xC0, 0x00,  // Row 1
    0xF0, 0x00,  // Row 2 - bars 3+4
    0xF0, 0x00,  // Row 3
    0xFC, 0x00,  // Row 4 - bars 2+3+4
    0xFC, 0x00,  // Row 5
    0xFF, 0x00,  // Row 6 - all 4 bars
    0xFF, 0x00,  // Row 7
};

// Online indicator - filled circle 8x8
static const uint8_t INDICATOR_SIZE = 8;
static const uint8_t indicator_online[] PROGMEM = {
    0x3C,  // ..####..
    0x7E,  // .######.
    0xFF,  // ########
    0xFF,  // ########
    0xFF,  // ########
    0xFF,  // ########
    0x7E,  // .######.
    0x3C,  // ..####..
};

// Offline indicator - empty circle 8x8
static const uint8_t indicator_offline[] PROGMEM = {
    0x3C,  // ..####..
    0x42,  // .#....#.
    0x81,  // #......#
    0x81,  // #......#
    0x81,  // #......#
    0x81,  // #......#
    0x42,  // .#....#.
    0x3C,  // ..####..
};

// Link icon - two connected nodes 12x8
static const uint8_t LINK_ICON_WIDTH = 12;
static const uint8_t LINK_ICON_HEIGHT = 8;
static const uint8_t icon_link[] PROGMEM = {
    0x1E, 0x07,  // Row 0
    0x21, 0x08,  // Row 1
    0x21, 0x08,  // Row 2
    0xE1, 0x08,  // Row 3 - connection line
    0xE1, 0x08,  // Row 4 - connection line
    0x21, 0x08,  // Row 5
    0x21, 0x08,  // Row 6
    0x1E, 0x07,  // Row 7
};

// Helper to get signal bitmap based on level (0-4)
inline const uint8_t* get_signal_bitmap(uint8_t level) {
    switch(level) {
        case 1: return signal_1;
        case 2: return signal_2;
        case 3: return signal_3;
        case 4: return signal_4;
        default: return signal_0;
    }
}

// Convert RSSI (dBm) to signal level (0-4)
// Typical LoRa RSSI ranges: -120 dBm (weak) to -30 dBm (strong)
inline uint8_t rssi_to_level(float rssi) {
    if (rssi >= -60) return 4;      // Excellent
    if (rssi >= -80) return 3;      // Good
    if (rssi >= -100) return 2;     // Fair
    if (rssi >= -120) return 1;     // Weak
    return 0;                        // No signal / offline
}

} // namespace Graphics
} // namespace RNS

#endif // DISPLAY_GRAPHICS_H
