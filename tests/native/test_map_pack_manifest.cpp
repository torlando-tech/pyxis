#include "UI/LXMF/MapPackManifest.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using Hardware::TDeck::TileKey;
using Pyxis::MapPackManifest;
using Pyxis::ManifestResult;
using Pyxis::RowSpan;
using Pyxis::ZoomExtent;

namespace {
std::size_t tests_run = 0U;
void fail(const char* expression, int line) {
    std::cerr << "line " << line << ": " << expression << '\n';
    std::exit(1);
}
#define CHECK(expression) do { if (!(expression)) fail(#expression, __LINE__); } while (false)
void beginTest() { ++tests_run; }

void setText(char* output, std::size_t capacity, const char* text) {
    CHECK(std::strlen(text) < capacity);
    std::strcpy(output, text);
}

MapPackManifest sample() {
    MapPackManifest manifest = {};
    setText(manifest.pack_id, sizeof(manifest.pack_id), "west-coast_1");
    setText(manifest.name, sizeof(manifest.name), "West Coast");
    setText(manifest.attribution, sizeof(manifest.attribution), "Example Maps contributors");
    setText(manifest.source, sizeof(manifest.source), "local-example");
    setText(manifest.license, sizeof(manifest.license), "CC-BY-4.0");
    manifest.min_zoom = 2U;
    manifest.max_zoom = 3U;
    manifest.extent_count = 2U;
    manifest.tile_count = 12U;
    manifest.extents[0].zoom = 2U;
    manifest.extents[0].interval_count = 1U;
    manifest.extents[0].x[0].minimum = 1U;
    manifest.extents[0].x[0].maximum = 2U;
    manifest.extents[0].y_minimum = 1U;
    manifest.extents[0].y_maximum = 2U;
    manifest.extents[1].zoom = 3U;
    manifest.extents[1].interval_count = 2U;
    manifest.extents[1].x[0].minimum = 0U;
    manifest.extents[1].x[0].maximum = 1U;
    manifest.extents[1].x[1].minimum = 6U;
    manifest.extents[1].x[1].maximum = 7U;
    manifest.extents[1].y_minimum = 4U;
    manifest.extents[1].y_maximum = 5U;
    return manifest;
}

std::vector<std::uint8_t> encode(const MapPackManifest& manifest) {
    std::uint8_t storage[MapPackManifest::MAX_SERIALIZED_SIZE];
    std::size_t written = 0U;
    const ManifestResult result = MapPackManifest::serialize(manifest, storage, sizeof(storage), written);
    CHECK(result == ManifestResult::OK);
    return std::vector<std::uint8_t>(storage, storage + written);
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t length) {
    std::uint32_t crc = UINT32_C(0xffffffff);
    for (std::size_t index = 0U; index < length; ++index) {
        crc ^= data[index];
        for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? UINT32_C(0xedb88320) : 0U);
        }
    }
    return ~crc;
}
void putU32(std::uint8_t* output, std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
    output[2] = static_cast<std::uint8_t>(value >> 16U);
    output[3] = static_cast<std::uint8_t>(value >> 24U);
}
void refreshCrc(std::vector<std::uint8_t>& bytes) {
    putU32(&bytes[bytes.size() - 4U], crc32(&bytes[0], bytes.size() - 4U));
}

void testRoundTripAndCoverage() {
    beginTest();
    const MapPackManifest original = sample();
    const std::vector<std::uint8_t> bytes = encode(original);
    MapPackManifest parsed = {};
    CHECK(MapPackManifest::parse(&bytes[0], bytes.size(), parsed) == ManifestResult::OK);
    CHECK(std::strcmp(parsed.pack_id, original.pack_id) == 0);
    CHECK(std::strcmp(parsed.name, original.name) == 0);
    CHECK(std::strcmp(parsed.attribution, original.attribution) == 0);
    CHECK(std::strcmp(parsed.source, original.source) == 0);
    CHECK(std::strcmp(parsed.license, original.license) == 0);
    CHECK(parsed.tile_count == 12U);
    CHECK(parsed.covers(TileKey{2U, 1U, 1U}));
    CHECK(parsed.covers(TileKey{3U, 0U, 4U}));
    CHECK(parsed.covers(TileKey{3U, 7U, 5U}));
    CHECK(!parsed.covers(TileKey{3U, 3U, 4U}));
    CHECK(!parsed.covers(TileKey{3U, 7U, 6U}));
}

