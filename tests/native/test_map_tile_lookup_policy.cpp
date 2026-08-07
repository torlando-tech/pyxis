#include "UI/LXMF/MapTileLookupPolicy.h"

#include <cstdlib>
#include <iostream>

using Pyxis::MapTileLoadResult;
using Pyxis::MapTileLookupPolicy;

namespace {
std::size_t tests_run = 0U;
void fail(const char* expression, int line) {
    std::cerr << "line " << line << ": " << expression << '\n';
    std::exit(1);
}
#define CHECK(expression) do { if (!(expression)) fail(#expression, __LINE__); } while (false)
void beginTest() { ++tests_run; }

void testLocalPrecedenceTable() {
    beginTest();
    CHECK(MapTileLookupPolicy::resolveLocal(MapTileLoadResult::READY,
                                            MapTileLoadResult::IO_ERROR) ==
          MapTileLoadResult::READY);
    const MapTileLoadResult fallthrough[] = {
        MapTileLoadResult::MISS, MapTileLoadResult::INVALID_PNG,
        MapTileLoadResult::TOO_LARGE
    };
    for (std::size_t index = 0U; index < sizeof(fallthrough) / sizeof(fallthrough[0]); ++index) {
        CHECK(MapTileLookupPolicy::resolveLocal(fallthrough[index],
                                                MapTileLoadResult::READY) ==
              MapTileLoadResult::READY);
        CHECK(MapTileLookupPolicy::resolveLocal(fallthrough[index],
                                                MapTileLoadResult::MISS) ==
              MapTileLoadResult::MISS);
    }
    CHECK(MapTileLookupPolicy::resolveLocal(MapTileLoadResult::IO_ERROR,
                                            MapTileLoadResult::MISS) ==
          MapTileLoadResult::IO_ERROR);
    CHECK(MapTileLookupPolicy::resolveLocal(MapTileLoadResult::STORAGE_UNAVAILABLE,
                                            MapTileLoadResult::MISS) ==
          MapTileLoadResult::STORAGE_UNAVAILABLE);
    CHECK(MapTileLookupPolicy::resolveLocal(MapTileLoadResult::IO_ERROR,
                                            MapTileLoadResult::READY) ==
          MapTileLoadResult::READY);
    CHECK(MapTileLookupPolicy::resolveLocal(MapTileLoadResult::MISS,
                                            MapTileLoadResult::INVALID_PNG) ==
          MapTileLoadResult::INVALID_PNG);
}

void testOnlineGate() {
    beginTest();
    CHECK(MapTileLookupPolicy::shouldStartOnline(MapTileLoadResult::MISS,
                                                 true, true, false, 7U, 7U));
    CHECK(MapTileLookupPolicy::shouldStartOnline(MapTileLoadResult::INVALID_PNG,
                                                 true, true, false, 7U, 7U));
    CHECK(!MapTileLookupPolicy::shouldStartOnline(MapTileLoadResult::READY,
                                                  true, true, false, 7U, 7U));
    CHECK(!MapTileLookupPolicy::shouldStartOnline(MapTileLoadResult::MISS,
                                                  false, true, false, 7U, 7U));
    CHECK(!MapTileLookupPolicy::shouldStartOnline(MapTileLoadResult::MISS,
                                                  true, false, false, 7U, 7U));
    CHECK(!MapTileLookupPolicy::shouldStartOnline(MapTileLoadResult::MISS,
                                                  true, true, true, 7U, 7U));
    CHECK(!MapTileLookupPolicy::shouldStartOnline(MapTileLoadResult::MISS,
                                                  true, true, false, 7U, 8U));
    CHECK(!MapTileLookupPolicy::shouldStartOnline(MapTileLoadResult::IO_ERROR,
                                                  true, true, false, 7U, 7U));
}

struct FakeLocalSources {
    MapTileLoadResult pack;
    MapTileLoadResult live;
    int calls[2];
    std::size_t count;
};

MapTileLoadResult readFake(void* opaque, MapTileLookupPolicy::LocalSource source) {
    FakeLocalSources* fake = static_cast<FakeLocalSources*>(opaque);
    fake->calls[fake->count++] = source == MapTileLookupPolicy::LocalSource::PACK ? 0 : 1;
    return source == MapTileLookupPolicy::LocalSource::PACK ? fake->pack : fake->live;
}

void testExecutableSourceOrderAndFallback() {
    beginTest();
    FakeLocalSources ready = {MapTileLoadResult::READY, MapTileLoadResult::IO_ERROR, {0, 0}, 0U};
    CHECK(MapTileLookupPolicy::readLocal(&ready, readFake) == MapTileLoadResult::READY);
    CHECK(ready.count == 1U && ready.calls[0] == 0);
    const MapTileLoadResult fallthrough[] = {
        MapTileLoadResult::MISS, MapTileLoadResult::INVALID_PNG,
        MapTileLoadResult::TOO_LARGE, MapTileLoadResult::IO_ERROR
    };
    for (std::size_t index = 0U; index < sizeof(fallthrough) / sizeof(fallthrough[0]); ++index) {
        FakeLocalSources fake = {fallthrough[index], MapTileLoadResult::READY, {0, 0}, 0U};
        CHECK(MapTileLookupPolicy::readLocal(&fake, readFake) == MapTileLoadResult::READY);
        CHECK(fake.count == 2U && fake.calls[0] == 0 && fake.calls[1] == 1);
    }
    CHECK(MapTileLookupPolicy::readLocal(NULL, NULL) == MapTileLoadResult::IO_ERROR);
}
}  // namespace

int main() {
    testLocalPrecedenceTable();
    testOnlineGate();
    testExecutableSourceOrderAndFallback();
    std::cout << "map tile lookup policy: " << tests_run << " tests passed\n";
    return 0;
}
