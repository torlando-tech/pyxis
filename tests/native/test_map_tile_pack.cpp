#include "Hardware/TDeck/MapTilePack.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>

using Hardware::TDeck::MapTilePack;
using Hardware::TDeck::MapTilePackResult;
using Hardware::TDeck::MapTilePackStatus;
using Hardware::TDeck::MapTileStorage;
using Hardware::TDeck::TileKey;
using Hardware::TDeck::TileStoreResult;
using Pyxis::MapPackManifest;
using Pyxis::RowSpan;

namespace {
std::size_t allocations = 0U;
std::size_t tests_run = 0U;
void fail(const char* expression, int line) {
    std::cerr << "line " << line << ": " << expression << '\n';
    std::exit(1);
}
#define CHECK(expression) do { if (!(expression)) fail(#expression, __LINE__); } while (false)
void beginTest() { ++tests_run; }
}

void* operator new(std::size_t size) {
    ++allocations;
    void* memory = std::malloc(size);
    if (memory == NULL) throw std::bad_alloc();
    return memory;
}
void* operator new[](std::size_t size) {
    ++allocations;
    void* memory = std::malloc(size);
    if (memory == NULL) throw std::bad_alloc();
    return memory;
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }

namespace {
struct File {
    char path[MapTilePack::PATH_CAPACITY];
    std::uint8_t bytes[MapTilePack::ACTIVE_SELECTION_SIZE];
    std::size_t size;
    std::uint32_t declared_size;
};

class FakeStorage : public MapTileStorage {
public:
    FakeStorage()
        : available(true), file_count(0U), open_file(NULL), position(0U),
          read_calls(0U), end_calls(0U), fail_read_call(0U), zero_read_call(0U),
          fail_begin_path(NULL), fail_begin_result(TileStoreResult::IO_ERROR) {}

    void clear() { file_count = 0U; open_file = NULL; position = 0U; }
    void add(const char* path, const std::uint8_t* bytes, std::size_t size) {
        CHECK(file_count < 8U);
        CHECK(std::strlen(path) < sizeof(files[0].path));
        CHECK(size <= sizeof(files[0].bytes));
        File& file = files[file_count++];
        std::strcpy(file.path, path);
        if (size != 0U) std::memcpy(file.bytes, bytes, size);
        file.size = size;
        file.declared_size = static_cast<std::uint32_t>(size);
    }
    void addText(const char* path, const char* text) {
        add(path, reinterpret_cast<const std::uint8_t*>(text), std::strlen(text));
    }
    void setDeclaredSize(const char* path, std::uint32_t size) {
        File* file = find(path); CHECK(file != NULL); file->declared_size = size;
    }
    File* find(const char* path) {
        for (std::size_t index = 0U; index < file_count; ++index) {
            if (std::strcmp(files[index].path, path) == 0) return &files[index];
        }
        return NULL;
    }

    virtual bool isAvailable() const { return available; }
    virtual TileStoreResult beginRead(const char* path, std::uint32_t& size) {
        if (!available) return TileStoreResult::STORAGE_UNAVAILABLE;
        if (fail_begin_path != NULL && std::strcmp(path, fail_begin_path) == 0)
            return fail_begin_result;
        if (open_file != NULL) return TileStoreResult::BUSY;
        File* file = find(path);
        if (file == NULL) return TileStoreResult::MISS;
        open_file = file; position = 0U; read_calls = 0U; size = file->declared_size;
        return TileStoreResult::OK;
    }
    virtual TileStoreResult readChunk(std::uint8_t* output, std::size_t capacity, std::size_t& count) {
        count = 0U;
        if (open_file == NULL) return TileStoreResult::NOT_INITIALIZED;
        ++read_calls;
        if (fail_read_call != 0U && read_calls == fail_read_call) return TileStoreResult::IO_ERROR;
        if (zero_read_call != 0U && read_calls == zero_read_call) return TileStoreResult::OK;
        const std::size_t remaining = open_file->size - position;
        const std::size_t amount = remaining < capacity ? remaining : capacity;
        if (amount != 0U) std::memcpy(output, open_file->bytes + position, amount);
        position += amount; count = amount;
        return TileStoreResult::OK;
    }
    virtual void endRead() { if (open_file != NULL) { open_file = NULL; ++end_calls; } }
    virtual TileStoreResult beginWrite(const char*) { return TileStoreResult::IO_ERROR; }
    virtual TileStoreResult writeChunk(const std::uint8_t*, std::size_t, std::size_t&) { return TileStoreResult::IO_ERROR; }
    virtual TileStoreResult commitWrite() { return TileStoreResult::IO_ERROR; }
    virtual void abortWrite() {}
    virtual TileStoreResult remove(const char*) { return TileStoreResult::IO_ERROR; }
    virtual TileStoreResult rename(const char*, const char*) { return TileStoreResult::IO_ERROR; }
    virtual TileStoreResult stat(const char*, std::uint32_t&) { return TileStoreResult::IO_ERROR; }
    virtual TileStoreResult beginList() { return TileStoreResult::IO_ERROR; }
    virtual TileStoreResult nextList(char*, std::size_t, bool&) { return TileStoreResult::IO_ERROR; }
    virtual void endList() {}