void testDeterministicBytesAndFixturePrefix() {
    beginTest();
    const std::vector<std::uint8_t> first = encode(sample());
    const std::vector<std::uint8_t> second = encode(sample());
    CHECK(first == second);
    const std::uint8_t fixture[] = {
        0x50U,0x4dU,0x50U,0x4bU,0x01U,0x00U,0x10U,0x00U,0x99U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,
        0x0cU,0x77U,0x65U,0x73U,0x74U,0x2dU,0x63U,0x6fU,0x61U,0x73U,0x74U,0x5fU,0x31U,0x0aU,0x57U,0x65U,
        0x73U,0x74U,0x20U,0x43U,0x6fU,0x61U,0x73U,0x74U,0x19U,0x45U,0x78U,0x61U,0x6dU,0x70U,0x6cU,0x65U,
        0x20U,0x4dU,0x61U,0x70U,0x73U,0x20U,0x63U,0x6fU,0x6eU,0x74U,0x72U,0x69U,0x62U,0x75U,0x74U,0x6fU,
        0x72U,0x73U,0x0dU,0x6cU,0x6fU,0x63U,0x61U,0x6cU,0x2dU,0x65U,0x78U,0x61U,0x6dU,0x70U,0x6cU,0x65U,
        0x09U,0x43U,0x43U,0x2dU,0x42U,0x59U,0x2dU,0x34U,0x2eU,0x30U,0x02U,0x03U,0x02U,0x0cU,0x00U,0x00U,
        0x00U,0x02U,0x01U,0x01U,0x00U,0x00U,0x00U,0x02U,0x00U,0x00U,0x00U,0x01U,0x00U,0x00U,0x00U,0x02U,
        0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x03U,0x02U,0x04U,0x00U,0x00U,
        0x00U,0x05U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x01U,0x00U,0x00U,0x00U,0x06U,0x00U,0x00U,
        0x00U,0x07U,0x00U,0x00U,0x00U,0xd5U,0x5fU,0xfdU,0x67U
    };
    CHECK(first.size() == sizeof(fixture));
    CHECK(std::memcmp(&first[0], fixture, sizeof(fixture)) == 0);
    const std::uint32_t encoded_length = static_cast<std::uint32_t>(first[8]) |
        (static_cast<std::uint32_t>(first[9]) << 8U) |
        (static_cast<std::uint32_t>(first[10]) << 16U) |
        (static_cast<std::uint32_t>(first[11]) << 24U);
    CHECK(encoded_length == first.size());
}

void testMagicVersionHeaderAndLengthRejected() {
    beginTest();
    const std::vector<std::uint8_t> valid = encode(sample());
    MapPackManifest output = {};
    for (std::size_t index = 0U; index < 4U; ++index) {
        std::vector<std::uint8_t> bad = valid; bad[index] ^= 1U;
        CHECK(MapPackManifest::parse(&bad[0], bad.size(), output) == ManifestResult::BAD_MAGIC);
    }
    std::vector<std::uint8_t> bad = valid; bad[4] = 3U; refreshCrc(bad);
    CHECK(MapPackManifest::parse(&bad[0], bad.size(), output) == ManifestResult::UNSUPPORTED_VERSION);
    bad = valid; bad[6] = 15U; refreshCrc(bad);
    CHECK(MapPackManifest::parse(&bad[0], bad.size(), output) == ManifestResult::BAD_HEADER);
    bad = valid; bad[8] ^= 1U; refreshCrc(bad);
    CHECK(MapPackManifest::parse(&bad[0], bad.size(), output) == ManifestResult::BAD_LENGTH);
}

