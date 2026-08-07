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
    std::uint8_t bytes[MapPackManifest::MAX_SERIALIZED_SIZE];
    std::size_t size;
    std::uint32_t declared_size;
};

class FakeStorage : public MapTileStorage {
public:
    FakeStorage()
        : available(true), file_count(0U), open_file(NULL), position(0U),
          read_calls(0U), end_calls(0U), fail_read_call(0U), zero_read_call(0U) {}

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
    testTileDeclaredLengthCapsWritesAndProbesEof(); testCoreDoesNotAllocate();
    std::cout << "map tile pack: " << tests_run << " tests passed\n";
    return 0;
}