    bool available;
    File files[8];
    std::size_t file_count;
    File* open_file;
    std::size_t position;
    std::size_t read_calls;
    std::size_t end_calls;
    std::size_t fail_read_call;
    std::size_t zero_read_call;
    const char* fail_begin_path;
    TileStoreResult fail_begin_result;
};

MapPackManifest manifestFor(const char* id) {
    MapPackManifest manifest = {};
    std::strcpy(manifest.pack_id, id);
    std::strcpy(manifest.name, "Test Pack");
    std::strcpy(manifest.attribution, "Test attribution");
    std::strcpy(manifest.source, "local-test");
    std::strcpy(manifest.license, "CC0");
    manifest.min_zoom = 2U; manifest.max_zoom = 2U; manifest.extent_count = 1U;
    manifest.tile_count = 4U;
    manifest.extents[0].zoom = 2U; manifest.extents[0].interval_count = 1U;
    manifest.extents[0].x[0].minimum = 1U; manifest.extents[0].x[0].maximum = 2U;
    manifest.extents[0].y_minimum = 1U; manifest.extents[0].y_maximum = 2U;
    return manifest;
}

void addSelection(FakeStorage& storage, const char* marker_id, const MapPackManifest& manifest) {
    storage.addText(MapTilePack::ACTIVE_PACK_PATH, marker_id);
    std::uint8_t bytes[MapPackManifest::MAX_SERIALIZED_SIZE];
    std::size_t written = 0U;
    CHECK(MapPackManifest::serialize(manifest, bytes, sizeof(bytes), written) == Pyxis::ManifestResult::OK);
    char path[MapTilePack::PATH_CAPACITY];
    CHECK(MapTilePack::manifestPath(marker_id, path, sizeof(path)) == MapTilePackResult::OK);
    storage.add(path, bytes, written);
}

std::uint32_t testCrc32(const std::uint8_t* input, std::size_t length) {
    std::uint32_t crc = UINT32_C(0xffffffff);
    for (std::size_t index = 0U; index < length; ++index) {
        crc ^= input[index];
        for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? UINT32_C(0xedb88320) : 0U);
        }
    }
    return ~crc;
}

void testPutU32(std::uint8_t* output, std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
    output[2] = static_cast<std::uint8_t>(value >> 16U);
    output[3] = static_cast<std::uint8_t>(value >> 24U);
}

void testPutU16(std::uint8_t* output, std::uint16_t value) {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
}

void addManifest(FakeStorage& storage, const char* id) {
    std::uint8_t bytes[MapPackManifest::MAX_SERIALIZED_SIZE];
    std::size_t written = 0U;
    CHECK(MapPackManifest::serialize(manifestFor(id), bytes, sizeof(bytes), written) == Pyxis::ManifestResult::OK);
    char path[MapTilePack::PATH_CAPACITY];
    CHECK(MapTilePack::manifestPath(id, path, sizeof(path)) == MapTilePackResult::OK);
    storage.add(path, bytes, written);
}

void addSparseManifest(FakeStorage& storage, const char* id, const char* attribution,
                       const char* source, const char* license,
                       const RowSpan* spans, std::size_t span_count) {
    MapPackManifest manifest = {};
    std::strcpy(manifest.pack_id, id); std::strcpy(manifest.name, "Test Pack");
    std::strcpy(manifest.attribution, attribution); std::strcpy(manifest.source, source);
    std::strcpy(manifest.license, license);
    manifest.min_zoom = spans[0].zoom; manifest.max_zoom = spans[span_count - 1U].zoom;
    std::uint32_t tile_count = 0U;
    for (std::size_t index = 0U; index < span_count; ++index) {
        tile_count += spans[index].x_maximum - spans[index].x_minimum + 1U;
    }
    manifest.tile_count = tile_count;
    std::uint8_t bytes[MapPackManifest::MAX_SERIALIZED_SIZE]; std::size_t written = 0U;
    CHECK(MapPackManifest::serializeSparse(manifest, spans, span_count, bytes,
        sizeof(bytes), written) == Pyxis::ManifestResult::OK);
    char path[MapTilePack::PATH_CAPACITY];
    CHECK(MapTilePack::manifestPath(id, path, sizeof(path)) == MapTilePackResult::OK);
    storage.add(path, bytes, written);
}

void addSlot(FakeStorage& storage, const char* path, const char* id, std::uint32_t generation,
             bool corrupt = false) {
    std::uint8_t record[MapTilePack::LEGACY_ACTIVE_SELECTION_SIZE] = {};
    record[0] = 'P'; record[1] = 'M'; record[2] = 'A'; record[3] = 'S'; record[4] = 1U;
    record[6] = static_cast<std::uint8_t>(MapTilePack::LEGACY_ACTIVE_SELECTION_SIZE);
    testPutU32(record + 8U, generation);
    const std::size_t length = std::strlen(id); CHECK(length > 0U && length < 32U);
    record[12] = static_cast<std::uint8_t>(length); std::memcpy(record + 13U, id, length);
    testPutU32(record + 44U, testCrc32(record, 44U));
    if (corrupt) record[47] ^= 1U;
    storage.add(path, record, sizeof(record));
}