void testCrcTruncationAndTrailingDataRejected() {
    beginTest();
    const std::vector<std::uint8_t> valid = encode(sample());
    MapPackManifest output = {};
    std::vector<std::uint8_t> bad = valid; bad[20] ^= 0x80U;
    CHECK(MapPackManifest::parse(&bad[0], bad.size(), output) == ManifestResult::BAD_CRC);
    for (std::size_t length = 0U; length < valid.size(); ++length) {
        const std::uint8_t* input = length == 0U ? static_cast<const std::uint8_t*>(0) : &valid[0];
        CHECK(MapPackManifest::parse(input, length, output) != ManifestResult::OK);
    }
    bad = valid; bad.push_back(0U);
    CHECK(MapPackManifest::parse(&bad[0], bad.size(), output) == ManifestResult::BAD_LENGTH);
}

void testBoundedStringsAndPackIdGrammar() {
    beginTest();
    MapPackManifest manifest = sample();
    std::memset(manifest.pack_id, 'a', sizeof(manifest.pack_id));
    std::uint8_t bytes[MapPackManifest::MAX_SERIALIZED_SIZE]; std::size_t written = 9U;
    CHECK(MapPackManifest::serialize(manifest, bytes, sizeof(bytes), written) == ManifestResult::INVALID_STRING);
    CHECK(written == 9U);
    manifest = sample(); setText(manifest.pack_id, sizeof(manifest.pack_id), "Bad/ID");
    CHECK(MapPackManifest::serialize(manifest, bytes, sizeof(bytes), written) == ManifestResult::INVALID_STRING);
    manifest = sample(); manifest.name[0] = '\0';
    CHECK(MapPackManifest::serialize(manifest, bytes, sizeof(bytes), written) == ManifestResult::INVALID_STRING);
    manifest = sample(); manifest.attribution[0] = static_cast<char>(0x80);
    manifest.attribution[1] = '\0';
    CHECK(MapPackManifest::serialize(manifest, bytes, sizeof(bytes), written) == ManifestResult::INVALID_STRING);
}

void testZoomAndExtentValidation() {
    beginTest();
    std::uint8_t bytes[MapPackManifest::MAX_SERIALIZED_SIZE]; std::size_t written = 0U;
    MapPackManifest manifest = sample(); manifest.max_zoom = 23U;
    CHECK(MapPackManifest::serialize(manifest, bytes, sizeof(bytes), written) == ManifestResult::INVALID_ZOOM);
    manifest = sample(); manifest.min_zoom = 3U; manifest.max_zoom = 2U;
    CHECK(MapPackManifest::serialize(manifest, bytes, sizeof(bytes), written) == ManifestResult::INVALID_ZOOM);
    manifest = sample(); manifest.extent_count = 1U;
    CHECK(MapPackManifest::serialize(manifest, bytes, sizeof(bytes), written) == ManifestResult::INVALID_EXTENT);
    manifest = sample(); manifest.extents[1].zoom = 2U;
    CHECK(MapPackManifest::serialize(manifest, bytes, sizeof(bytes), written) == ManifestResult::INVALID_EXTENT);
    manifest = sample(); manifest.extents[0].x[0].maximum = 4U;
    CHECK(MapPackManifest::serialize(manifest, bytes, sizeof(bytes), written) == ManifestResult::INVALID_EXTENT);
    manifest = sample(); manifest.extents[1].interval_count = 0U;
    CHECK(MapPackManifest::serialize(manifest, bytes, sizeof(bytes), written) == ManifestResult::INVALID_EXTENT);
    manifest = sample(); manifest.extents[1].x[1].minimum = 1U;
    CHECK(MapPackManifest::serialize(manifest, bytes, sizeof(bytes), written) == ManifestResult::INVALID_EXTENT);
}

