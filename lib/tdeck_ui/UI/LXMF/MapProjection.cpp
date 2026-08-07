#include "MapProjection.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace Pyxis {
namespace MapProjection {

const double WEB_MERCATOR_MAX_LATITUDE = 85.05112878;

namespace {

const double PI = 3.141592653589793238462643383279502884;
const double MAX_RAW_TILE_COORDINATE = 1000000000000.0;

bool finite(double value) {
    return std::isfinite(value) != 0;
}

double worldPixels(std::uint32_t zoom) {
    return static_cast<double>(tileCount(zoom)) * static_cast<double>(TILE_SIZE);
}

double wrap(double value, double period) {
    double result = std::fmod(value, period);
    if (result < 0.0) {
        result += period;
    }
    // Guard against a possible rounded period after arithmetic at the edge.
    return result >= period ? 0.0 : result;
}

double shortestWrappedDelta(double delta, double period) {
    return wrap(delta + (period * 0.5), period) - (period * 0.5);
}

std::uint32_t wrapTileX(std::int64_t raw, std::uint32_t tiles) {
    const std::int64_t period = static_cast<std::int64_t>(tiles);
    std::int64_t result = raw % period;
    if (result < 0) {
        result += period;
    }
    return static_cast<std::uint32_t>(result);
}

std::uint32_t clampTileY(std::int64_t raw, std::uint32_t tiles) {
    if (raw < 0) {
        return 0U;
    }
    const std::int64_t last = static_cast<std::int64_t>(tiles) - 1;
    if (raw > last) {
        return tiles - 1U;
    }
    return static_cast<std::uint32_t>(raw);
}

bool validViewport(const Viewport& viewport) {
    return finite(viewport.left) && finite(viewport.top) &&
           (viewport.width != 0U) && (viewport.height != 0U);
}

} // namespace

double clampLatitude(double latitude) {
    if (latitude > WEB_MERCATOR_MAX_LATITUDE) {
        return WEB_MERCATOR_MAX_LATITUDE;
    }
    if (latitude < -WEB_MERCATOR_MAX_LATITUDE) {
        return -WEB_MERCATOR_MAX_LATITUDE;
    }
    return latitude;
}

double normalizeLongitude(double longitude) {
    if (!finite(longitude)) {
        return longitude;
    }
    double result = std::fmod(longitude, 360.0);
    if (result >= 180.0) {
        result -= 360.0;
    } else if (result < -180.0) {
        result += 360.0;
    }
    return result;
}

bool isValidZoom(std::uint32_t zoom) {
    return zoom <= static_cast<std::uint32_t>(MAX_ZOOM);
}

std::uint32_t tileCount(std::uint32_t zoom) {
    if (!isValidZoom(zoom)) {
        return 0U;
    }
    std::uint32_t count = 1U;
    for (std::uint32_t level = 0U; level < zoom; ++level) {
        count *= 2U;
    }
    return count;
}

Status latLonToGlobalPixel(const GeoPoint& point, std::uint32_t zoom, GlobalPixel& output) {
    if (!isValidZoom(zoom)) {
        return Status::INVALID_ZOOM;
    }
    if (!finite(point.latitude) || !finite(point.longitude)) {
        return Status::INVALID_ARGUMENT;
    }

    const double world = worldPixels(zoom);
    const double latitude = clampLatitude(point.latitude);
    const double longitude = normalizeLongitude(point.longitude);
    const double radians = latitude * PI / 180.0;
    const double mercator = std::log(std::tan((PI * 0.25) + (radians * 0.5)));
    GlobalPixel result;
    result.x = ((longitude + 180.0) / 360.0) * world;
    result.y = (0.5 - (mercator / (2.0 * PI))) * world;
    if (result.y < 0.0) {
        result.y = 0.0;
    } else if (result.y > world) {
        result.y = world;
    }
    output = result;
    return Status::OK;
}

Status globalPixelToLatLon(const GlobalPixel& pixel, std::uint32_t zoom, GeoPoint& output) {
    if (!isValidZoom(zoom)) {
        return Status::INVALID_ZOOM;
    }
    if (!finite(pixel.x) || !finite(pixel.y)) {
        return Status::INVALID_ARGUMENT;
    }

    const double world = worldPixels(zoom);
    double y = pixel.y;
    if (y < 0.0) {
        y = 0.0;
    } else if (y > world) {
        y = world;
    }
    const double normalized_y = 0.5 - (y / world);
    GeoPoint result;
    result.latitude = (180.0 / PI) * std::atan(std::sinh(2.0 * PI * normalized_y));
    result.latitude = clampLatitude(result.latitude);
    result.longitude = normalizeLongitude((wrap(pixel.x, world) / world * 360.0) - 180.0);
    output = result;
    return Status::OK;
}

Status globalPixelToTile(const GlobalPixel& pixel, std::uint32_t zoom, TileIndex& output) {
    if (!isValidZoom(zoom)) {
        return Status::INVALID_ZOOM;
    }
    if (!finite(pixel.x) || !finite(pixel.y)) {
        return Status::INVALID_ARGUMENT;
    }

    const std::uint32_t tiles = tileCount(zoom);
    const double world = worldPixels(zoom);
    const double wrapped_x = wrap(pixel.x, world);
    double clamped_y = pixel.y;
    if (clamped_y < 0.0) {
        clamped_y = 0.0;
    } else if (clamped_y >= world) {
        clamped_y = std::nextafter(world, 0.0);
    }
    TileIndex result;
    result.x = static_cast<std::uint32_t>(std::floor(wrapped_x / static_cast<double>(TILE_SIZE)));
    result.y = static_cast<std::uint32_t>(std::floor(clamped_y / static_cast<double>(TILE_SIZE)));
    result.zoom = zoom;
    if (result.x >= tiles) {
        result.x = 0U;
    }
    if (result.y >= tiles) {
        result.y = tiles - 1U;
    }
    output = result;
    return Status::OK;
}

Status viewportTiles(const Viewport& viewport,
                     std::uint32_t zoom,
                     bool include_border,
                     TilePlacement* output,
                     std::size_t capacity,
                     std::size_t& count) {
    count = 0U;
    if (!isValidZoom(zoom)) {
        return Status::INVALID_ZOOM;
    }
    if (!validViewport(viewport) || ((output == NULL) && (capacity != 0U))) {
        return Status::INVALID_ARGUMENT;
    }

    const double right = viewport.left + static_cast<double>(viewport.width);
    const double bottom = viewport.top + static_cast<double>(viewport.height);
    const double tile_size = static_cast<double>(TILE_SIZE);
    if (!finite(right) || !finite(bottom)) {
        return Status::INVALID_ARGUMENT;
    }

    const double world = worldPixels(zoom);
    const double clipped_top = viewport.top < 0.0 ? 0.0 : viewport.top;
    const double clipped_bottom = bottom > world ? world : bottom;
    if (clipped_top >= clipped_bottom) {
        return Status::OK;
    }

    double first_x_value = std::floor(viewport.left / tile_size);
    double last_x_value = std::ceil(right / tile_size) - 1.0;
    double first_y_value = std::floor(clipped_top / tile_size);
    double last_y_value = std::ceil(clipped_bottom / tile_size) - 1.0;
    if (include_border) {
        first_x_value -= 1.0;
        last_x_value += 1.0;
        first_y_value -= 1.0;
        last_y_value += 1.0;
    }
    if ((std::fabs(first_x_value) > MAX_RAW_TILE_COORDINATE) ||
        (std::fabs(last_x_value) > MAX_RAW_TILE_COORDINATE) ||
        (std::fabs(first_y_value) > MAX_RAW_TILE_COORDINATE) ||
        (std::fabs(last_y_value) > MAX_RAW_TILE_COORDINATE)) {
        return Status::VIEWPORT_TOO_LARGE;
    }

    const std::int64_t first_x = static_cast<std::int64_t>(first_x_value);
    const std::int64_t last_x = static_cast<std::int64_t>(last_x_value);
    const std::int64_t first_y = static_cast<std::int64_t>(first_y_value);
    const std::int64_t last_y = static_cast<std::int64_t>(last_y_value);
    const std::int64_t columns = (last_x - first_x) + 1;
    const std::int64_t rows = (last_y - first_y) + 1;
    if ((columns <= 0) || (rows <= 0) ||
        (columns > static_cast<std::int64_t>(MAX_VIEWPORT_TILES)) ||
        (rows > static_cast<std::int64_t>(MAX_VIEWPORT_TILES)) ||
        (columns * rows > static_cast<std::int64_t>(MAX_VIEWPORT_TILES))) {
        return Status::VIEWPORT_TOO_LARGE;
    }

    TilePlacement candidates[MAX_VIEWPORT_TILES];
    std::size_t unique_count = 0U;
    const std::uint32_t tiles = tileCount(zoom);
    const double viewport_center_x = static_cast<double>(viewport.width) * 0.5;
    for (std::int64_t raw_y = first_y; raw_y <= last_y; ++raw_y) {
        const std::uint32_t y = clampTileY(raw_y, tiles);
        const double screen_y = (static_cast<double>(y) * tile_size) - viewport.top;
        for (std::int64_t raw_x = first_x; raw_x <= last_x; ++raw_x) {
            const std::uint32_t x = wrapTileX(raw_x, tiles);
            const double screen_x = (static_cast<double>(raw_x) * tile_size) - viewport.left;
            std::size_t duplicate = unique_count;
            for (std::size_t i = 0U; i < unique_count; ++i) {
                if ((candidates[i].tile.x == x) && (candidates[i].tile.y == y)) {
                    duplicate = i;
                    break;
                }
            }
            if (duplicate == unique_count) {
                candidates[unique_count].tile.x = x;
                candidates[unique_count].tile.y = y;
                candidates[unique_count].tile.zoom = zoom;
                candidates[unique_count].screen_x = screen_x;
                candidates[unique_count].screen_y = screen_y;
                ++unique_count;
            } else {
                const double old_distance = std::fabs((candidates[duplicate].screen_x + (tile_size * 0.5)) - viewport_center_x);
                const double new_distance = std::fabs((screen_x + (tile_size * 0.5)) - viewport_center_x);
                if (new_distance < old_distance) {
                    candidates[duplicate].screen_x = screen_x;
                }
            }
        }
    }

    if (unique_count > capacity) {
        return Status::CAPACITY_EXCEEDED;
    }
    if ((unique_count != 0U) && (output == NULL)) {
        return Status::CAPACITY_EXCEEDED;
    }
    for (std::size_t i = 0U; i < unique_count; ++i) {
        output[i] = candidates[i];
    }
    count = unique_count;
    return Status::OK;
}

Status projectMarker(const GeoPoint& point,
                     const Viewport& viewport,
                     std::uint32_t zoom,
                     MarkerProjection& output) {
    if (!isValidZoom(zoom)) {
        return Status::INVALID_ZOOM;
    }
    if (!validViewport(viewport)) {
        return Status::INVALID_ARGUMENT;
    }
    GlobalPixel marker_pixel;
    const Status status = latLonToGlobalPixel(point, zoom, marker_pixel);
    if (status != Status::OK) {
        return status;
    }

    const double world = worldPixels(zoom);
    const double half_width = static_cast<double>(viewport.width) * 0.5;
    const double center_x = viewport.left + half_width;
    MarkerProjection result;
    result.screen_x = half_width + shortestWrappedDelta(marker_pixel.x - center_x, world);
    result.screen_y = marker_pixel.y - viewport.top;
    result.visible = (result.screen_x >= 0.0) &&
                     (result.screen_x < static_cast<double>(viewport.width)) &&
                     (result.screen_y >= 0.0) &&
                     (result.screen_y < static_cast<double>(viewport.height));
    output = result;
    return Status::OK;
}

Status panGlobalPixel(const GlobalPixel& input,
                      double delta_x,
                      double delta_y,
                      std::uint32_t zoom,
                      GlobalPixel& output) {
    if (!isValidZoom(zoom)) {
        return Status::INVALID_ZOOM;
    }
    if (!finite(input.x) || !finite(input.y) || !finite(delta_x) || !finite(delta_y)) {
        return Status::INVALID_ARGUMENT;
    }
    const double world = worldPixels(zoom);
    const double next_x = input.x + delta_x;
    const double next_y = input.y + delta_y;
    if (!finite(next_x) || !finite(next_y)) {
        return Status::INVALID_ARGUMENT;
    }
    GlobalPixel result;
    result.x = wrap(next_x, world);
    result.y = next_y;
    if (result.y < 0.0) {
        result.y = 0.0;
    } else if (result.y > world) {
        result.y = world;
    }
    output = result;
    return Status::OK;
}

} // namespace MapProjection
} // namespace Pyxis
