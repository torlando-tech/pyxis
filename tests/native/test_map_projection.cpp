#include "UI/LXMF/MapProjection.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

using Pyxis::MapProjection::GeoPoint;
using Pyxis::MapProjection::GlobalPixel;
using Pyxis::MapProjection::MarkerProjection;
using Pyxis::MapProjection::Status;
using Pyxis::MapProjection::TileIndex;
using Pyxis::MapProjection::TilePlacement;
using Pyxis::MapProjection::Viewport;

std::size_t tests_run = 0U;

void fail(const char* expression, int line) {
    std::cerr << "line " << line << ": check failed: " << expression << '\n';
    std::exit(1);
}

#define CHECK(expression) do { if (!(expression)) { fail(#expression, __LINE__); } } while (false)

bool near(double actual, double expected, double tolerance = 1.0e-9) {
    return std::fabs(actual - expected) <= tolerance;
}

void beginTest() { ++tests_run; }

bool hasTile(const TilePlacement* tiles, std::size_t count, std::uint32_t x, std::uint32_t y) {
    for (std::size_t i = 0U; i < count; ++i) {
        if ((tiles[i].tile.x == x) && (tiles[i].tile.y == y)) {
            return true;
        }
    }
    return false;
}

void testConstantsAndZoomValidation() {
    beginTest();
    CHECK(near(Pyxis::MapProjection::clampLatitude(90.0), Pyxis::MapProjection::WEB_MERCATOR_MAX_LATITUDE));
    CHECK(near(Pyxis::MapProjection::clampLatitude(-90.0), -Pyxis::MapProjection::WEB_MERCATOR_MAX_LATITUDE));
    CHECK(Pyxis::MapProjection::isValidZoom(0U));
    CHECK(Pyxis::MapProjection::isValidZoom(Pyxis::MapProjection::MAX_ZOOM));
    CHECK(!Pyxis::MapProjection::isValidZoom(Pyxis::MapProjection::MAX_ZOOM + 1U));
    CHECK(Pyxis::MapProjection::tileCount(0U) == 1U);
    CHECK(Pyxis::MapProjection::tileCount(Pyxis::MapProjection::MAX_ZOOM) == 4194304U);
    CHECK(Pyxis::MapProjection::tileCount(Pyxis::MapProjection::MAX_ZOOM + 1U) == 0U);
}

void testLongitudeNormalization() {
    beginTest();
    CHECK(near(Pyxis::MapProjection::normalizeLongitude(180.0), -180.0));
    CHECK(near(Pyxis::MapProjection::normalizeLongitude(540.0), -180.0));
    CHECK(near(Pyxis::MapProjection::normalizeLongitude(-540.0), -180.0));
    CHECK(near(Pyxis::MapProjection::normalizeLongitude(181.0), -179.0));
    CHECK(near(Pyxis::MapProjection::normalizeLongitude(-181.0), 179.0));
}

void testEquatorPrimeMeridian() {
    beginTest();
    GlobalPixel pixel = {0.0, 0.0};
    CHECK(Pyxis::MapProjection::latLonToGlobalPixel(GeoPoint{0.0, 0.0}, 0U, pixel) == Status::OK);
    CHECK(near(pixel.x, 128.0));
    CHECK(near(pixel.y, 128.0));
    GeoPoint point = {1.0, 1.0};
    CHECK(Pyxis::MapProjection::globalPixelToLatLon(pixel, 0U, point) == Status::OK);
    CHECK(near(point.latitude, 0.0));
    CHECK(near(point.longitude, 0.0));
}

