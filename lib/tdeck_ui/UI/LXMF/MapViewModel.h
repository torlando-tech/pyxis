#ifndef PYXIS_UI_LXMF_MAP_VIEW_MODEL_H
#define PYXIS_UI_LXMF_MAP_VIEW_MODEL_H

#include <cstddef>
#include <cstdint>

#include "MapProjection.h"
#include "Telemetry/LocationShareState.h"

namespace Pyxis {
namespace MapView {

enum {
    MAX_MAP_PEERS = Telemetry::MAX_PEER_LOCATIONS,
    MAX_MAP_MARKERS = Telemetry::MAX_PEER_LOCATIONS + 1
};

enum class Result : std::uint8_t {
    OK,
    INVALID_ARGUMENT,
    CAPACITY_EXCEEDED,
    VIEWPORT_TOO_LARGE
};

enum class MarkerKind : std::uint8_t {
    LOCAL,
    PEER
};

struct Marker {
    MarkerKind kind = MarkerKind::PEER;
    Telemetry::PeerId peer{};
    double screen_x = 0.0;
    double screen_y = 0.0;
    bool has_approx_radius = false;
    std::uint32_t approx_radius_meters = 0;
    double approx_radius_pixels = 0.0;
};

struct Request {
    MapProjection::GeoPoint center{};
    std::uint32_t zoom = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool include_tile_border = false;
    bool has_local_location = false;
    Telemetry::LocationTelemetry local_location{};
    const Telemetry::PeerLocationRecord* peers = nullptr;
    std::size_t peer_count = 0;
    std::uint64_t wall_now_millis = 0;
};

struct Frame {
    MapProjection::Viewport viewport{};
    MapProjection::TilePlacement tiles[MapProjection::MAX_VIEWPORT_TILES]{};
    std::size_t tile_count = 0;
    Marker markers[MAX_MAP_MARKERS]{};
    std::size_t marker_count = 0;
};

// Builds into caller-owned fixed storage. Invalid/capacity failures leave output
// unchanged. Expiry is exclusive: a peer is hidden when wall_now >= expiry.
Result buildFrame(const Request& request, Frame& output);

}  // namespace MapView
}  // namespace Pyxis

#endif