void testTileCountValidationAndOverflowSafety() {
    beginTest();
    std::uint8_t bytes[MapPackManifest::MAX_SERIALIZED_SIZE]; std::size_t written = 0U;
    MapPackManifest manifest = sample(); manifest.tile_count = 11U;
    CHECK(MapPackManifest::serialize(manifest, bytes, sizeof(bytes), written) == ManifestResult::INVALID_TILE_COUNT);
    manifest = sample(); manifest.extent_count = 1U; manifest.min_zoom = 22U; manifest.max_zoom = 22U;
    ZoomExtent& extent = manifest.extents[0]; extent.zoom = 22U; extent.interval_count = 1U;
    extent.x[0].minimum = 0U; extent.x[0].maximum = 4194303U;
    extent.y_minimum = 0U; extent.y_maximum = 4194303U; manifest.tile_count = UINT32_MAX;
    CHECK(MapPackManifest::serialize(manifest, bytes, sizeof(bytes), written) == ManifestResult::INVALID_TILE_COUNT);
}

void testInvalidTileKeysNeverCovered() {
    beginTest();
    const MapPackManifest manifest = sample();
    CHECK(!manifest.covers(TileKey{23U, 0U, 0U}));
    CHECK(!manifest.covers(TileKey{2U, 4U, 0U}));
    CHECK(!manifest.covers(TileKey{2U, 0U, 4U}));
}

void testCapacityFailureLeavesOutputUntouched() {
    beginTest();
    const MapPackManifest manifest = sample();
    std::uint8_t output[32]; std::memset(output, 0xa5, sizeof(output));
    std::size_t written = 77U;
    CHECK(MapPackManifest::serialize(manifest, output, sizeof(output), written) == ManifestResult::INSUFFICIENT_CAPACITY);
    CHECK(written == 77U);
    for (std::size_t index = 0U; index < sizeof(output); ++index) CHECK(output[index] == 0xa5U);
}

void testParseFailureLeavesOutputUnchanged() {
    beginTest();
    const std::vector<std::uint8_t> valid = encode(sample());
    std::vector<std::uint8_t> bad = valid; bad[valid.size() - 1U] ^= 1U;
    MapPackManifest output = sample();
    std::uint8_t before[sizeof(output)]; std::memcpy(before, &output, sizeof(output));
    CHECK(MapPackManifest::parse(&bad[0], bad.size(), output) == ManifestResult::BAD_CRC);
    CHECK(std::memcmp(before, &output, sizeof(output)) == 0);

    bad = valid;
    const MapPackManifest fixture = sample();
    const std::size_t first_extent = 16U + 1U + std::strlen(fixture.pack_id) +
        1U + std::strlen(fixture.name) + 1U + std::strlen(fixture.attribution) +
        1U + std::strlen(fixture.source) + 1U + std::strlen(fixture.license) + 7U;
    putU32(&bad[first_extent - 4U], fixture.tile_count + 1U);
    refreshCrc(bad);
    CHECK(MapPackManifest::parse(&bad[0], bad.size(), output) == ManifestResult::INVALID_TILE_COUNT);
    CHECK(std::memcmp(before, &output, sizeof(output)) == 0);
}

void testMalformedPayloadWithValidCrcRejected() {
    beginTest();
    std::vector<std::uint8_t> bytes = encode(sample());
    bytes[16] = 32U; refreshCrc(bytes);
    MapPackManifest output = {};
    CHECK(MapPackManifest::parse(&bytes[0], bytes.size(), output) == ManifestResult::INVALID_STRING);
    bytes = encode(sample());
    const std::size_t first_extent = 16U + 1U + std::strlen(sample().pack_id) + 1U + std::strlen(sample().name) +
        1U + std::strlen(sample().attribution) + 1U + std::strlen(sample().source) + 1U + std::strlen(sample().license) + 7U;
    bytes[first_extent + 1U] = 3U; refreshCrc(bytes);
    CHECK(MapPackManifest::parse(&bytes[0], bytes.size(), output) == ManifestResult::INVALID_EXTENT);
}

