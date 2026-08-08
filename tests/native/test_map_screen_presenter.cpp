#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "UI/LXMF/MapScreenPresenter.h"

namespace {
int passed = 0;
int failures = 0;
#define CHECK(expr) do { if (expr) { ++passed; } else { ++failures; std::cerr << "FAIL line " << __LINE__ << ": " #expr << '\n'; } } while (false)

using Pyxis::MapScreenPresenter;
using Pyxis::MapTileCompletion;
using Pyxis::MapTileLoadResult;
using Pyxis::MapTileRequest;
using Pyxis::MapTileSlot;

Pyxis::MapView::Request requestAt(double latitude, double longitude, std::uint32_t zoom) {
    Pyxis::MapView::Request request{};
    request.center = {latitude, longitude};
    request.zoom = zoom;
    request.width = MapScreenPresenter::VIEWPORT_WIDTH;
    request.height = MapScreenPresenter::VIEWPORT_HEIGHT;
    request.include_tile_border = false;
    return request;
}

bool sameKey(const Hardware::TDeck::TileKey& a,
             const Hardware::TDeck::TileKey& b) {
    return a.zoom == b.zoom && a.x == b.x && a.y == b.y;
}

MapTileCompletion completionFor(const MapTileRequest& request,
                                MapTileLoadResult result) {
    MapTileCompletion completion{};
    completion.generation = request.generation;
    completion.frame_epoch = request.frame_epoch;
    completion.slot_token = request.slot_token;
    completion.slot_index = request.slot_index;
    completion.key = request.key;
    completion.result = result;
    return completion;
}

void fixedCapacityAndDedupe() {
    MapScreenPresenter presenter;
    presenter.show();
    CHECK(presenter.buildFrame(requestAt(0.0, 0.0, 3)) ==
          Pyxis::MapView::Result::OK);
    CHECK(presenter.frame().tile_count <= MapScreenPresenter::TILE_SLOT_COUNT);
    CHECK(presenter.requestCount() == presenter.frame().tile_count);

    MapTileRequest requests[MapScreenPresenter::TILE_REQUEST_CAPACITY]{};
    std::size_t count = 0;
    while (presenter.takeRequest(requests[count])) ++count;
    CHECK(count == presenter.frame().tile_count);
    CHECK(!presenter.takeRequest(requests[0]));
    CHECK(presenter.buildFrame(requestAt(0.0, 0.0, 3)) ==
          Pyxis::MapView::Result::OK);
    CHECK(presenter.requestCount() == 0);
    for (std::size_t a = 0; a < count; ++a) {
        for (std::size_t b = a + 1; b < count; ++b) {
            CHECK(!sameKey(requests[a].key, requests[b].key));
        }
    }
}

void staleCompletionsRejectedAndAcceptedOnce() {
    MapScreenPresenter presenter;
    presenter.show();
    CHECK(presenter.buildFrame(requestAt(0.0, 0.0, 4)) ==
          Pyxis::MapView::Result::OK);
    MapTileRequest request{};
    CHECK(presenter.takeRequest(request));

    MapTileCompletion stale = completionFor(request, MapTileLoadResult::READY);
    ++stale.generation;
    CHECK(!presenter.publishCompletion(stale));
    stale = completionFor(request, MapTileLoadResult::READY);
    ++stale.frame_epoch;
    CHECK(!presenter.publishCompletion(stale));
    stale = completionFor(request, MapTileLoadResult::READY);
    ++stale.slot_token;
    CHECK(!presenter.publishCompletion(stale));
    stale = completionFor(request, MapTileLoadResult::READY);
    stale.key.x ^= 1U;
    CHECK(!presenter.publishCompletion(stale));
    MapTileCompletion output{};
    CHECK(!presenter.takeApplicableCompletion(output));

    const MapTileCompletion good = completionFor(request, MapTileLoadResult::READY);
    CHECK(presenter.publishCompletion(good));
    CHECK(presenter.takeApplicableCompletion(output));
    CHECK(output.slot_index == request.slot_index);
    CHECK(presenter.slot(request.slot_index).state == MapTileSlot::READY);
    CHECK(!presenter.takeApplicableCompletion(output));
    CHECK(!presenter.publishCompletion(good));
}

void newestFrameReusesSlotsAndPurgesOldRequests() {
    MapScreenPresenter presenter;
    presenter.show();
    CHECK(presenter.buildFrame(requestAt(0.0, 0.0, 4)) ==
          Pyxis::MapView::Result::OK);
    const std::uint32_t first_epoch = presenter.frameEpoch();
    const std::uint32_t tokens_before[MapScreenPresenter::TILE_SLOT_COUNT] = {
        presenter.slot(0).token, presenter.slot(1).token,
        presenter.slot(2).token, presenter.slot(3).token,
        presenter.slot(4).token, presenter.slot(5).token};
    CHECK(presenter.panPixels(300.0, 0.0));
    CHECK(presenter.frameEpoch() != first_epoch);
    CHECK(presenter.buildFrame(requestAt(99.0, 99.0, 1)) ==
          Pyxis::MapView::Result::OK);
    CHECK(presenter.requestCount() <= MapScreenPresenter::TILE_REQUEST_CAPACITY);
    std::size_t occupied = 0;
    bool token_advanced = false;
    for (std::size_t i = 0; i < MapScreenPresenter::TILE_SLOT_COUNT; ++i) {
        if (presenter.slot(i).state != MapTileSlot::EMPTY) ++occupied;
        if (presenter.slot(i).token != tokens_before[i]) token_advanced = true;
    }
    CHECK(occupied == presenter.frame().tile_count);
    CHECK(token_advanced);
    MapTileRequest next{};
    while (presenter.takeRequest(next)) {
        CHECK(next.frame_epoch == presenter.frameEpoch());
        CHECK(next.generation == presenter.generation());
    }
}

void generationPanZoomAndRecenterBounds() {
    MapScreenPresenter presenter;
    const std::uint32_t initial_generation = presenter.generation();
    presenter.show();
    CHECK(presenter.generation() != initial_generation);
    const std::uint32_t shown_generation = presenter.generation();
    presenter.hide();
    CHECK(presenter.generation() != shown_generation);
    CHECK(!presenter.active());
    presenter.show();

    CHECK(presenter.setView({0.0, 179.99}, 22));
    CHECK(presenter.panPixels(100000.0, 1000000000.0));
    CHECK(presenter.center().longitude >= -180.0);
    CHECK(presenter.center().longitude < 180.0);
    CHECK(std::fabs(presenter.center().latitude) <=
          Pyxis::MapProjection::WEB_MERCATOR_MAX_LATITUDE + 1e-9);
    CHECK(presenter.zoomBy(-100));
    CHECK(presenter.zoom() == 0U);
    CHECK(!presenter.zoomBy(-1));
    CHECK(presenter.zoomBy(100));
    CHECK(presenter.zoom() == 22U);
    CHECK(!presenter.zoomBy(1));

    Telemetry::LocationTelemetry fix{};
    fix.latitude_e6 = 51500000;
    fix.longitude_e6 = -114000;
    CHECK(!presenter.recenter(false, fix));
    CHECK(presenter.recenter(true, fix));
    CHECK(std::fabs(presenter.center().latitude - 51.5) < 1e-9);
    CHECK(std::fabs(presenter.center().longitude + 0.114) < 1e-9);
}

void visibleStatusSummarizesTheCurrentFrame() {
    MapScreenPresenter presenter;
    presenter.show();
    CHECK(presenter.buildFrame(requestAt(0.0, 0.0, 5)) ==
          Pyxis::MapView::Result::OK);

    MapTileLoadResult visible = MapTileLoadResult::IO_ERROR;
    CHECK(!presenter.visibleTileStatus(visible));

    MapTileRequest requests[MapScreenPresenter::TILE_REQUEST_CAPACITY]{};
    std::size_t count = 0U;
    while (count < MapScreenPresenter::TILE_REQUEST_CAPACITY &&
           presenter.takeRequest(requests[count])) ++count;
    CHECK(count > 1U);
    for (std::size_t index = 0U; index < count; ++index) {
        const MapTileLoadResult result = index == 0U
            ? MapTileLoadResult::READY : MapTileLoadResult::MISS;
        CHECK(presenter.publishCompletion(completionFor(requests[index], result)));
    }
    MapTileCompletion completion{};
    while (presenter.takeApplicableCompletion(completion)) {}
    CHECK(presenter.visibleTileStatus(visible));
    CHECK(visible == MapTileLoadResult::READY);

    CHECK(presenter.zoomBy(1));
    CHECK(presenter.buildFrame(requestAt(0.0, 0.0, 6)) ==
          Pyxis::MapView::Result::OK);
    count = 0U;
    while (count < MapScreenPresenter::TILE_REQUEST_CAPACITY &&
           presenter.takeRequest(requests[count])) ++count;
    for (std::size_t index = 0U; index < count; ++index) {
        CHECK(presenter.publishCompletion(
            completionFor(requests[index], MapTileLoadResult::MISS)));
    }
    while (presenter.takeApplicableCompletion(completion)) {}
    CHECK(presenter.visibleTileStatus(visible));
    CHECK(visible == MapTileLoadResult::MISS);
}

void resultStatesAndQueueLimit() {
    MapScreenPresenter presenter;
    presenter.show();
    CHECK(presenter.buildFrame(requestAt(0.0, 0.0, 5)) ==
          Pyxis::MapView::Result::OK);
    MapTileRequest requests[MapScreenPresenter::TILE_REQUEST_CAPACITY]{};
    std::size_t count = 0;
    while (count < MapScreenPresenter::TILE_REQUEST_CAPACITY &&
           presenter.takeRequest(requests[count])) ++count;
    CHECK(count <= MapScreenPresenter::TILE_REQUEST_CAPACITY);
    const MapTileLoadResult results[] = {
        MapTileLoadResult::MISS,
        MapTileLoadResult::STORAGE_UNAVAILABLE,
        MapTileLoadResult::INVALID_PNG,
        MapTileLoadResult::TOO_LARGE,
        MapTileLoadResult::IO_ERROR,
        MapTileLoadResult::READY};
    for (std::size_t i = 0; i < count; ++i) {
        CHECK(presenter.publishCompletion(completionFor(requests[i], results[i])));
    }
    CHECK(presenter.completionCount() == count);
    MapTileCompletion completion{};
    std::size_t applied = 0;
    while (presenter.takeApplicableCompletion(completion)) ++applied;
    CHECK(applied == count);
    CHECK(presenter.completionCount() == 0);
}

void deterministicHundredThousandOperationStress() {
    MapScreenPresenter presenter;
    presenter.show();
    std::uint32_t state = 0x12345678U;
    for (std::size_t operation = 0; operation < 100000U; ++operation) {
        state = state * 1664525U + 1013904223U;
        if ((state & 15U) == 0U) {
            presenter.zoomBy((state & 16U) ? 1 : -1);
        } else {
            const double dx = static_cast<int>((state >> 8) & 127U) - 63;
            const double dy = static_cast<int>((state >> 16) & 127U) - 63;
            (void)presenter.panPixels(dx, dy);
        }
        Pyxis::MapView::Request request =
            requestAt(presenter.center().latitude,
                      presenter.center().longitude,
                      presenter.zoom());
        CHECK(presenter.buildFrame(request) == Pyxis::MapView::Result::OK);
        CHECK(presenter.frame().tile_count <= MapScreenPresenter::TILE_SLOT_COUNT);
        CHECK(presenter.requestCount() <= MapScreenPresenter::TILE_REQUEST_CAPACITY);
        CHECK(presenter.completionCount() <= MapScreenPresenter::TILE_COMPLETION_CAPACITY);
        MapTileRequest tile_request{};
        if (presenter.takeRequest(tile_request)) {
            CHECK(presenter.publishCompletion(
                completionFor(tile_request, MapTileLoadResult::READY)));
            MapTileCompletion completion{};
            CHECK(presenter.takeApplicableCompletion(completion));
        }
    }
}
}  // namespace

int main() {
    fixedCapacityAndDedupe();
    staleCompletionsRejectedAndAcceptedOnce();
    newestFrameReusesSlotsAndPurgesOldRequests();
    generationPanZoomAndRecenterBounds();
    visibleStatusSummarizesTheCurrentFrame();
    resultStatesAndQueueLimit();
    deterministicHundredThousandOperationStress();
    std::cout << "map screen presenter: " << passed << " passed, "
              << failures << " failed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
