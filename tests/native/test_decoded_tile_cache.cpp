#include <cstddef>
#include <cstdint>
#include <iostream>

#include "UI/LXMF/DecodedTileCache.h"

namespace {
int passed = 0;
int failures = 0;
#define CHECK(expr) do { if (expr) { ++passed; } else { ++failures; std::cerr << "FAIL line " << __LINE__ << ": " #expr << '\n'; } } while (false)

Hardware::TDeck::TileKey key(std::uint8_t zoom, std::uint32_t x, std::uint32_t y) {
    Hardware::TDeck::TileKey value{};
    value.zoom = zoom;
    value.x = x;
    value.y = y;
    return value;
}

void missDoesNotMutateOutput() {
    Pyxis::DecodedTileCache cache(4U);
    std::uint16_t storage[4] = {};
    std::uint16_t output[4] = {9U, 9U, 9U, 9U};
    CHECK(cache.attach(0U, storage));
    CHECK(!cache.get(key(2U, 1U, 1U), output, 4U));
    CHECK(output[0] == 9U && output[3] == 9U);
    CHECK(cache.validCount() == 0U);
}

void putAndGetCopiesPixels() {
    Pyxis::DecodedTileCache cache(4U);
    std::uint16_t storage[4] = {};
    const std::uint16_t input[4] = {1U, 2U, 3U, 4U};
    std::uint16_t output[4] = {};
    CHECK(cache.attach(0U, storage));
    CHECK(cache.put(key(3U, 2U, 4U), input, 4U));
    CHECK(cache.get(key(3U, 2U, 4U), output, 4U));
    CHECK(output[0] == 1U && output[1] == 2U &&
          output[2] == 3U && output[3] == 4U);
    CHECK(cache.validCount() == 1U);
}

void leastRecentlyUsedAttachedEntryIsEvicted() {
    Pyxis::DecodedTileCache cache(2U);
    std::uint16_t buffers[3][2] = {};
    const std::uint16_t a[2] = {10U, 11U};
    const std::uint16_t b[2] = {20U, 21U};
    const std::uint16_t c[2] = {30U, 31U};
    std::uint16_t output[2] = {};
    for (std::size_t index = 0U; index < 3U; ++index) {
        CHECK(cache.attach(index, buffers[index]));
    }
    CHECK(cache.put(key(4U, 1U, 1U), a, 2U));
    CHECK(cache.put(key(4U, 2U, 2U), b, 2U));
    CHECK(cache.put(key(4U, 3U, 3U), c, 2U));
    CHECK(cache.get(key(4U, 1U, 1U), output, 2U));
    CHECK(cache.put(key(4U, 4U, 4U), a, 2U));
    CHECK(cache.get(key(4U, 1U, 1U), output, 2U));
    CHECK(!cache.get(key(4U, 2U, 2U), output, 2U));
    CHECK(cache.get(key(4U, 3U, 3U), output, 2U));
    CHECK(cache.get(key(4U, 4U, 4U), output, 2U));
    CHECK(cache.validCount() == 3U);
}

void duplicatePutRefreshesAndReplacesPixels() {
    Pyxis::DecodedTileCache cache(2U);
    std::uint16_t buffers[2][2] = {};
    const Hardware::TDeck::TileKey a_key = key(5U, 6U, 7U);
    const std::uint16_t first[2] = {1U, 2U};
    const std::uint16_t second[2] = {8U, 9U};
    std::uint16_t output[2] = {};
    CHECK(cache.attach(0U, buffers[0]));
    CHECK(cache.attach(1U, buffers[1]));
    CHECK(cache.put(a_key, first, 2U));
    CHECK(cache.put(a_key, second, 2U));
    CHECK(cache.validCount() == 1U);
    CHECK(cache.get(a_key, output, 2U));
    CHECK(output[0] == 8U && output[1] == 9U);
}

void invalidArgumentsFailClosed() {
    Pyxis::DecodedTileCache cache(2U);
    std::uint16_t buffer[2] = {};
    const std::uint16_t input[2] = {1U, 2U};
    CHECK(!cache.attach(Pyxis::DecodedTileCache::CAPACITY, buffer));
    CHECK(!cache.attach(0U, nullptr));
    CHECK(cache.attach(0U, buffer));
    CHECK(!cache.put(key(1U, 0U, 0U), input, 1U));
    CHECK(!cache.put(key(1U, 0U, 0U), nullptr, 2U));
    CHECK(!cache.get(key(1U, 0U, 0U), buffer, 1U));
    CHECK(cache.validCount() == 0U);
}
}

int main() {
    missDoesNotMutateOutput();
    putAndGetCopiesPixels();
    leastRecentlyUsedAttachedEntryIsEvicted();
    duplicatePutRefreshesAndReplacesPixels();
    invalidArgumentsFailClosed();
    std::cout << "decoded tile cache: " << passed << " passed, "
              << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