void testDeterministicStressRoundTrips() {
    beginTest();
    for (std::uint8_t zoom = 0U; zoom <= MapPackManifest::MAX_ZOOM; ++zoom) {
        MapPackManifest manifest = sample(); manifest.min_zoom = zoom; manifest.max_zoom = zoom;
        manifest.extent_count = 1U; ZoomExtent& extent = manifest.extents[0]; extent = ZoomExtent();
        const std::uint32_t edge = (UINT32_C(1) << zoom) - 1U;
        extent.zoom = zoom; extent.interval_count = edge <= 1U ? 1U : 2U;
        extent.x[0].minimum = 0U; extent.x[0].maximum = edge <= 1U ? edge : 0U;
        extent.y_minimum = edge; extent.y_maximum = edge;
        extent.x[1].minimum = edge; extent.x[1].maximum = edge;
        if (extent.interval_count == 1U) { extent.x[1].minimum = 0U; extent.x[1].maximum = 0U; }
        manifest.tile_count = edge <= 1U ? edge + 1U : 2U;
        const std::vector<std::uint8_t> bytes = encode(manifest); MapPackManifest parsed = {};
        CHECK(MapPackManifest::parse(&bytes[0], bytes.size(), parsed) == ManifestResult::OK);
        CHECK(parsed.covers(TileKey{zoom, 0U, edge}));
        CHECK(parsed.covers(TileKey{zoom, edge, edge}));
    }
}

void testMaximumSerializedSizeRoundTrip() {
    beginTest();
    MapPackManifest manifest = {};
    std::memset(manifest.pack_id, 'a', sizeof(manifest.pack_id) - 1U);
    std::memset(manifest.name, 'n', sizeof(manifest.name) - 1U);
    std::memset(manifest.attribution, 'a', sizeof(manifest.attribution) - 1U);
    std::memset(manifest.source, 's', sizeof(manifest.source) - 1U);
    std::memset(manifest.license, 'l', sizeof(manifest.license) - 1U);
    manifest.min_zoom = 0U;
    manifest.max_zoom = MapPackManifest::MAX_ZOOM;
    manifest.extent_count = static_cast<std::uint8_t>(MapPackManifest::MAX_ZOOM_LEVELS);
    manifest.tile_count = static_cast<std::uint32_t>(MapPackManifest::MAX_ZOOM_LEVELS);
    for (std::size_t index = 0U; index < MapPackManifest::MAX_ZOOM_LEVELS; ++index) {
        ZoomExtent& extent = manifest.extents[index];
        extent.zoom = static_cast<std::uint8_t>(index);
        extent.interval_count = 1U;
        extent.x[0].minimum = 0U;
        extent.x[0].maximum = 0U;
        extent.y_minimum = 0U;
        extent.y_maximum = 0U;
    }
    const std::vector<std::uint8_t> bytes = encode(manifest);
    CHECK(bytes.size() <= MapPackManifest::MAX_SERIALIZED_SIZE);
    MapPackManifest parsed = {};
    CHECK(MapPackManifest::parse(&bytes[0], bytes.size(), parsed) == ManifestResult::OK);
    CHECK(parsed.tile_count == MapPackManifest::MAX_ZOOM_LEVELS);

    RowSpan spans[MapPackManifest::MAX_ROW_SPANS];
    manifest.min_zoom = 22U; manifest.max_zoom = 22U; manifest.extent_count = 0U;
    manifest.tile_count = static_cast<std::uint32_t>(MapPackManifest::MAX_ROW_SPANS);
    for (std::size_t index = 0U; index < MapPackManifest::MAX_ROW_SPANS; ++index) {
        spans[index].zoom = 22U;
        spans[index].y = static_cast<std::uint32_t>(index);
        spans[index].x_minimum = 0U;
        spans[index].x_maximum = 0U;
    }
    std::uint8_t sparse[MapPackManifest::MAX_SERIALIZED_SIZE];
    std::size_t sparse_size = 0U;
    CHECK(MapPackManifest::serializeSparse(manifest, spans, MapPackManifest::MAX_ROW_SPANS,
                                           sparse, sizeof(sparse), sparse_size) == ManifestResult::OK);
    CHECK(sparse_size == MapPackManifest::MAX_SERIALIZED_SIZE);
    CHECK(MapPackManifest::parse(sparse, sparse_size, parsed) == ManifestResult::OK);
    CHECK(parsed.row_span_count == MapPackManifest::MAX_ROW_SPANS);
}