void testWorldCornersPolesAndAntimeridian() {
    beginTest();
    GlobalPixel northwest = {0.0, 0.0};
    GlobalPixel southeast = {0.0, 0.0};
    CHECK(Pyxis::MapProjection::latLonToGlobalPixel(GeoPoint{90.0, -180.0}, 3U, northwest) == Status::OK);
    CHECK(near(northwest.x, 0.0));
    CHECK(near(northwest.y, 0.0, 1.0e-7));
    CHECK(Pyxis::MapProjection::latLonToGlobalPixel(GeoPoint{-90.0, 179.999999}, 3U, southeast) == Status::OK);
    CHECK(southeast.x < 2048.0);
    CHECK(near(southeast.y, 2048.0, 1.0e-7));
    GlobalPixel east = {0.0, 0.0};
    GlobalPixel west = {0.0, 0.0};
    CHECK(Pyxis::MapProjection::latLonToGlobalPixel(GeoPoint{0.0, 180.0}, 3U, east) == Status::OK);
    CHECK(Pyxis::MapProjection::latLonToGlobalPixel(GeoPoint{0.0, -180.0}, 3U, west) == Status::OK);
    CHECK(near(east.x, west.x));
}

void testTileIndexWrappingAndClamping() {
    beginTest();
    TileIndex tile = {99U, 99U, 99U};
    CHECK(Pyxis::MapProjection::globalPixelToTile(GlobalPixel{-1.0, -500.0}, 2U, tile) == Status::OK);
    CHECK(tile.x == 3U);
    CHECK(tile.y == 0U);
    CHECK(tile.zoom == 2U);
    CHECK(Pyxis::MapProjection::globalPixelToTile(GlobalPixel{1024.0, 5000.0}, 2U, tile) == Status::OK);
    CHECK(tile.x == 0U);
    CHECK(tile.y == 3U);
}

void testRoundTripGridAtZoomBounds() {
    beginTest();
    const double latitudes[] = {-85.05112878, -80.0, -45.25, 0.0, 44.75, 80.0, 85.05112878};
    const double longitudes[] = {-180.0, -179.9, -90.0, 0.0, 89.5, 179.9};
    const std::uint32_t zooms[] = {0U, 1U, 8U, Pyxis::MapProjection::MAX_ZOOM};
    for (std::size_t z = 0U; z < (sizeof(zooms) / sizeof(zooms[0])); ++z) {
        for (std::size_t i = 0U; i < (sizeof(latitudes) / sizeof(latitudes[0])); ++i) {
            for (std::size_t j = 0U; j < (sizeof(longitudes) / sizeof(longitudes[0])); ++j) {
                const GeoPoint input = {latitudes[i], longitudes[j]};
                GlobalPixel pixel = {0.0, 0.0};
                GeoPoint output = {0.0, 0.0};
                CHECK(Pyxis::MapProjection::latLonToGlobalPixel(input, zooms[z], pixel) == Status::OK);
                CHECK(Pyxis::MapProjection::globalPixelToLatLon(pixel, zooms[z], output) == Status::OK);
                CHECK(near(output.latitude, input.latitude, 2.0e-8));
                CHECK(near(output.longitude, input.longitude, 2.0e-8));
            }
        }
    }
}

void testExactViewportEdgesAreExclusive() {
    beginTest();
    TilePlacement tiles[Pyxis::MapProjection::MAX_VIEWPORT_TILES] = {};
    std::size_t count = 99U;
    CHECK(Pyxis::MapProjection::viewportTiles(Viewport{256.0, 256.0, 256U, 256U}, 3U, false, tiles,
                                               Pyxis::MapProjection::MAX_VIEWPORT_TILES, count) == Status::OK);
    CHECK(count == 1U);
    CHECK(tiles[0].tile.x == 1U);
    CHECK(tiles[0].tile.y == 1U);
    CHECK(near(tiles[0].screen_x, 0.0));
    CHECK(near(tiles[0].screen_y, 0.0));
}

void testViewportTwoToThreeTileTransition() {
    beginTest();
    TilePlacement tiles[Pyxis::MapProjection::MAX_VIEWPORT_TILES] = {};
    std::size_t count = 0U;
    CHECK(Pyxis::MapProjection::viewportTiles(Viewport{0.0, 480.0, 320U, 100U}, 4U, false, tiles,
                                               Pyxis::MapProjection::MAX_VIEWPORT_TILES, count) == Status::OK);
    CHECK(count == 4U); // two columns by two rows
    CHECK(Pyxis::MapProjection::viewportTiles(Viewport{200.0, 480.0, 320U, 100U}, 4U, false, tiles,
                                               Pyxis::MapProjection::MAX_VIEWPORT_TILES, count) == Status::OK);
    CHECK(count == 6U); // three columns by two rows: old fixed 2x2 code lost these
    CHECK(hasTile(tiles, count, 0U, 1U));
    CHECK(hasTile(tiles, count, 1U, 1U));
    CHECK(hasTile(tiles, count, 2U, 1U));
}

