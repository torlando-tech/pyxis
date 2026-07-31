#include "UI/LXMF/MapTileStreamReader.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

using Hardware::TDeck::TileKey;
using Pyxis::MapTileReadStream;
using Pyxis::MapTileStopSource;
using Pyxis::MapTileStreamReader;
using Pyxis::MapTileStreamResult;

namespace {
std::size_t tests_run = 0U;
void fail(const char* expression, int line) { std::cerr << "line " << line << ": " << expression << '\n'; std::exit(1); }
#define CHECK(expression) do { if (!(expression)) fail(#expression, __LINE__); } while (false)
void beginTest() { ++tests_run; }

class Stop final : public MapTileStopSource {
public:
    Stop() : requested(false) {}
    bool stopRequested() const override { return requested; }
    bool requested;
};

class Stream final : public MapTileReadStream {
public:
    Stream() : begin_result(MapTileStreamResult::OK), read_result(MapTileStreamResult::OK),
               declared(3U), position(0U), begin_calls(0U), read_calls(0U), end_calls(0U),
               zero_progress(false), oversized_count(false), stop(NULL) {
        bytes[0] = 1U; bytes[1] = 2U; bytes[2] = 3U;
    }
    MapTileStreamResult begin(const TileKey&, std::uint32_t& size) override {
        ++begin_calls; size = declared; return begin_result;
    }
    MapTileStreamResult read(std::uint8_t* output, std::size_t capacity,
                             std::size_t& count) override {
        ++read_calls;
        if (read_result != MapTileStreamResult::OK) { count = 0U; return read_result; }
        if (zero_progress) { count = 0U; return MapTileStreamResult::OK; }
        if (oversized_count) { count = capacity + 1U; return MapTileStreamResult::OK; }
        const std::size_t remaining = declared - position;
        count = remaining < capacity ? remaining : capacity;
        if (count != 0U) std::memcpy(output, bytes + position, count);
        position += count;
        if (stop != NULL) stop->requested = true;
        return MapTileStreamResult::OK;
    }
    void end() override { ++end_calls; }
    MapTileStreamResult begin_result;
    MapTileStreamResult read_result;
    std::uint32_t declared;
    std::size_t position;
    std::size_t begin_calls;
    std::size_t read_calls;
    std::size_t end_calls;
    bool zero_progress;
    bool oversized_count;
    Stop* stop;
    std::uint8_t bytes[8];
};

void testSuccessAndMiss() {
    beginTest(); Stop stop; Stream stream; std::uint8_t output[8] = {}; std::size_t total = 99U;
    CHECK(MapTileStreamReader::readExact(stream, stop, TileKey{1U,0U,0U}, output, sizeof(output), 2U, total) == MapTileStreamResult::OK);
    CHECK(total == 3U && output[0] == 1U && output[2] == 3U); CHECK(stream.end_calls == 1U);
    Stream miss; miss.begin_result = MapTileStreamResult::MISS; total = 99U;
    CHECK(MapTileStreamReader::readExact(miss, stop, TileKey{1U,0U,0U}, output, sizeof(output), 2U, total) == MapTileStreamResult::MISS);
    CHECK(total == 0U && miss.end_calls == 0U);
}

void testEveryOpenedFailureCloses() {
    beginTest(); std::uint8_t output[4] = {}; std::size_t total = 0U; Stop stop;
    Stream large; large.declared = 5U;
    CHECK(MapTileStreamReader::readExact(large, stop, TileKey{1U,0U,0U}, output, sizeof(output), 2U, total) == MapTileStreamResult::TOO_LARGE); CHECK(large.end_calls == 1U);
    Stream error; error.read_result = MapTileStreamResult::IO_ERROR;
    CHECK(MapTileStreamReader::readExact(error, stop, TileKey{1U,0U,0U}, output, sizeof(output), 2U, total) == MapTileStreamResult::IO_ERROR); CHECK(error.end_calls == 1U);
    Stream unavailable; unavailable.read_result = MapTileStreamResult::STORAGE_UNAVAILABLE;
    CHECK(MapTileStreamReader::readExact(unavailable, stop, TileKey{1U,0U,0U}, output, sizeof(output), 2U, total) == MapTileStreamResult::STORAGE_UNAVAILABLE); CHECK(unavailable.end_calls == 1U);
    Stream zero; zero.zero_progress = true;
    CHECK(MapTileStreamReader::readExact(zero, stop, TileKey{1U,0U,0U}, output, sizeof(output), 2U, total) == MapTileStreamResult::IO_ERROR); CHECK(zero.end_calls == 1U);
    Stream excessive; excessive.oversized_count = true;
    CHECK(MapTileStreamReader::readExact(excessive, stop, TileKey{1U,0U,0U}, output, sizeof(output), 2U, total) == MapTileStreamResult::IO_ERROR); CHECK(excessive.end_calls == 1U);
}

void testStopMidReadCloses() {
    beginTest(); Stop stop; Stream stream; stream.stop = &stop; std::uint8_t output[8] = {}; std::size_t total = 0U;
    CHECK(MapTileStreamReader::readExact(stream, stop, TileKey{1U,0U,0U}, output, sizeof(output), 1U, total) == MapTileStreamResult::IO_ERROR);
    CHECK(stream.read_calls == 1U && stream.end_calls == 1U);
}
}

int main() {
    testSuccessAndMiss(); testEveryOpenedFailureCloses(); testStopMidReadCloses();
    std::cout << "map tile stream reader: " << tests_run << " tests passed\n";
}