void testSparseRowSpanRoundTripAndExactCoverage() {
    beginTest();
    MapPackManifest manifest = sample();
    manifest.min_zoom = 7U;
    manifest.max_zoom = 7U;
    manifest.extent_count = 0U;
    manifest.tile_count = 5U;
    const RowSpan spans[] = {
        {7U, 48U, 35U, 36U},
        {7U, 49U, 34U, 36U},
    };
    std::uint8_t storage[MapPackManifest::MAX_SERIALIZED_SIZE] = {};
    std::size_t written = 0U;
    CHECK(MapPackManifest::serializeSparse(manifest, spans, 2U, storage,
                                           sizeof(storage), written) == ManifestResult::OK);
    MapPackManifest parsed = {};
    CHECK(MapPackManifest::parse(storage, written, parsed) == ManifestResult::OK);
    CHECK(parsed.format_version == 2U);
    CHECK(parsed.row_span_count == 2U);
    CHECK(parsed.tile_count == 5U);
    CHECK(parsed.covers(TileKey{7U, 35U, 48U}));
    CHECK(parsed.covers(TileKey{7U, 34U, 49U}));
    CHECK(parsed.covers(TileKey{7U, 36U, 49U}));
    CHECK(!parsed.covers(TileKey{7U, 34U, 48U}));
    CHECK(!parsed.covers(TileKey{7U, 37U, 49U}));
}

void testSparseRowSpansMustBeCanonicalAndBounded() {
    beginTest();
    MapPackManifest manifest = sample();
    manifest.min_zoom = 7U;
    manifest.max_zoom = 7U;
    manifest.extent_count = 0U;
    manifest.tile_count = 2U;
    std::uint8_t storage[MapPackManifest::MAX_SERIALIZED_SIZE] = {};
    std::size_t written = 0U;
    const RowSpan adjacent[] = {{7U, 48U, 35U, 35U}, {7U, 48U, 36U, 36U}};
    CHECK(MapPackManifest::serializeSparse(manifest, adjacent, 2U, storage,
                                           sizeof(storage), written) == ManifestResult::INVALID_EXTENT);
    const RowSpan reversed[] = {{7U, 49U, 35U, 35U}, {7U, 48U, 35U, 35U}};
    CHECK(MapPackManifest::serializeSparse(manifest, reversed, 2U, storage,
                                           sizeof(storage), written) == ManifestResult::INVALID_EXTENT);
}
}  // namespace

int main() {
    testRoundTripAndCoverage();
    testDeterministicBytesAndFixturePrefix();
    testMagicVersionHeaderAndLengthRejected();
    testCrcTruncationAndTrailingDataRejected();
    testBoundedStringsAndPackIdGrammar();
    testZoomAndExtentValidation();
    testTileCountValidationAndOverflowSafety();
    testInvalidTileKeysNeverCovered();
    testCapacityFailureLeavesOutputUntouched();
    testParseFailureLeavesOutputUnchanged();
    testMalformedPayloadWithValidCrcRejected();
    testDeterministicStressRoundTrips();
    testMaximumSerializedSizeRoundTrip();
    testSparseRowSpanRoundTripAndExactCoverage();
    testSparseRowSpansMustBeCanonicalAndBounded();
    std::cout << "map pack manifest: " << tests_run << " tests passed\n";
    return 0;
}
