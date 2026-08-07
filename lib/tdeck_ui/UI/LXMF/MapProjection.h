#ifndef PYXIS_UI_LXMF_MAP_PROJECTION_H
#define PYXIS_UI_LXMF_MAP_PROJECTION_H

#include <cstddef>
#include <cstdint>

namespace Pyxis {
namespace MapProjection {

// Zoom 22 is the highest level normally published by OSM tile providers. With
// 256 px tiles its 1,073,741,824 px world remains exactly representable and all
// integer tile indices remain safely inside uint32_t without shifting.
enum {
    TILE_SIZE = 256,
    MAX_ZOOM = 22,
    MAX_VIEWPORT_TILES = 64
};

extern const double WEB_MERCATOR_MAX_LATITUDE;

enum class Status {
    OK,
    INVALID_ARGUMENT,
    INVALID_ZOOM,
    CAPACITY_EXCEEDED,
    VIEWPORT_TOO_LARGE
};

struct GeoPoint {
    double latitude;
    double longitude;
};

struct GlobalPixel {
    double x;
    double y;
};

struct TileIndex {
    std::uint32_t x;
    std::uint32_t y;
    std::uint32_t zoom;
};

// left/top are global-pixel coordinates. X may be outside the canonical world
// to support seamless antimeridian panning. Right and bottom edges are
// exclusive; width and height must be non-zero.
struct Viewport {
    double left;
    double top;
    std::uint32_t width;
    std::uint32_t height;
};

struct TilePlacement {
    TileIndex tile;
    double screen_x;
    double screen_y;
};

struct MarkerProjection {
    double screen_x;
    double screen_y;
    bool visible;
};

double clampLatitude(double latitude);
double normalizeLongitude(double longitude);
bool isValidZoom(std::uint32_t zoom);
std::uint32_t tileCount(std::uint32_t zoom);

Status latLonToGlobalPixel(const GeoPoint& point, std::uint32_t zoom, GlobalPixel& output);
Status globalPixelToLatLon(const GlobalPixel& pixel, std::uint32_t zoom, GeoPoint& output);
Status globalPixelToTile(const GlobalPixel& pixel, std::uint32_t zoom, TileIndex& output);

// Computes unique wrapped/clamped tile indices into caller-owned storage. An
// optional border adds one raw tile on every side. At most
// MAX_VIEWPORT_TILES raw candidates are accepted, bounding stack use and run
// time. On failure count is zero and output storage is unchanged.
Status viewportTiles(const Viewport& viewport,
                     std::uint32_t zoom,
                     bool include_border,
                     TilePlacement* output,
                     std::size_t capacity,
                     std::size_t& count);

// Uses the shortest wrapped horizontal delta from viewport center. Visibility
// follows half-open screen bounds [0,width) x [0,height).
Status projectMarker(const GeoPoint& point,
                     const Viewport& viewport,
                     std::uint32_t zoom,
                     MarkerProjection& output);

// Applies screen-pixel pan deltas, wrapping X and clamping Y to the world.
Status panGlobalPixel(const GlobalPixel& input,
                      double delta_x,
                      double delta_y,
                      std::uint32_t zoom,
                      GlobalPixel& output);

} // namespace MapProjection
} // namespace Pyxis

#endif
