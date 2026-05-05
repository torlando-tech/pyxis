// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef HARDWARE_TDECK_TILEDOWNLOADER_H
#define HARDWARE_TDECK_TILEDOWNLOADER_H

#ifdef ARDUINO
#include <Arduino.h>

namespace Hardware {
namespace TDeck {

/**
 * Downloads OSM raster tiles over WiFi and caches them on SD card.
 *
 * Tile URL template uses {z}/{x}/{y} substitution, e.g.:
 *   "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
 *
 * Downloaded tiles are saved to SD at: /tiles/{z}/{x}/{y}.png
 * Requires WiFi to be connected and SD card to be ready.
 */
class TileDownloader {
public:
    /**
     * Set the tile server URL template.
     * Must contain {z}, {x}, {y} placeholders.
     * Default: "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
     */
    static void set_tile_url(const String& url_template);

    /**
     * Check if a tile exists on SD card.
     */
    static bool tile_exists(int z, int x, int y);

    /**
     * Download a tile and save to SD card.
     * Creates directory structure as needed.
     * @return true if tile was downloaded and saved successfully
     */
    static bool download_tile(int z, int x, int y);

    /**
     * Try to ensure a tile is available on SD.
     * Returns immediately if already cached, downloads if not.
     * @return true if tile is available on SD after this call
     */
    static bool ensure_tile(int z, int x, int y);

private:
    static String _url_template;

    static bool create_tile_dirs(int z, int x);
    static String build_url(int z, int x, int y);
    static String build_sd_path(int z, int x, int y);
};

} // namespace TDeck
} // namespace Hardware

#endif // ARDUINO
#endif // HARDWARE_TDECK_TILEDOWNLOADER_H