void testViewportPrefetchBorderAndAntimeridian() {
    beginTest();
    TilePlacement tiles[Pyxis::MapProjection::MAX_VIEWPORT_TILES] = {};
    std::size_t count = 0U;
    CHECK(Pyxis::MapProjection::viewportTiles(Viewport{1000.0, 256.0, 64U, 64U}, 2U, true, tiles,
                                               Pyxis::MapProjection::MAX_VIEWPORT_TILES, count) == Status::OK);
    CHECK(count == 12U); // raw x 2..5 wraps across the antimeridian; y spans 0..2
    CHECK(hasTile(tiles, count, 0U, 0U));
    CHECK(hasTile(tiles, count, 3U, 2U));
}

void testLowZoomCoverageDeduplicatesWrappedAndClampedTiles() {
    beginTest();
    TilePlacement tiles[Pyxis::MapProjection::MAX_VIEWPORT_TILES] = {};
    std::size_t count = 0U;
    CHECK(Pyxis::MapProjection::viewportTiles(Viewport{-100.0, -100.0, 320U, 240U}, 0U, true, tiles,
                                               Pyxis::MapProjection::MAX_VIEWPORT_TILES, count) == Status::OK);
    CHECK(count == 1U);
    CHECK(tiles[0].tile.x == 0U);
    CHECK(tiles[0].tile.y == 0U);
}

void testCapacityAndCoverageBoundAreTransactional() {
    beginTest();
    TilePlacement tiles[2] = {};
    tiles[0].tile.x = 77U;
    std::size_t count = 55U;
    CHECK(Pyxis::MapProjection::viewportTiles(Viewport{200.0, 300.0, 320U, 100U}, 4U, false, tiles, 2U, count)
          == Status::CAPACITY_EXCEEDED);
    CHECK(count == 0U);
    CHECK(tiles[0].tile.x == 77U);
    CHECK(Pyxis::MapProjection::viewportTiles(Viewport{0.0, 0.0, 4096U, 4096U}, 10U, true, tiles, 2U, count)
          == Status::VIEWPORT_TOO_LARGE);
}

void testMarkerUsesShortestWrappedDeltaAndClipsExclusiveEdges() {
    beginTest();
    MarkerProjection marker = {0.0, 0.0, false};
    CHECK(Pyxis::MapProjection::projectMarker(GeoPoint{0.0, -179.0}, Viewport{1000.0, 384.0, 100U, 256U}, 2U,
                                              marker) == Status::OK);
    CHECK(marker.visible);
    CHECK(marker.screen_x > 20.0);
    CHECK(marker.screen_x < 30.0);
    CHECK(near(marker.screen_y, 128.0));
    CHECK(Pyxis::MapProjection::projectMarker(GeoPoint{0.0, 0.0}, Viewport{0.0, 128.0, 256U, 256U}, 1U,
                                              marker) == Status::OK);
    CHECK(!marker.visible); // x is exactly the exclusive right edge
}

void testPanDeltasWrapXClampY() {
    beginTest();
    GlobalPixel output = {0.0, 0.0};
    CHECK(Pyxis::MapProjection::panGlobalPixel(GlobalPixel{10.0, 10.0}, -20.0, -20.0, 2U, output) == Status::OK);
    CHECK(near(output.x, 1014.0));
    CHECK(near(output.y, 0.0));
    CHECK(Pyxis::MapProjection::panGlobalPixel(output, 20.0, 5000.0, 2U, output) == Status::OK);
    CHECK(near(output.x, 10.0));
    CHECK(near(output.y, 1024.0));
}