void addMapSetSlot(FakeStorage& storage, const char* path, std::uint32_t generation,
                   const char* map_set = "osm-bright",
                   const char* attribution = "Map data (c) OpenStreetMap contributors") {
    std::uint8_t record[256] = {};
    record[0] = 'P'; record[1] = 'M'; record[2] = 'A'; record[3] = 'S'; record[4] = 2U;
    testPutU32(record + 8U, generation);
    std::size_t position = 12U;
    record[position++] = static_cast<std::uint8_t>(std::strlen(map_set));
    std::memcpy(record + position, map_set, std::strlen(map_set)); position += std::strlen(map_set);
    record[position++] = static_cast<std::uint8_t>(std::strlen(attribution));
    std::memcpy(record + position, attribution, std::strlen(attribution)); position += std::strlen(attribution);
    record[position++] = 2U;
    const char* ids[2] = {"detail", "state"};
    const RowSpan detail[] = {{2U, 1U, 1U, 1U}, {3U, 5U, 5U, 5U}};
    const RowSpan state[] = {{2U, 1U, 1U, 2U}};
    const RowSpan* spans[2] = {detail, state};
    const std::uint16_t counts[2] = {2U, 1U};
    for (std::size_t pack = 0U; pack < 2U; ++pack) {
        const std::size_t id_length = std::strlen(ids[pack]);
        record[position++] = static_cast<std::uint8_t>(id_length);
        std::memcpy(record + position, ids[pack], id_length); position += id_length;
        testPutU16(record + position, counts[pack]); position += 2U;
        for (std::uint16_t index = 0U; index < counts[pack]; ++index) {
            record[position++] = spans[pack][index].zoom;
            testPutU32(record + position, spans[pack][index].y); position += 4U;
            testPutU32(record + position, spans[pack][index].x_minimum); position += 4U;
            testPutU32(record + position, spans[pack][index].x_maximum); position += 4U;
        }
    }
    const std::size_t total_length = position + 4U;
    testPutU16(record + 6U, static_cast<std::uint16_t>(total_length));
    testPutU32(record + position, testCrc32(record, position));
    storage.add(path, record, total_length);
}

void addLegacyBrightMapSetManifests(FakeStorage& storage) {
    const RowSpan detail[] = {{2U, 1U, 1U, 1U}, {3U, 5U, 5U, 5U}};
    const RowSpan state[] = {{2U, 1U, 1U, 2U}};
    const char* attribution = "Map data (c) OpenStreetMap contributors";
    addSparseManifest(storage, "detail", attribution,
                      "Coalition MUI OSM Bright user download", "ODbL-1.0", detail, 2U);
    addSparseManifest(storage, "state", attribution,
                      "Coalition MUI OSM Bright user download", "ODbL-1.0", state, 1U);
}

