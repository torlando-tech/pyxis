/*
 * Display.h - OLED display support for microReticulum
 *
 * Provides status display on supported hardware (T-Beam Supreme, etc.)
 * with auto-rotating pages showing identity, interface, and network info.
 */

#pragma once

#include "Type.h"
#include "Bytes.h"

#include <stdint.h>
#include <string>

// Only compile display code if enabled
#ifdef HAS_DISPLAY

namespace RNS {

// Forward declarations
class Identity;
class Interface;
class Reticulum;

class Display {
public:
    // Number of pages to rotate through
    static const uint8_t NUM_PAGES = 3;

    // Page interval in milliseconds
    static const uint32_t PAGE_INTERVAL = 4000;

    // Display update interval (~7 FPS)
    static const uint32_t UPDATE_INTERVAL = 143;

public:
    /**
     * Initialize the display hardware.
     * Must be called once during setup.
     * @return true if display initialized successfully
     */
    static bool init();

    /**
     * Update the display. Call this frequently in the main loop.
     * Handles page rotation and display refresh internally.
     */
    static void update();

    /**
     * Set the identity to display.
     * @param identity The identity whose hash will be shown
     */
    static void set_identity(const Identity& identity);

    /**
     * Set the primary interface to display status for.
     * @param iface Pointer to the interface (can be nullptr)
     */
    static void set_interface(Interface* iface);

    /**
     * Set the Reticulum instance for network statistics.
     * @param rns Pointer to the Reticulum instance
     */
    static void set_reticulum(Reticulum* rns);

    /**
     * Enable or disable display blanking (power save).
     * @param blank true to blank the display
     */
    static void blank(bool blank);

    /**
     * Set the current page manually.
     * @param page Page number (0 to NUM_PAGES-1)
     */
    static void set_page(uint8_t page);

    /**
     * Advance to the next page.
     */
    static void next_page();

    /**
     * Get the current page number.
     * @return Current page (0 to NUM_PAGES-1)
     */
    static uint8_t current_page();

    /**
     * Check if display is ready.
     * @return true if display was initialized successfully
     */
    static bool ready();

    /**
     * Set RSSI value for signal bars display.
     * @param rssi Signal strength in dBm
     */
    static void set_rssi(float rssi);

private:
    // Page rendering functions
    static void draw_page_main();       // Page 0: Main status
    static void draw_page_interface();  // Page 1: Interface details
    static void draw_page_network();    // Page 2: Network info

    // Common elements
    static void draw_header();          // Logo + signal bars (all pages)
    static void draw_signal_bars(int16_t x, int16_t y);

    // Helper functions
    static std::string format_bytes(size_t bytes);
    static std::string format_time(uint32_t seconds);
    static std::string format_bitrate(uint32_t bps);

private:
    // State
    static bool _ready;
    static bool _blanked;
    static uint8_t _current_page;
    static uint32_t _last_page_flip;
    static uint32_t _last_update;
    static uint32_t _start_time;

    // Data sources
    static Bytes _identity_hash;
    static Interface* _interface;
    static Reticulum* _reticulum;

    // Signal strength
    static float _rssi;
};

} // namespace RNS

#endif // HAS_DISPLAY
