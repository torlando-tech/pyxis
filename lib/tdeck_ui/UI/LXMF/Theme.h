#pragma once

#include <lvgl.h>

namespace UI::LXMF::Theme {

// Primary accent color (Columba Vibrant theme)
constexpr uint32_t PRIMARY = 0xB844C8;         // Vibrant magenta-purple (buttons, sent bubbles)
constexpr uint32_t PRIMARY_PRESSED = 0x9C27B0; // Darker purple when pressed
constexpr uint32_t PRIMARY_LIGHT = 0xE8B4F0;   // Light lavender (Purple80 from Vibrant)

// Surface colors (purple-tinted like Vibrant theme)
constexpr uint32_t SURFACE = 0x1D1A1E;            // Very dark purple-gray background
constexpr uint32_t SURFACE_HEADER = 0x2D2832;     // Dark purple-gray header
constexpr uint32_t SURFACE_CONTAINER = 0x3D3344;  // Purple-tinted list items
constexpr uint32_t SURFACE_ELEVATED = 0x4A454F;   // VibrantSurface80 - pressed/focused
constexpr uint32_t SURFACE_INPUT = 0x332D41;      // Dark mauve input fields

// Button colors (purple-tinted)
constexpr uint32_t BTN_SECONDARY = 0x3D3344;      // Purple-gray button
constexpr uint32_t BTN_SECONDARY_PRESSED = 0x4F2B54;  // VibrantContainer80

// Border colors
constexpr uint32_t BORDER = 0x4A454F;             // Purple-gray border
constexpr uint32_t BORDER_FOCUS = PRIMARY_LIGHT;  // Light lavender focus

// Text colors (Vibrant theme palette)
constexpr uint32_t TEXT_PRIMARY = 0xE8B4F0;       // Light lavender (Purple80)
constexpr uint32_t TEXT_SECONDARY = 0xD4C0DC;     // PurpleGrey80
constexpr uint32_t TEXT_TERTIARY = 0xADA6B0;      // VibrantOutline80
constexpr uint32_t TEXT_MUTED = 0x7A7580;         // Muted purple-gray

// Status colors
constexpr uint32_t SUCCESS = 0x4CAF50;         // Green
constexpr uint32_t SUCCESS_DARK = 0x2e7d32;    // Dark green (buttons)
constexpr uint32_t SUCCESS_PRESSED = 0x388e3c;
constexpr uint32_t WARNING = 0xFFEB3B;         // Yellow
constexpr uint32_t ERROR = 0xF44336;           // Red
constexpr uint32_t INFO = 0xE8B4F0;            // Light lavender (matches Vibrant)
constexpr uint32_t CHARGING = 0xFFB3C6;        // Pink80 (coral-pink from Vibrant)
constexpr uint32_t BLUETOOTH = 0x2196F3;       // Blue (keep Bluetooth icon blue)

// Convenience functions
inline lv_color_t primary() { return lv_color_hex(PRIMARY); }
inline lv_color_t primaryPressed() { return lv_color_hex(PRIMARY_PRESSED); }
inline lv_color_t primaryLight() { return lv_color_hex(PRIMARY_LIGHT); }
inline lv_color_t surface() { return lv_color_hex(SURFACE); }
inline lv_color_t surfaceHeader() { return lv_color_hex(SURFACE_HEADER); }
inline lv_color_t surfaceContainer() { return lv_color_hex(SURFACE_CONTAINER); }
inline lv_color_t surfaceElevated() { return lv_color_hex(SURFACE_ELEVATED); }
inline lv_color_t surfaceInput() { return lv_color_hex(SURFACE_INPUT); }
inline lv_color_t btnSecondary() { return lv_color_hex(BTN_SECONDARY); }
inline lv_color_t btnSecondaryPressed() { return lv_color_hex(BTN_SECONDARY_PRESSED); }
inline lv_color_t border() { return lv_color_hex(BORDER); }
inline lv_color_t borderFocus() { return lv_color_hex(BORDER_FOCUS); }
inline lv_color_t textPrimary() { return lv_color_hex(TEXT_PRIMARY); }
inline lv_color_t textSecondary() { return lv_color_hex(TEXT_SECONDARY); }
inline lv_color_t textTertiary() { return lv_color_hex(TEXT_TERTIARY); }
inline lv_color_t textMuted() { return lv_color_hex(TEXT_MUTED); }
inline lv_color_t success() { return lv_color_hex(SUCCESS); }
inline lv_color_t successDark() { return lv_color_hex(SUCCESS_DARK); }
inline lv_color_t successPressed() { return lv_color_hex(SUCCESS_PRESSED); }
inline lv_color_t warning() { return lv_color_hex(WARNING); }
inline lv_color_t error() { return lv_color_hex(ERROR); }
inline lv_color_t info() { return lv_color_hex(INFO); }
inline lv_color_t charging() { return lv_color_hex(CHARGING); }
inline lv_color_t bluetooth() { return lv_color_hex(BLUETOOTH); }

} // namespace UI::LXMF::Theme
