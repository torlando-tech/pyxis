#include "MapViewModel.h"

#include <cmath>

namespace Pyxis {
namespace MapView {
namespace {

bool validLocation(const Telemetry::LocationTelemetry& location) {
    return location.latitude_e6 >= -90000000 &&
           location.latitude_e6 <= 90000000 &&
           location.longitude_e6 >= -180000000 &&
           location.longitude_e6 <= 180000000;
}

bool validRequest(const Request& request) {
    if (!(std::isfinite(request.center.latitude) &&
           std::isfinite(request.center.longitude) &&
           request.center.latitude >= -90.0 &&
           request.center.latitude <= 90.0 &&
           request.width != 0 && request.height != 0 &&
           MapProjection::isValidZoom(request.zoom) &&
           request.peer_count <= MAX_MAP_PEERS &&
           (request.peer_count == 0 || request.peers != nullptr))) {
        return false;
    }
    if (request.has_local_location &&
        !validLocation(request.local_location)) return false;
    for (std::size_t index = 0; index < request.peer_count; ++index) {
        if (!validLocation(request.peers[index].location)) return false;
    }
    return true;
}

MapProjection::GeoPoint pointFromLocation(
    const Telemetry::LocationTelemetry& location) {
    MapProjection::GeoPoint point{};
    point.latitude = static_cast<double>(location.latitude_e6) / 1000000.0;
    point.longitude = static_cast<double>(location.longitude_e6) / 1000000.0;
    return point;
}

void appendMarker(const Telemetry::LocationTelemetry& location,
                  MarkerKind kind,
                  const Telemetry::PeerId& peer,
                  bool has_approx_radius,
                  std::uint32_t approx_radius_meters,
                  const MapProjection::Viewport& viewport,
                  std::uint32_t zoom,
                  Frame& output) {
    MapProjection::MarkerProjection projected{};
    if (MapProjection::projectMarker(
            pointFromLocation(location), viewport, zoom, projected) !=
            MapProjection::Status::OK ||
        !projected.visible) {
        return;
    }
    Marker& marker = output.markers[output.marker_count++];
    marker.kind = kind;
    marker.peer = peer;
    marker.screen_x = projected.screen_x;
    marker.screen_y = projected.screen_y;
    marker.has_approx_radius = has_approx_radius;
    marker.approx_radius_meters = approx_radius_meters;
}

}  // namespace

Result buildFrame(const Request& request, Frame& output) {
    if (!validRequest(request)) {
        return request.peer_count > MAX_MAP_PEERS
                   ? Result::CAPACITY_EXCEEDED
                   : Result::INVALID_ARGUMENT;
    }

    MapProjection::GlobalPixel center_pixel{};
    if (MapProjection::latLonToGlobalPixel(
            request.center, request.zoom, center_pixel) !=
        MapProjection::Status::OK) {
        return Result::INVALID_ARGUMENT;
    }

    MapProjection::Viewport viewport{};
    viewport.left = center_pixel.x - static_cast<double>(request.width) / 2.0;
    viewport.top = center_pixel.y - static_cast<double>(request.height) / 2.0;
    viewport.width = request.width;
    viewport.height = request.height;

    std::size_t tile_count = 0;
    const MapProjection::Status tile_status = MapProjection::viewportTiles(
        viewport, request.zoom, request.include_tile_border,
        output.tiles, MapProjection::MAX_VIEWPORT_TILES, tile_count);
    if (tile_status != MapProjection::Status::OK) {
        return tile_status == MapProjection::Status::VIEWPORT_TOO_LARGE ||
                       tile_status == MapProjection::Status::CAPACITY_EXCEEDED
                   ? Result::VIEWPORT_TOO_LARGE
                   : Result::INVALID_ARGUMENT;
    }

    output.viewport = viewport;
    output.tile_count = tile_count;
    output.marker_count = 0;
    const Telemetry::PeerId empty_peer{};
    if (request.has_local_location) {
        appendMarker(request.local_location, MarkerKind::LOCAL, empty_peer,
                     false, 0, viewport, request.zoom, output);
    }
    for (std::size_t index = 0; index < request.peer_count; ++index) {
        const Telemetry::PeerLocationRecord& record = request.peers[index];
        if (record.has_expiry &&
            request.wall_now_millis >= record.expires_at_millis) {
            continue;
        }
        appendMarker(record.location, MarkerKind::PEER, record.peer,
                     record.has_approx_radius,
                     record.approx_radius_meters,
                     viewport, request.zoom, output);
    }
    return Result::OK;
}

}  // namespace MapView
}  // namespace Pyxis