void testMalformedArgumentsFailClosed() {
    beginTest();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    GlobalPixel pixel = {7.0, 8.0};
    GeoPoint point = {7.0, 8.0};
    TileIndex tile = {7U, 8U, 9U};
    MarkerProjection marker = {7.0, 8.0, true};
    std::size_t count = 9U;
    CHECK(Pyxis::MapProjection::latLonToGlobalPixel(GeoPoint{nan, 0.0}, 1U, pixel) == Status::INVALID_ARGUMENT);
    CHECK(near(pixel.x, 7.0));
    CHECK(Pyxis::MapProjection::latLonToGlobalPixel(GeoPoint{0.0, 0.0}, 23U, pixel) == Status::INVALID_ZOOM);
    CHECK(Pyxis::MapProjection::globalPixelToLatLon(GlobalPixel{nan, 0.0}, 1U, point) == Status::INVALID_ARGUMENT);
    CHECK(Pyxis::MapProjection::globalPixelToTile(GlobalPixel{0.0, nan}, 1U, tile) == Status::INVALID_ARGUMENT);
    CHECK(Pyxis::MapProjection::viewportTiles(Viewport{0.0, 0.0, 0U, 1U}, 1U, false, NULL, 0U, count)
          == Status::INVALID_ARGUMENT);
    CHECK(count == 0U);
    CHECK(Pyxis::MapProjection::projectMarker(GeoPoint{0.0, 0.0}, Viewport{nan, 0.0, 1U, 1U}, 1U, marker)
          == Status::INVALID_ARGUMENT);
    CHECK(marker.visible);
}

void testDeterministicPropertiesAndStress() {
    beginTest();
    std::uint32_t state = 0x12345678U;
    for (std::size_t i = 0U; i < 100000U; ++i) {
        state = (state * 1664525U) + 1013904223U;
        const double latitude = (static_cast<double>(state) / 4294967295.0 * 220.0) - 110.0;
        state = (state * 1664525U) + 1013904223U;
        const double longitude = (static_cast<double>(state) / 4294967295.0 * 2160.0) - 1080.0;
        const std::uint32_t zoom = state % (Pyxis::MapProjection::MAX_ZOOM + 1U);
        GlobalPixel pixel = {0.0, 0.0};
        GeoPoint round_trip = {0.0, 0.0};
        CHECK(Pyxis::MapProjection::latLonToGlobalPixel(GeoPoint{latitude, longitude}, zoom, pixel) == Status::OK);
        CHECK(Pyxis::MapProjection::globalPixelToLatLon(pixel, zoom, round_trip) == Status::OK);
        CHECK(round_trip.latitude <= Pyxis::MapProjection::WEB_MERCATOR_MAX_LATITUDE);
        CHECK(round_trip.latitude >= -Pyxis::MapProjection::WEB_MERCATOR_MAX_LATITUDE);
        CHECK(round_trip.longitude >= -180.0);
        CHECK(round_trip.longitude < 180.0);
        TileIndex tile = {0U, 0U, 0U};
        CHECK(Pyxis::MapProjection::globalPixelToTile(pixel, zoom, tile) == Status::OK);
        CHECK(tile.x < Pyxis::MapProjection::tileCount(zoom));
        CHECK(tile.y < Pyxis::MapProjection::tileCount(zoom));
    }
}

} // namespace

int main() {
    testConstantsAndZoomValidation();
    testLongitudeNormalization();
    testEquatorPrimeMeridian();
    testWorldCornersPolesAndAntimeridian();
    testTileIndexWrappingAndClamping();
    testRoundTripGridAtZoomBounds();
    testExactViewportEdgesAreExclusive();
    testViewportTwoToThreeTileTransition();
    testViewportPrefetchBorderAndAntimeridian();
    testLowZoomCoverageDeduplicatesWrappedAndClampedTiles();
    testCapacityAndCoverageBoundAreTransactional();
    testMarkerUsesShortestWrappedDeltaAndClipsExclusiveEdges();
    testPanDeltasWrapXClampY();
    testMalformedArgumentsFailClosed();
    testDeterministicPropertiesAndStress();
    std::cout << "map projection: " << tests_run << " tests passed\n";
    return 0;
}