void testNoSelection() {
    beginTest(); FakeStorage storage; MapTilePack pack(storage);
    CHECK(pack.initialize() == MapTilePackResult::NO_SELECTION);
    CHECK(pack.status() == MapTilePackStatus::NO_SELECTION); CHECK(!pack.hasSelection());
    storage.add(MapTilePack::ACTIVE_PACK_PATH, NULL, 0U);
    CHECK(pack.initialize() == MapTilePackResult::NO_SELECTION);
}
void testInvalidAndTraversalIds() {
    beginTest(); const char* bad[] = {"../bad", "bad/id", "UPPER", "a.b", "abcdefghijklmnopqrstuvwxyzabcdef"};
    for (std::size_t index = 0U; index < sizeof(bad) / sizeof(bad[0]); ++index) {
        FakeStorage storage; storage.addText(MapTilePack::ACTIVE_PACK_PATH, bad[index]); MapTilePack pack(storage);
        CHECK(pack.initialize() == MapTilePackResult::INVALID_PACK_ID);
        CHECK(pack.status() == MapTilePackStatus::INVALID_SELECTION);
    }
}
void testMissingManifest() {
    beginTest(); FakeStorage storage; storage.addText(MapTilePack::ACTIVE_PACK_PATH, "one"); MapTilePack pack(storage);
    CHECK(pack.initialize() == MapTilePackResult::MANIFEST_MISSING);
    CHECK(pack.status() == MapTilePackStatus::INVALID_SELECTION);
}
void testCorruptAndOversizedManifest() {
    beginTest();
    FakeStorage corrupt; corrupt.addText(MapTilePack::ACTIVE_PACK_PATH, "one"); corrupt.addText("/pyxis-map/packs/one/manifest.pmp", "bad");
    MapTilePack first(corrupt); CHECK(first.initialize() == MapTilePackResult::INVALID_MANIFEST); CHECK(corrupt.end_calls == 2U);
    FakeStorage large; large.addText(MapTilePack::ACTIVE_PACK_PATH, "one"); large.addText("/pyxis-map/packs/one/manifest.pmp", "x");
    large.setDeclaredSize("/pyxis-map/packs/one/manifest.pmp", static_cast<std::uint32_t>(MapPackManifest::MAX_SERIALIZED_SIZE + 1U));
    MapTilePack second(large); CHECK(second.initialize() == MapTilePackResult::MANIFEST_TOO_LARGE); CHECK(large.end_calls == 2U);
}
void testMarkerManifestMismatch() {
    beginTest(); FakeStorage storage; addSelection(storage, "one", manifestFor("two")); MapTilePack pack(storage);
    CHECK(pack.initialize() == MapTilePackResult::PACK_ID_MISMATCH); CHECK(!pack.hasSelection());
}
void testUncoveredKey() {
    beginTest(); FakeStorage storage; addSelection(storage, "one", manifestFor("one")); MapTilePack pack(storage);
    CHECK(pack.initialize() == MapTilePackResult::OK); std::uint32_t size = 99U;
    CHECK(pack.beginGet(TileKey{2U, 0U, 0U}, size) == MapTilePackResult::UNCOVERED); CHECK(size == 99U);
}
void testCoveredMissingFile() {
    beginTest(); FakeStorage storage; addSelection(storage, "one", manifestFor("one")); MapTilePack pack(storage);
    CHECK(pack.initialize() == MapTilePackResult::OK); std::uint32_t size = 0U;
    CHECK(pack.beginGet(TileKey{2U, 1U, 1U}, size) == MapTilePackResult::TILE_MISSING);
}
void testCanonicalPathsAndCapacity() {
    beginTest(); char path[MapTilePack::PATH_CAPACITY]; const char* id = "abcdefghijklmnopqrstuvwxyz12345";
    CHECK(MapTilePack::manifestPath(id, path, sizeof(path)) == MapTilePackResult::OK);
    CHECK(std::strcmp(path, "/pyxis-map/packs/abcdefghijklmnopqrstuvwxyz12345/manifest.pmp") == 0);
    CHECK(MapTilePack::tilePath(id, TileKey{22U, 4194303U, 4194303U}, path, sizeof(path)) == MapTilePackResult::OK);
    CHECK(std::strcmp(path, "/pyxis-map/packs/abcdefghijklmnopqrstuvwxyz12345/tiles/22/4194303/4194303.png") == 0);
    char short_path[77]; std::memset(short_path, 'q', sizeof(short_path));
    CHECK(MapTilePack::tilePath(id, TileKey{22U, 4194303U, 4194303U}, short_path, sizeof(short_path)) == MapTilePackResult::PATH_TOO_LONG);
    CHECK(short_path[0] == 'q');
    CHECK(MapTilePack::tilePath("../x", TileKey{2U, 1U, 1U}, path, sizeof(path)) == MapTilePackResult::INVALID_PACK_ID);
}
void testStorageUnavailable() {
    beginTest(); FakeStorage storage; storage.available = false; MapTilePack pack(storage);
    CHECK(pack.initialize() == MapTilePackResult::STORAGE_UNAVAILABLE); CHECK(pack.status() == MapTilePackStatus::STORAGE_UNAVAILABLE);
}
void testChunkReadAndAutomaticEnd() {
    beginTest(); FakeStorage storage; addSelection(storage, "one", manifestFor("one"));
    const std::uint8_t tile[] = {1U, 2U, 3U}; storage.add("/pyxis-map/packs/one/tiles/2/1/1.png", tile, sizeof(tile));
    MapTilePack pack(storage); CHECK(pack.initialize() == MapTilePackResult::OK); const std::size_t before = storage.end_calls;
    std::uint32_t size = 0U; CHECK(pack.beginGet(TileKey{2U, 1U, 1U}, size) == MapTilePackResult::OK); CHECK(size == 3U);
    std::uint8_t output[2] = {}; std::size_t count = 0U;
    CHECK(pack.readGetChunk(output, sizeof(output), count) == MapTilePackResult::OK); CHECK(count == 2U && output[0] == 1U && output[1] == 2U);
    CHECK(pack.readGetChunk(output, sizeof(output), count) == MapTilePackResult::OK); CHECK(count == 1U && output[0] == 3U);
    CHECK(storage.end_calls == before + 1U);
    CHECK(pack.readGetChunk(output, sizeof(output), count) == MapTilePackResult::NOT_STREAMING); CHECK(count == 0U);
}
void testReadErrorClosesAndArgumentsAreBounded() {
    beginTest(); FakeStorage storage; addSelection(storage, "one", manifestFor("one")); const std::uint8_t tile[] = {1U};
    storage.add("/pyxis-map/packs/one/tiles/2/1/1.png", tile, sizeof(tile)); MapTilePack pack(storage); CHECK(pack.initialize() == MapTilePackResult::OK);
    std::uint32_t size = 0U; CHECK(pack.beginGet(TileKey{2U, 1U, 1U}, size) == MapTilePackResult::OK); storage.fail_read_call = 1U;
    std::uint8_t output = 0U; std::size_t count = 55U; const std::size_t before = storage.end_calls;
    CHECK(pack.readGetChunk(&output, 1U, count) == MapTilePackResult::IO_ERROR); CHECK(count == 0U); CHECK(storage.end_calls == before + 1U);
    CHECK(pack.readGetChunk(&output, 1U, count) == MapTilePackResult::NOT_STREAMING);
    storage.fail_read_call = 0U;
    CHECK(pack.beginGet(TileKey{2U, 1U, 1U}, size) == MapTilePackResult::OK);
    const std::size_t argument_before = storage.end_calls;
    CHECK(pack.readGetChunk(NULL, 1U, count) == MapTilePackResult::INVALID_ARGUMENT);
    CHECK(storage.end_calls == argument_before + 1U);
    CHECK(pack.readGetChunk(&output, 1U, count) == MapTilePackResult::NOT_STREAMING);
}
void testPrematureEndCloses() {
    beginTest(); FakeStorage storage; addSelection(storage, "one", manifestFor("one")); const std::uint8_t tile[] = {1U, 2U};
    storage.add("/pyxis-map/packs/one/tiles/2/1/1.png", tile, sizeof(tile)); MapTilePack pack(storage); CHECK(pack.initialize() == MapTilePackResult::OK);
    std::uint32_t size = 0U; CHECK(pack.beginGet(TileKey{2U, 1U, 1U}, size) == MapTilePackResult::OK); storage.zero_read_call = 1U;
    std::uint8_t output[2]; std::size_t count = 0U; const std::size_t before = storage.end_calls;
    CHECK(pack.readGetChunk(output, sizeof(output), count) == MapTilePackResult::IO_ERROR); CHECK(storage.end_calls == before + 1U);
}
void testReinitializeSelectionChange() {
    beginTest(); FakeStorage storage; addSelection(storage, "one", manifestFor("one")); MapTilePack pack(storage);
    CHECK(pack.initialize() == MapTilePackResult::OK); CHECK(std::strcmp(pack.metadata().pack_id, "one") == 0);
    storage.clear(); addSelection(storage, "two", manifestFor("two"));
    CHECK(pack.initialize() == MapTilePackResult::OK); CHECK(std::strcmp(pack.metadata().pack_id, "two") == 0);
}
void testFailedReinitializeIsTransactional() {
    beginTest(); FakeStorage storage; addSelection(storage, "one", manifestFor("one")); MapTilePack pack(storage);
    CHECK(pack.initialize() == MapTilePackResult::OK); storage.clear(); storage.addText(MapTilePack::ACTIVE_PACK_PATH, "bad/../id");
    CHECK(pack.initialize() == MapTilePackResult::INVALID_PACK_ID); CHECK(pack.status() == MapTilePackStatus::READY);
    CHECK(pack.hasSelection()); CHECK(std::strcmp(pack.metadata().pack_id, "one") == 0);
}
void testReinitializeAndDestructorCloseStreams() {
    beginTest(); FakeStorage storage; addSelection(storage, "one", manifestFor("one")); const std::uint8_t tile[] = {1U};
    storage.add("/pyxis-map/packs/one/tiles/2/1/1.png", tile, sizeof(tile));
    {
        MapTilePack pack(storage); CHECK(pack.initialize() == MapTilePackResult::OK); std::uint32_t size = 0U;
        CHECK(pack.beginGet(TileKey{2U, 1U, 1U}, size) == MapTilePackResult::OK); const std::size_t before = storage.end_calls;
        CHECK(pack.initialize() == MapTilePackResult::OK); CHECK(storage.end_calls > before);
        CHECK(pack.beginGet(TileKey{2U, 1U, 1U}, size) == MapTilePackResult::OK);
    }
    CHECK(storage.open_file == NULL);
}
void testDeclaredLengthsAndEmbeddedNulFailClosed() {
    beginTest();
    FakeStorage marker_storage;
    const std::uint8_t bad_marker[] = {'o', 'n', 'e', 0U, 'x'};
    marker_storage.add(MapTilePack::ACTIVE_PACK_PATH, bad_marker, sizeof(bad_marker));
    MapTilePack marker_pack(marker_storage);
    CHECK(marker_pack.initialize() == MapTilePackResult::INVALID_PACK_ID);

    FakeStorage short_marker;
    addSelection(short_marker, "one", manifestFor("one"));
    short_marker.setDeclaredSize(MapTilePack::ACTIVE_PACK_PATH, 2U);
    MapTilePack short_marker_pack(short_marker);
    CHECK(short_marker_pack.initialize() == MapTilePackResult::IO_ERROR);

    FakeStorage trailing_manifest;
    addSelection(trailing_manifest, "one", manifestFor("one"));
    File* manifest = trailing_manifest.find("/pyxis-map/packs/one/manifest.pmp"); CHECK(manifest != NULL);
    CHECK(manifest->size + 1U <= sizeof(manifest->bytes));
    manifest->bytes[manifest->size++] = 0xa5U;
    MapTilePack trailing_manifest_pack(trailing_manifest);
    CHECK(trailing_manifest_pack.initialize() == MapTilePackResult::IO_ERROR);
}
void testTileDeclaredLengthCapsWritesAndProbesEof() {
    beginTest(); FakeStorage storage; addSelection(storage, "one", manifestFor("one"));
    const std::uint8_t tile[] = {1U, 2U};
    storage.add("/pyxis-map/packs/one/tiles/2/1/1.png", tile, sizeof(tile));
    storage.setDeclaredSize("/pyxis-map/packs/one/tiles/2/1/1.png", 1U);
    MapTilePack pack(storage); CHECK(pack.initialize() == MapTilePackResult::OK);
    std::uint32_t size = 0U; CHECK(pack.beginGet(TileKey{2U, 1U, 1U}, size) == MapTilePackResult::OK); CHECK(size == 1U);
    std::uint8_t output[2] = {0xa5U, 0xa5U}; std::size_t count = 99U;
    CHECK(pack.readGetChunk(output, sizeof(output), count) == MapTilePackResult::IO_ERROR);
    CHECK(count == 0U); CHECK(output[1] == 0xa5U); CHECK(storage.open_file == NULL);

    FakeStorage empty_storage; addSelection(empty_storage, "one", manifestFor("one"));
    empty_storage.add("/pyxis-map/packs/one/tiles/2/1/1.png", NULL, 0U);
    MapTilePack empty_pack(empty_storage); CHECK(empty_pack.initialize() == MapTilePackResult::OK);
    size = 99U; CHECK(empty_pack.beginGet(TileKey{2U, 1U, 1U}, size) == MapTilePackResult::OK); CHECK(size == 0U);
    count = 99U; CHECK(empty_pack.readGetChunk(output, sizeof(output), count) == MapTilePackResult::OK);
    CHECK(count == 0U); CHECK(empty_storage.open_file == NULL);
}
void testActiveSelectionSlotsPreferNewestValidAndRejectConflict() {
    beginTest();
    FakeStorage storage;
    addSlot(storage, MapTilePack::ACTIVE_PACK_SLOT_0_PATH, "one", 1U);
    addSlot(storage, MapTilePack::ACTIVE_PACK_SLOT_1_PATH, "two", 2U);
    addManifest(storage, "one"); addManifest(storage, "two");
    MapTilePack pack(storage); CHECK(pack.initialize() == MapTilePackResult::OK);
    CHECK(std::strcmp(pack.metadata().pack_id, "two") == 0);
    CHECK(pack.selectionGeneration() == 2U);

    FakeStorage fallback;
    addSlot(fallback, MapTilePack::ACTIVE_PACK_SLOT_0_PATH, "one", 1U);
    addSlot(fallback, MapTilePack::ACTIVE_PACK_SLOT_1_PATH, "two", 2U, true);
    addManifest(fallback, "one");
    MapTilePack fallback_pack(fallback); CHECK(fallback_pack.initialize() == MapTilePackResult::OK);
    CHECK(std::strcmp(fallback_pack.metadata().pack_id, "one") == 0);

    FakeStorage conflict;
    addSlot(conflict, MapTilePack::ACTIVE_PACK_SLOT_0_PATH, "one", 7U);
    addSlot(conflict, MapTilePack::ACTIVE_PACK_SLOT_1_PATH, "two", 7U);
    MapTilePack conflict_pack(conflict);
    CHECK(conflict_pack.initialize() == MapTilePackResult::INVALID_PACK_ID);
}
void testEqualGenerationIdentityUsesLengthAndExactBytesNotTrailingChecksum() {
    beginTest();
    std::uint8_t first[MapTilePack::LEGACY_ACTIVE_SELECTION_SIZE] = {};
    std::uint8_t second[MapTilePack::LEGACY_ACTIVE_SELECTION_SIZE] = {};
    FakeStorage records;
    addSlot(records, "first", "one", 7U); addSlot(records, "second", "two", 7U);
    std::memcpy(first, records.find("first")->bytes, sizeof(first));
    std::memcpy(second, records.find("second")->bytes, sizeof(second));
    std::memcpy(second + sizeof(second) - 4U, first + sizeof(first) - 4U, 4U);
    CHECK(!MapTilePack::selectionRecordsEqual(first, sizeof(first), second, sizeof(second)));
    CHECK(MapTilePack::selectionRecordsEqual(first, sizeof(first), first, sizeof(first)));
    CHECK(!MapTilePack::selectionRecordsEqual(first, sizeof(first), first, sizeof(first) - 1U));
}
void testActiveMapSetRequiresAllowlistedStyleAndMatchingImmutableManifests() {
    beginTest();
    FakeStorage valid;
    addMapSetSlot(valid, MapTilePack::ACTIVE_PACK_SLOT_0_PATH, 1U);
    addLegacyBrightMapSetManifests(valid);
    MapTilePack valid_pack(valid); CHECK(valid_pack.initialize() == MapTilePackResult::OK);

    FakeStorage unknown;
    addMapSetSlot(unknown, MapTilePack::ACTIVE_PACK_SLOT_0_PATH, 1U, "rogue");
    addLegacyBrightMapSetManifests(unknown);
    MapTilePack unknown_pack(unknown); CHECK(unknown_pack.initialize() != MapTilePackResult::OK);

    const RowSpan detail[] = {{2U, 1U, 1U, 1U}, {3U, 5U, 5U, 5U}};
    const RowSpan state[] = {{2U, 1U, 1U, 2U}};
    FakeStorage altered_attribution;
    addMapSetSlot(altered_attribution, MapTilePack::ACTIVE_PACK_SLOT_0_PATH, 1U);
    addSparseManifest(altered_attribution, "detail", "changed attribution",
                      "Coalition MUI OSM Bright user download", "ODbL-1.0", detail, 2U);
    addSparseManifest(altered_attribution, "state", "changed attribution",
                      "Coalition MUI OSM Bright user download", "ODbL-1.0", state, 1U);
    MapTilePack attribution_pack(altered_attribution);
    CHECK(attribution_pack.initialize() != MapTilePackResult::OK);

    FakeStorage wrong_coverage;
    addMapSetSlot(wrong_coverage, MapTilePack::ACTIVE_PACK_SLOT_0_PATH, 1U);
    const RowSpan changed_detail[] = {{2U, 1U, 1U, 2U}, {3U, 5U, 5U, 5U}};
    addSparseManifest(wrong_coverage, "detail", "Map data (c) OpenStreetMap contributors",
                      "Coalition MUI OSM Bright user download", "ODbL-1.0", changed_detail, 2U);
    addSparseManifest(wrong_coverage, "state", "Map data (c) OpenStreetMap contributors",
                      "Coalition MUI OSM Bright user download", "ODbL-1.0", state, 1U);
    MapTilePack coverage_pack(wrong_coverage); CHECK(coverage_pack.initialize() != MapTilePackResult::OK);
}
void testNewCanonicalProfilesPassAndCrossStyleCompositionFails() {
    beginTest();
    const RowSpan detail[] = {{2U, 1U, 1U, 1U}, {3U, 5U, 5U, 5U}};
    const RowSpan state[] = {{2U, 1U, 1U, 2U}};
    const char* styles[] = {"osm-bright", "dark-matter", "positron", "toner"};
    const char* labels[] = {"OSM Bright", "Dark Matter", "Positron", "Toner"};
    const char* attributions[] = {
        "(c) OpenMapTiles (c) OpenStreetMap contributors",
        "(c) OpenMapTiles (c) OpenStreetMap contributors; style (c) CARTO",
        "(c) OpenMapTiles (c) OpenStreetMap contributors; style (c) CARTO",
        "(c) MapTiler (c) OpenStreetMap contributors"};
    const char* licenses[] = {
        "OSM ODbL; style CC-BY-4.0/BSD-3-Clause",
        "OSM ODbL; style CC-BY-4.0/BSD-3-Clause (CARTO CC-BY-3.0)",
        "OSM ODbL; style CC-BY-4.0/BSD-3-Clause (CARTO CC-BY-3.0)",
        "OSM ODbL; style CC-BY-4.0/BSD-3-Clause (Stamen ISC)"};
    for (std::size_t index = 0U; index < 4U; ++index) {
        FakeStorage storage;
        addMapSetSlot(storage, MapTilePack::ACTIVE_PACK_SLOT_0_PATH, 1U,
                      styles[index], attributions[index]);
        char source[128] = {}; std::snprintf(source, sizeof(source),
            "Oxed's Map Tile Downloader (%s)", labels[index]);
        addSparseManifest(storage, "detail", attributions[index], source, licenses[index], detail, 2U);
        addSparseManifest(storage, "state", attributions[index], source, licenses[index], state, 1U);
        MapTilePack pack(storage); CHECK(pack.initialize() == MapTilePackResult::OK);
    }

    FakeStorage mixed;
    addMapSetSlot(mixed, MapTilePack::ACTIVE_PACK_SLOT_0_PATH, 1U, "dark-matter", attributions[1]);
    addSparseManifest(mixed, "detail", attributions[1],
                      "Oxed's Map Tile Downloader (Positron)", licenses[2], detail, 2U);
    addSparseManifest(mixed, "state", attributions[1],
                      "Oxed's Map Tile Downloader (Dark Matter)", licenses[1], state, 1U);
    MapTilePack mixed_pack(mixed); CHECK(mixed_pack.initialize() != MapTilePackResult::OK);
}
void testActiveMapSetComposesPacksByPriorityAndCoverage() {
    beginTest();
    FakeStorage storage;
    addMapSetSlot(storage, MapTilePack::ACTIVE_PACK_SLOT_0_PATH, 1U);
    addLegacyBrightMapSetManifests(storage);
    const std::uint8_t detail = 1U, state_overlap = 2U, state_wide = 3U, detail_high = 4U;
    storage.add("/pyxis-map/packs/detail/tiles/2/1/1.png", &detail, 1U);
    storage.add("/pyxis-map/packs/detail/tiles/3/5/5.png", &detail_high, 1U);
    storage.add("/pyxis-map/packs/state/tiles/2/1/1.png", &state_overlap, 1U);
    storage.add("/pyxis-map/packs/state/tiles/2/2/1.png", &state_wide, 1U);
    MapTilePack pack(storage); CHECK(pack.initialize() == MapTilePackResult::OK);
    CHECK(std::strcmp(pack.metadata().pack_id, "osm-bright") == 0);
    CHECK(pack.selectionGeneration() == 1U);
    CHECK(std::strcmp(pack.metadata().attribution, "Map data (c) OpenStreetMap contributors") == 0);
    const TileKey keys[] = {{2U,1U,1U},{2U,2U,1U},{3U,5U,5U}};
    const std::uint8_t expected[] = {detail,state_wide,detail_high};
    for (std::size_t index = 0U; index < 3U; ++index) {
        std::uint32_t size = 0U; CHECK(pack.beginGet(keys[index], size) == MapTilePackResult::OK); CHECK(size == 1U);
        std::uint8_t output = 0U; std::size_t count = 0U;
        CHECK(pack.readGetChunk(&output, 1U, count) == MapTilePackResult::OK); CHECK(count == 1U); CHECK(output == expected[index]);
    }
    std::uint32_t size = 0U;
    CHECK(pack.beginGet(TileKey{4U,1U,1U}, size) == MapTilePackResult::UNCOVERED);

    FakeStorage fallback;
    addMapSetSlot(fallback, MapTilePack::ACTIVE_PACK_SLOT_0_PATH, 1U);
    addLegacyBrightMapSetManifests(fallback);
    fallback.add("/pyxis-map/packs/state/tiles/2/1/1.png", &state_overlap, 1U);
    MapTilePack fallback_pack(fallback); CHECK(fallback_pack.initialize() == MapTilePackResult::OK);
    CHECK(fallback_pack.beginGet(TileKey{2U,1U,1U}, size) == MapTilePackResult::OK);
    std::uint8_t output = 0U; std::size_t count = 0U;
    CHECK(fallback_pack.readGetChunk(&output, 1U, count) == MapTilePackResult::OK);
    CHECK(output == state_overlap);
}
void testRebootFallsBackFromNewerSemanticallyInvalidMapSet() {
    beginTest(); FakeStorage storage;
    addMapSetSlot(storage, MapTilePack::ACTIVE_PACK_SLOT_0_PATH, 4U);
    addMapSetSlot(storage, MapTilePack::ACTIVE_PACK_SLOT_1_PATH, 5U, "dark-matter",
                  "(c) OpenMapTiles (c) OpenStreetMap contributors; style (c) CARTO");
    addLegacyBrightMapSetManifests(storage);
    const std::uint8_t tile = 0x5aU;
    storage.add("/pyxis-map/packs/detail/tiles/2/1/1.png", &tile, 1U);
    MapTilePack pack(storage); CHECK(pack.initialize() == MapTilePackResult::OK);
    CHECK(pack.selectionGeneration() == 4U);
    CHECK(std::strcmp(pack.metadata().pack_id, "osm-bright") == 0);
    std::uint32_t size = 0U; CHECK(pack.beginGet(TileKey{2U,1U,1U}, size) == MapTilePackResult::OK);
    std::uint8_t output = 0U; std::size_t count = 0U;
    CHECK(pack.readGetChunk(&output, 1U, count) == MapTilePackResult::OK);
    CHECK(count == 1U && output == tile);
}
void testNewerManifestIndeterminateDoesNotFallBackAndPreservesSelection() {
    beginTest(); FakeStorage storage;
    addMapSetSlot(storage, MapTilePack::ACTIVE_PACK_SLOT_0_PATH, 4U);
    addLegacyBrightMapSetManifests(storage);
    const std::uint8_t tile = 0x6bU;
    storage.add("/pyxis-map/packs/detail/tiles/2/1/1.png", &tile, 1U);
    MapTilePack pack(storage); CHECK(pack.initialize() == MapTilePackResult::OK);
    addMapSetSlot(storage, MapTilePack::ACTIVE_PACK_SLOT_1_PATH, 5U, "dark-matter",
                  "(c) OpenMapTiles (c) OpenStreetMap contributors; style (c) CARTO");
    storage.fail_begin_path = "/pyxis-map/packs/detail/manifest.pmp";
    storage.fail_begin_result = TileStoreResult::BUSY;
    CHECK(pack.initialize() == MapTilePackResult::BUSY);
    CHECK(pack.status() == MapTilePackStatus::READY);
    CHECK(pack.selectionGeneration() == 4U);
    CHECK(std::strcmp(pack.metadata().pack_id, "osm-bright") == 0);
    std::uint32_t size = 0U; CHECK(pack.beginGet(TileKey{2U,1U,1U}, size) == MapTilePackResult::OK);
    std::uint8_t output = 0U; std::size_t count = 0U;
    CHECK(pack.readGetChunk(&output, 1U, count) == MapTilePackResult::OK);
    CHECK(count == 1U && output == tile);
}
void testBothSemanticallyInvalidSlotsDoNotUseLegacyMarker() {
    beginTest(); FakeStorage storage;
    addMapSetSlot(storage, MapTilePack::ACTIVE_PACK_SLOT_0_PATH, 4U, "rogue");
    addMapSetSlot(storage, MapTilePack::ACTIVE_PACK_SLOT_1_PATH, 5U, "also-rogue");
    addSelection(storage, "legacy", manifestFor("legacy"));
    MapTilePack pack(storage);
    CHECK(pack.initialize() == MapTilePackResult::INVALID_MANIFEST);
    CHECK(!pack.hasSelection());
}
void testCoreDoesNotAllocate() {
    beginTest(); FakeStorage storage; addSelection(storage, "one", manifestFor("one")); const std::uint8_t tile[] = {7U};
    storage.add("/pyxis-map/packs/one/tiles/2/1/1.png", tile, sizeof(tile)); const std::size_t before = allocations;
    MapTilePack pack(storage); CHECK(pack.initialize() == MapTilePackResult::OK); std::uint32_t size = 0U;
    CHECK(pack.beginGet(TileKey{2U, 1U, 1U}, size) == MapTilePackResult::OK); std::uint8_t output = 0U; std::size_t count = 0U;
    CHECK(pack.readGetChunk(&output, 1U, count) == MapTilePackResult::OK); pack.endGet(); CHECK(allocations == before);
}
}  // namespace

