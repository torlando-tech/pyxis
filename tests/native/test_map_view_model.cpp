#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "UI/LXMF/MapViewModel.h"

namespace {
int passed = 0;
int failures = 0;
#define CHECK(expr) do { if (expr) { ++passed; } else { ++failures; std::cerr << "FAIL line " << __LINE__ << ": " #expr << '\n'; } } while (false)

Telemetry::PeerId peer(uint8_t seed) {
    Telemetry::PeerId id{};
    for (std::size_t i = 0; i < Telemetry::PEER_ID_SIZE; ++i) {
        id.bytes[i] = static_cast<uint8_t>(seed + i);
    }
    return id;
}

bool samePeer(const Telemetry::PeerId& left, const Telemetry::PeerId& right) {
    for (std::size_t i = 0; i < Telemetry::PEER_ID_SIZE; ++i) {
        if (left.bytes[i] != right.bytes[i]) return false;
    }
    return true;
}

Telemetry::LocationTelemetry fix(double latitude, double longitude) {
    Telemetry::LocationTelemetry value{};
    value.latitude_e6 = static_cast<int32_t>(latitude * 1000000.0);
    value.longitude_e6 = static_cast<int32_t>(longitude * 1000000.0);
    value.timestamp_seconds = 1700000000ULL;
    return value;
}

void buildsBoundedTilesAndVisibleMarkers() {
    Pyxis::MapView::Request request{};
    request.center = {0.0, 179.9};
    request.zoom = 3;
    request.width = 320;
    request.height = 200;
    request.include_tile_border = true;
    request.local_location = fix(0.0, 179.9);
    request.has_local_location = true;
    request.wall_now_millis = 1700000000000ULL;

    Telemetry::PeerLocationRecord peers[3]{};
    peers[0].peer = peer(1);
    peers[0].location = fix(0.0, -179.9); // visible across antimeridian
    peers[0].has_approx_radius = true;
    peers[0].approx_radius_meters = 0;
    peers[1].peer = peer(2);
    peers[1].location = fix(80.0, 0.0); // outside viewport
    peers[2].peer = peer(3);
    peers[2].location = fix(0.0, 179.8);
    peers[2].has_expiry = true;
    peers[2].expires_at_millis = request.wall_now_millis; // exclusive expiry
    request.peers = peers;
    request.peer_count = 3;

    Pyxis::MapView::Frame frame{};
    CHECK(Pyxis::MapView::buildFrame(request, frame) ==
          Pyxis::MapView::Result::OK);
    CHECK(frame.tile_count > 0);
    CHECK(frame.tile_count <= Pyxis::MapProjection::MAX_VIEWPORT_TILES);
    CHECK(frame.marker_count == 2);
    CHECK(frame.markers[0].kind == Pyxis::MapView::MarkerKind::LOCAL);
    CHECK(frame.markers[1].kind == Pyxis::MapView::MarkerKind::PEER);
    CHECK(samePeer(frame.markers[1].peer, peer(1)));
    CHECK(frame.markers[1].has_approx_radius);
    CHECK(frame.markers[1].approx_radius_meters == 0);
    CHECK(frame.markers[1].approx_radius_pixels == 0.0);
}

void projectsApproximateRadiusToPixels() {
    Pyxis::MapView::Request request{};
    request.center = {0.0, 0.0};
    request.zoom = 10;
    request.width = 320;
    request.height = 200;
    request.wall_now_millis = 1700000000000ULL;
    Telemetry::PeerLocationRecord peer_record{};
    peer_record.peer = peer(9);
    peer_record.location = fix(0.0, 0.0);
    peer_record.has_approx_radius = true;
    peer_record.approx_radius_meters = 100000;
    request.peers = &peer_record;
    request.peer_count = 1;
    Pyxis::MapView::Frame frame{};
    CHECK(Pyxis::MapView::buildFrame(request, frame) == Pyxis::MapView::Result::OK);
    CHECK(frame.marker_count == 1);
    CHECK(frame.markers[0].approx_radius_pixels > 600.0);
    CHECK(frame.markers[0].approx_radius_pixels < 700.0);
}

void rejectsInvalidAndLeavesOutputUnchanged() {
    Pyxis::MapView::Request request{};
    request.center = {0.0, 0.0};
    request.zoom = Pyxis::MapProjection::MAX_ZOOM + 1;
    request.width = 320;
    request.height = 200;

    Pyxis::MapView::Frame frame{};
    frame.tile_count = 17;
    frame.marker_count = 9;
    CHECK(Pyxis::MapView::buildFrame(request, frame) ==
          Pyxis::MapView::Result::INVALID_ARGUMENT);
    CHECK(frame.tile_count == 17);
    CHECK(frame.marker_count == 9);

    request.zoom = 3;
    request.peer_count = Pyxis::MapView::MAX_MAP_PEERS + 1;
    CHECK(Pyxis::MapView::buildFrame(request, frame) ==
          Pyxis::MapView::Result::CAPACITY_EXCEEDED);
    CHECK(frame.tile_count == 17);

    request.peer_count = 0;
    request.has_local_location = true;
    request.local_location.latitude_e6 = 90000001;
    CHECK(Pyxis::MapView::buildFrame(request, frame) ==
          Pyxis::MapView::Result::INVALID_ARGUMENT);
    CHECK(frame.tile_count == 17);
}
}  // namespace

int main() {
    buildsBoundedTilesAndVisibleMarkers();
    projectsApproximateRadiusToPixels();
    rejectsInvalidAndLeavesOutputUnchanged();
    std::cout << "map view model: " << passed << " passed, " << failures
              << " failed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
