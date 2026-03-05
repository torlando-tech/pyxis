// Copyright (c) 2024 microReticulum contributors
// SPDX-License-Identifier: MIT

#ifndef UI_LXMF_TILEMATH_H
#define UI_LXMF_TILEMATH_H

#include <cmath>
#include <stdint.h>

namespace UI {
namespace LXMF {

/**
 * Web Mercator tile coordinate math.
 * Standard OSM/Slippy map tile numbering (z/x/y).
 */
namespace TileMath {

    struct TileCoord {
        int tile_x;
        int tile_y;
        int pixel_x;  // 0-255, pixel offset within tile
        int pixel_y;
    };

    /**
     * Convert lat/lon to tile coordinates at a given zoom level.
     * Returns the tile x/y and the pixel offset within that tile.
     */
    inline TileCoord latlon_to_tile(double lat, double lon, int z) {
        double n = (double)(1 << z);
        double x = (lon + 180.0) / 360.0 * n;
        double lat_rad = lat * M_PI / 180.0;
        double y = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * n;

        TileCoord tc;
        tc.tile_x = (int)floor(x);
        tc.tile_y = (int)floor(y);
        tc.pixel_x = (int)((x - tc.tile_x) * 256.0);
        tc.pixel_y = (int)((y - tc.tile_y) * 256.0);
        return tc;
    }

    /**
     * Convert tile coordinates (top-left corner) back to lat/lon.
     */
    inline void tile_to_latlon(int tile_x, int tile_y, int z, double& lat, double& lon) {
        double n = (double)(1 << z);
        lon = (double)tile_x / n * 360.0 - 180.0;
        double lat_rad = atan(sinh(M_PI * (1.0 - 2.0 * (double)tile_y / n)));
        lat = lat_rad * 180.0 / M_PI;
    }

    /**
     * Convert a global pixel coordinate to lat/lon at a given zoom level.
     * Global pixel = tile * 256 + pixel_offset
     */
    inline void pixel_to_latlon(double global_px, double global_py, int z,
                                 double& lat, double& lon) {
        double n = (double)(1 << z);
        lon = global_px / (n * 256.0) * 360.0 - 180.0;
        double lat_rad = atan(sinh(M_PI * (1.0 - 2.0 * global_py / (n * 256.0))));
        lat = lat_rad * 180.0 / M_PI;
    }

    /**
     * Convert lat/lon to global pixel coordinates at a given zoom level.
     */
    inline void latlon_to_pixel(double lat, double lon, int z,
                                 double& px, double& py) {
        double n = (double)(1 << z);
        px = (lon + 180.0) / 360.0 * n * 256.0;
        double lat_rad = lat * M_PI / 180.0;
        py = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * n * 256.0;
    }

} // namespace TileMath

} // namespace LXMF
} // namespace UI

#endif // UI_LXMF_TILEMATH_H