int main() {
    testNoSelection(); testInvalidAndTraversalIds(); testMissingManifest(); testCorruptAndOversizedManifest();
    testMarkerManifestMismatch(); testUncoveredKey(); testCoveredMissingFile(); testCanonicalPathsAndCapacity();
    testStorageUnavailable(); testChunkReadAndAutomaticEnd(); testReadErrorClosesAndArgumentsAreBounded();
    testPrematureEndCloses(); testReinitializeSelectionChange(); testFailedReinitializeIsTransactional();
    testReinitializeAndDestructorCloseStreams(); testDeclaredLengthsAndEmbeddedNulFailClosed();
    testTileDeclaredLengthCapsWritesAndProbesEof(); testActiveSelectionSlotsPreferNewestValidAndRejectConflict();
    testEqualGenerationIdentityUsesLengthAndExactBytesNotTrailingChecksum();
    testActiveMapSetRequiresAllowlistedStyleAndMatchingImmutableManifests();
    testNewCanonicalProfilesPassAndCrossStyleCompositionFails();
    testActiveMapSetComposesPacksByPriorityAndCoverage();
    testRebootFallsBackFromNewerSemanticallyInvalidMapSet();
    testNewerManifestIndeterminateDoesNotFallBackAndPreservesSelection();
    testBothSemanticallyInvalidSlotsDoNotUseLegacyMarker();
    testCoreDoesNotAllocate();
    std::cout << "map tile pack: " << tests_run << " tests passed\n";
    return 0;
}
