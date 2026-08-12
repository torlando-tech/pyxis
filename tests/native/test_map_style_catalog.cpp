#include "Hardware/TDeck/MapStyleCatalog.h"
#include "Hardware/TDeck/MapTilePack.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

using Hardware::TDeck::ActiveMapSetCodec;
using Hardware::TDeck::ActiveMapSetView;
using Hardware::TDeck::MapStyleCatalog;
using Hardware::TDeck::MapStyleCatalogResult;
using Hardware::TDeck::MapStyleSummary;
using Hardware::TDeck::MapTilePack;
using Hardware::TDeck::MapTileStorage;
using Hardware::TDeck::TileStoreResult;
using Pyxis::MapPackManifest;
using Pyxis::RowSpan;

namespace {
std::size_t tests_run = 0U;
void fail(const char* expression, int line) {
    std::cerr << "line " << line << ": " << expression << '\n';
    std::exit(1);
}
#define CHECK(expression) do { if (!(expression)) fail(#expression, __LINE__); } while (false)
void beginTest() { ++tests_run; }
bool rejectCommit(void*) { return false; }

std::uint32_t crc32(const std::uint8_t* input, std::size_t length) {
    std::uint32_t crc = UINT32_C(0xffffffff);
    for (std::size_t index = 0U; index < length; ++index) {
        crc ^= input[index];
        for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? UINT32_C(0xedb88320) : 0U);
        }
    }
    return ~crc;
}
void putU16(std::uint8_t* output, std::uint16_t value) {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
}
void putU32(std::uint8_t* output, std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
    output[2] = static_cast<std::uint8_t>(value >> 16U);
    output[3] = static_cast<std::uint8_t>(value >> 24U);
}
std::uint32_t getU32(const std::uint8_t* input) {
    return static_cast<std::uint32_t>(input[0]) |
        (static_cast<std::uint32_t>(input[1]) << 8U) |
        (static_cast<std::uint32_t>(input[2]) << 16U) |
        (static_cast<std::uint32_t>(input[3]) << 24U);
}

const char* attributionFor(const char* style) {
    if (std::strcmp(style, "osm-bright") == 0)
        return "(c) OpenMapTiles (c) OpenStreetMap contributors";
    if (std::strcmp(style, "toner") == 0)
        return "(c) MapTiler (c) OpenStreetMap contributors";
    return "(c) OpenMapTiles (c) OpenStreetMap contributors; style (c) CARTO";
}
const char* sourceFor(const char* style) {
    if (std::strcmp(style, "osm-bright") == 0) return "Oxed's Map Tile Downloader (OSM Bright)";
    if (std::strcmp(style, "dark-matter") == 0) return "Oxed's Map Tile Downloader (Dark Matter)";
    if (std::strcmp(style, "positron") == 0) return "Oxed's Map Tile Downloader (Positron)";
    return "Oxed's Map Tile Downloader (Toner)";
}
const char* licenseFor(const char* style) {
    if (std::strcmp(style, "osm-bright") == 0)
        return "OSM ODbL; style CC-BY-4.0/BSD-3-Clause";
    if (std::strcmp(style, "toner") == 0)
        return "OSM ODbL; style CC-BY-4.0/BSD-3-Clause (Stamen ISC)";
    return "OSM ODbL; style CC-BY-4.0/BSD-3-Clause (CARTO CC-BY-3.0)";
}

std::size_t makeRecord(std::uint8_t* output, const char* style, std::uint32_t generation,
                       const char* first_pack = "overview", const char* second_pack = NULL) {
    std::memset(output, 0, ActiveMapSetCodec::MAX_SERIALIZED_SIZE);
    output[0] = 'P'; output[1] = 'M'; output[2] = 'A'; output[3] = 'S'; output[4] = 2U;
    putU32(output + 8U, generation);
    std::size_t position = 12U;
    const std::size_t style_length = std::strlen(style);
    output[position++] = static_cast<std::uint8_t>(style_length);
    std::memcpy(output + position, style, style_length); position += style_length;
    const char* attribution = attributionFor(style);
    const std::size_t attribution_length = std::strlen(attribution);
    output[position++] = static_cast<std::uint8_t>(attribution_length);
    std::memcpy(output + position, attribution, attribution_length); position += attribution_length;
    output[position++] = second_pack == NULL ? 1U : 2U;
    const char* packs[2] = {first_pack, second_pack};
    const std::size_t pack_count = second_pack == NULL ? 1U : 2U;
    for (std::size_t pack = 0U; pack < pack_count; ++pack) {
        const std::size_t id_length = std::strlen(packs[pack]);
        output[position++] = static_cast<std::uint8_t>(id_length);
        std::memcpy(output + position, packs[pack], id_length); position += id_length;
        putU16(output + position, 1U); position += 2U;
        output[position++] = 2U;
        putU32(output + position, 1U); position += 4U;
        putU32(output + position, static_cast<std::uint32_t>(pack + 1U)); position += 4U;
        putU32(output + position, static_cast<std::uint32_t>(pack + 1U)); position += 4U;
    }
    const std::size_t length = position + 4U;
    putU16(output + 6U, static_cast<std::uint16_t>(length));
    putU32(output + position, crc32(output, position));
    return length;
}
void refreshCrc(std::uint8_t* record, std::size_t length) {
    putU32(record + length - 4U, crc32(record, length - 4U));
}

struct File {
    char path[96];
    std::uint8_t bytes[ActiveMapSetCodec::MAX_SERIALIZED_SIZE + 1U];
    std::size_t size;
};

class FakeStorage : public MapTileStorage {
public:
    FakeStorage() : available(true), file_count(0U), read_file(NULL), read_position(0U),
        write_file(NULL), write_size(0U), list_calls(0U), abort_calls(0U), begin_write_calls(0U),
        fail_active_slot_reads(false), fail_write(false), fail_commit(false),
        corrupt_after_commit(false), short_write(false), fail_manifest_reads(false) {}

    File* find(const char* path) {
        for (std::size_t index = 0U; index < file_count; ++index) {
            if (std::strcmp(files[index].path, path) == 0) return &files[index];
        }
        return NULL;
    }
    void add(const char* path, const std::uint8_t* bytes, std::size_t size) {
        File* file = find(path);
        if (file == NULL) {
            CHECK(file_count < 10U); file = &files[file_count++];
            CHECK(std::strlen(path) < sizeof(file->path)); std::strcpy(file->path, path);
        }
        CHECK(size <= sizeof(file->bytes));
        if (size != 0U) std::memcpy(file->bytes, bytes, size);
        file->size = size;
    }
    void erase(const char* path) {
        for (std::size_t index = 0U; index < file_count; ++index) {
            if (std::strcmp(files[index].path, path) != 0) continue;
            files[index] = files[file_count - 1U]; --file_count; return;
        }
    }
    virtual bool isAvailable() const { return available; }
    virtual TileStoreResult beginRead(const char* path, std::uint32_t& size) {
        if (!available) return TileStoreResult::STORAGE_UNAVAILABLE;
        if (fail_active_slot_reads && std::strstr(path, "active-pack.") != NULL) return TileStoreResult::IO_ERROR;
        if (fail_manifest_reads && std::strstr(path, "manifest.pmp") != NULL) return TileStoreResult::BUSY;
        if (read_file != NULL) return TileStoreResult::BUSY;
        File* file = find(path); if (file == NULL) return TileStoreResult::MISS;
        read_file = file; read_position = 0U; size = static_cast<std::uint32_t>(file->size);
        return TileStoreResult::OK;
    }
    virtual TileStoreResult readChunk(std::uint8_t* output, std::size_t capacity, std::size_t& count) {
        count = 0U; if (read_file == NULL) return TileStoreResult::IO_ERROR;
        const std::size_t remaining = read_file->size - read_position;
        count = remaining < capacity ? remaining : capacity;
        if (count != 0U) std::memcpy(output, read_file->bytes + read_position, count);
        read_position += count; return TileStoreResult::OK;
    }
    virtual void endRead() { read_file = NULL; read_position = 0U; }
    virtual TileStoreResult beginWrite(const char* path) {
        ++begin_write_calls;
        if (fail_write || write_file != NULL) return TileStoreResult::IO_ERROR;
        File* file = find(path);
        if (file == NULL) {
            CHECK(file_count < 10U); file = &files[file_count++];
            std::strcpy(file->path, path); file->size = 0U;
        }
        write_file = file; write_size = 0U; return TileStoreResult::OK;
    }
    virtual TileStoreResult writeChunk(const std::uint8_t* data, std::size_t size, std::size_t& written) {
        written = 0U; if (write_file == NULL) return TileStoreResult::IO_ERROR;
        const std::size_t amount = short_write && size > 1U ? size - 1U : size;
        CHECK(write_size + amount <= sizeof(write_file->bytes));
        std::memcpy(write_file->bytes + write_size, data, amount); write_size += amount; written = amount;
        return short_write ? TileStoreResult::IO_ERROR : TileStoreResult::OK;
    }
    virtual TileStoreResult commitWrite() {
        if (write_file == NULL || fail_commit) return TileStoreResult::IO_ERROR;
        write_file->size = write_size;
        if (corrupt_after_commit && write_file->size != 0U) write_file->bytes[write_file->size - 1U] ^= 1U;
        write_file = NULL; write_size = 0U; return TileStoreResult::OK;
    }
    virtual void abortWrite() { ++abort_calls; write_file = NULL; write_size = 0U; }
    virtual TileStoreResult remove(const char*) { return TileStoreResult::IO_ERROR; }
    virtual TileStoreResult rename(const char*, const char*) { return TileStoreResult::IO_ERROR; }
    virtual TileStoreResult stat(const char*, std::uint32_t&) { return TileStoreResult::IO_ERROR; }
    virtual TileStoreResult beginList() { ++list_calls; return TileStoreResult::IO_ERROR; }
    virtual TileStoreResult nextList(char*, std::size_t, bool&) { ++list_calls; return TileStoreResult::IO_ERROR; }
    virtual void endList() { ++list_calls; }

    bool available; File files[10]; std::size_t file_count; File* read_file; std::size_t read_position;
    File* write_file; std::size_t write_size; std::size_t list_calls; std::size_t abort_calls;
    std::size_t begin_write_calls;
    bool fail_active_slot_reads; bool fail_write; bool fail_commit; bool corrupt_after_commit; bool short_write;
    bool fail_manifest_reads;
};

void addStyleManifest(FakeStorage& storage, const char* style, const char* pack_id,
                      bool mismatch = false) {
    MapPackManifest manifest = {};
    std::strcpy(manifest.pack_id, pack_id); std::strcpy(manifest.name, "Style pack");
    std::strcpy(manifest.attribution, mismatch ? "wrong attribution" : attributionFor(style));
    std::strcpy(manifest.source, sourceFor(style)); std::strcpy(manifest.license, licenseFor(style));
    manifest.min_zoom = 2U; manifest.max_zoom = 2U; manifest.tile_count = 1U;
    const RowSpan span = {2U, 1U, 1U, 1U};
    std::uint8_t bytes[MapPackManifest::MAX_SERIALIZED_SIZE]; std::size_t written = 0U;
    CHECK(MapPackManifest::serializeSparse(manifest, &span, 1U, bytes, sizeof(bytes), written) ==
          Pyxis::ManifestResult::OK);
    char path[MapTilePack::PATH_CAPACITY];
    CHECK(MapTilePack::manifestPath(pack_id, path, sizeof(path)) ==
          Hardware::TDeck::MapTilePackResult::OK);
    storage.add(path, bytes, written);
}

void addStyle(FakeStorage& storage, const char* style, std::uint32_t generation = 1U) {
    std::uint8_t record[ActiveMapSetCodec::MAX_SERIALIZED_SIZE];
    const std::size_t length = makeRecord(record, style, generation, style);
    char path[80]; std::strcpy(path, "/pyxis-map/map-sets/"); std::strcat(path, style); std::strcat(path, ".pmas");
    storage.add(path, record, length);
    addStyleManifest(storage, style, style);
}
void addActive(FakeStorage& storage, unsigned slot, const char* style, std::uint32_t generation) {
    std::uint8_t record[ActiveMapSetCodec::MAX_SERIALIZED_SIZE];
    const std::size_t length = makeRecord(record, style, generation);
    storage.add(slot == 0U ? MapStyleCatalog::ACTIVE_SLOT_0_PATH : MapStyleCatalog::ACTIVE_SLOT_1_PATH,
                record, length);
}

void testCodecAcceptsCanonicalAndResequences() {
    beginTest(); std::uint8_t input[ActiveMapSetCodec::MAX_SERIALIZED_SIZE];
    std::uint8_t output[ActiveMapSetCodec::MAX_SERIALIZED_SIZE];
    const std::size_t length = makeRecord(input, "osm-bright", 7U, "overview", "detail");
    ActiveMapSetView view = {};
    CHECK(ActiveMapSetCodec::decode(input, length, view));
    CHECK(view.generation == 7U); CHECK(std::strcmp(view.map_set_id, "osm-bright") == 0);
    CHECK(view.pack_count == 2U); CHECK(view.total_span_count == 2U);
    std::size_t written = 0U;
    CHECK(ActiveMapSetCodec::resequence(input, length, 8U, output, sizeof(output), written));
    CHECK(written == length); CHECK(getU32(output + 8U) == 8U);
    CHECK(ActiveMapSetCodec::decode(output, written, view));
}
void testCodecRejectsMalformedCanonicalFields() {
    beginTest(); std::uint8_t record[ActiveMapSetCodec::MAX_SERIALIZED_SIZE];
    std::size_t length = makeRecord(record, "osm-bright", 1U, "same", "same");
    ActiveMapSetView view = {}; CHECK(!ActiveMapSetCodec::decode(record, length, view));
    length = makeRecord(record, "osm-bright", 1U);
    record[12U] = 10U; record[13U] = 'O'; refreshCrc(record, length);
    CHECK(!ActiveMapSetCodec::decode(record, length, view));
    length = makeRecord(record, "osm-bright", 1U);
    record[length - 5U] = 2U; refreshCrc(record, length);
    CHECK(!ActiveMapSetCodec::decode(record, length, view));
    CHECK(!ActiveMapSetCodec::decode(record, ActiveMapSetCodec::MAX_SERIALIZED_SIZE + 1U, view));
}
void testDiscoveryUsesOnlyAllowlistedExactPaths() {
    beginTest(); FakeStorage storage; addStyle(storage, "osm-bright"); addStyle(storage, "positron");
    std::uint8_t rogue[ActiveMapSetCodec::MAX_SERIALIZED_SIZE];
    storage.add("/pyxis-map/map-sets/rogue.pmas", rogue, makeRecord(rogue, "rogue", 1U));
    MapStyleCatalog catalog(storage); CHECK(catalog.discover() == MapStyleCatalogResult::OK);
    CHECK(catalog.count() == 2U); CHECK(std::strcmp(catalog.style(0U)->id, "osm-bright") == 0);
    CHECK(std::strcmp(catalog.style(1U)->id, "positron") == 0); CHECK(storage.list_calls == 0U);
}
void testDiscoveryRejectsMismatchedAndMalformedStyleRecords() {
    beginTest(); FakeStorage mismatch; addStyle(mismatch, "dark-matter");
    File* file = mismatch.find("/pyxis-map/map-sets/dark-matter.pmas"); CHECK(file != NULL);
    file->bytes[13U] = 'x'; refreshCrc(file->bytes, file->size);
    MapStyleCatalog first(mismatch); CHECK(first.discover() == MapStyleCatalogResult::INVALID_STYLE_RECORD);

    FakeStorage oversized; std::uint8_t bytes[ActiveMapSetCodec::MAX_SERIALIZED_SIZE + 1U] = {};
    oversized.add("/pyxis-map/map-sets/toner.pmas", bytes, sizeof(bytes));
    MapStyleCatalog second(oversized); CHECK(second.discover() == MapStyleCatalogResult::INVALID_STYLE_RECORD);
}
void testMissingCurrentRecordIsSynthesizedWithoutWrite() {
    beginTest(); FakeStorage storage; addActive(storage, 0U, "toner", 9U);
    MapStyleCatalog catalog(storage); CHECK(catalog.discover() == MapStyleCatalogResult::OK);
    CHECK(catalog.count() == 1U); const MapStyleSummary* style = catalog.style(0U); CHECK(style != NULL);
    CHECK(std::strcmp(style->id, "toner") == 0); CHECK(style->active); CHECK(style->synthesized);
    CHECK(storage.find("/pyxis-map/map-sets/toner.pmas") == NULL); CHECK(storage.list_calls == 0U);
}
void testStaleAndUnknownActivationAreRejectedWithoutWrite() {
    beginTest(); FakeStorage storage; addStyle(storage, "osm-bright"); addStyle(storage, "dark-matter");
    addActive(storage, 0U, "osm-bright", 4U); MapStyleCatalog catalog(storage);
    CHECK(catalog.discover() == MapStyleCatalogResult::OK); const std::uint32_t generation = catalog.generation();
    CHECK(catalog.activate(generation - 1U, "dark-matter") == MapStyleCatalogResult::STALE_CATALOG);
    CHECK(catalog.activate(generation, "rogue") == MapStyleCatalogResult::UNKNOWN_STYLE);
    CHECK(storage.find(MapStyleCatalog::ACTIVE_SLOT_1_PATH) == NULL);
}
void testCancellationAbortsBeforePublishingActiveSlot() {
    beginTest(); FakeStorage storage; addStyle(storage, "osm-bright");
    addStyle(storage, "dark-matter"); addActive(storage, 0U, "osm-bright", 4U);
    MapStyleCatalog catalog(storage); CHECK(catalog.discover() == MapStyleCatalogResult::OK);
    CHECK(catalog.activate(catalog.generation(), "dark-matter", &rejectCommit,
                           NULL) == MapStyleCatalogResult::CANCELLED);
    CHECK(storage.abort_calls == 1U);
    File* target = storage.find(MapStyleCatalog::ACTIVE_SLOT_1_PATH);
    CHECK(target != NULL && target->size == 0U);
    File* active = storage.find(MapStyleCatalog::ACTIVE_SLOT_0_PATH);
    ActiveMapSetView view = {};
    CHECK(active != NULL && ActiveMapSetCodec::decode(active->bytes, active->size, view));
    CHECK(std::strcmp(view.map_set_id, "osm-bright") == 0);
}
void testActivationTargetsMissingThenOlderSlotAndVerifiesBytes() {
    beginTest(); FakeStorage storage; addStyle(storage, "osm-bright"); addStyle(storage, "dark-matter");
    addActive(storage, 0U, "osm-bright", 4U); MapStyleCatalog catalog(storage);
    CHECK(catalog.discover() == MapStyleCatalogResult::OK); std::uint32_t generation = catalog.generation();
    CHECK(catalog.activate(generation, "dark-matter") == MapStyleCatalogResult::OK);
    File* slot1 = storage.find(MapStyleCatalog::ACTIVE_SLOT_1_PATH); CHECK(slot1 != NULL);
    ActiveMapSetView view = {}; CHECK(ActiveMapSetCodec::decode(slot1->bytes, slot1->size, view));
    CHECK(view.generation == 5U); CHECK(std::strcmp(view.map_set_id, "dark-matter") == 0);
    generation = catalog.generation();
    CHECK(catalog.activate(generation, "osm-bright") == MapStyleCatalogResult::OK);
    File* slot0 = storage.find(MapStyleCatalog::ACTIVE_SLOT_0_PATH); CHECK(slot0 != NULL);
    CHECK(ActiveMapSetCodec::decode(slot0->bytes, slot0->size, view)); CHECK(view.generation == 6U);
}
void testActivationSemanticallyValidatesBeforeAnySlotWrite() {
    beginTest();
    for (std::size_t case_index = 0U; case_index < 4U; ++case_index) {
        FakeStorage storage; addStyle(storage, "dark-matter"); addActive(storage, 0U, "osm-bright", 3U);
        MapStyleCatalog catalog(storage); CHECK(catalog.discover() == MapStyleCatalogResult::OK);
        File* prior = storage.find(MapStyleCatalog::ACTIVE_SLOT_0_PATH); CHECK(prior != NULL);
        std::uint8_t prior_bytes[ActiveMapSetCodec::MAX_SERIALIZED_SIZE];
        const std::size_t prior_size = prior->size; std::memcpy(prior_bytes, prior->bytes, prior_size);
        if (case_index == 0U) {
            storage.erase("/pyxis-map/packs/dark-matter/manifest.pmp");
        } else if (case_index == 1U) {
            File* manifest = storage.find("/pyxis-map/packs/dark-matter/manifest.pmp");
            CHECK(manifest != NULL); manifest->bytes[0] ^= 1U;
        } else if (case_index == 2U) {
            addStyleManifest(storage, "dark-matter", "dark-matter", true);
        } else {
            storage.fail_manifest_reads = true;
        }
        const MapStyleCatalogResult expected = case_index == 3U
            ? MapStyleCatalogResult::ACTIVE_IO_INDETERMINATE
            : MapStyleCatalogResult::INVALID_STYLE_RECORD;
        CHECK(catalog.activate(catalog.generation(), "dark-matter") == expected);
        CHECK(storage.begin_write_calls == 0U);
        prior = storage.find(MapStyleCatalog::ACTIVE_SLOT_0_PATH);
        CHECK(prior != NULL && prior->size == prior_size);
        CHECK(std::memcmp(prior->bytes, prior_bytes, prior_size) == 0);
        CHECK(storage.find(MapStyleCatalog::ACTIVE_SLOT_1_PATH) == NULL);
    }
}
void testActivationRejectsIndeterminateConflictAndExhaustion() {
    beginTest(); FakeStorage io; addStyle(io, "dark-matter"); io.fail_active_slot_reads = true;
    MapStyleCatalog first(io); CHECK(first.discover() == MapStyleCatalogResult::ACTIVE_IO_INDETERMINATE);

    FakeStorage conflict; addStyle(conflict, "dark-matter"); addActive(conflict, 0U, "osm-bright", 7U);
    addActive(conflict, 1U, "positron", 7U); MapStyleCatalog second(conflict);
    CHECK(second.discover() == MapStyleCatalogResult::ACTIVE_CONFLICT);

    FakeStorage exhausted; addStyle(exhausted, "dark-matter"); addActive(exhausted, 0U, "osm-bright", UINT32_MAX);
    MapStyleCatalog third(exhausted); CHECK(third.discover() == MapStyleCatalogResult::OK);
    CHECK(third.activate(third.generation(), "dark-matter") == MapStyleCatalogResult::GENERATION_EXHAUSTED);
}
void testWriteAndReadbackFailuresPreservePriorValidSlot() {
    beginTest(); FakeStorage torn; addStyle(torn, "dark-matter"); addActive(torn, 0U, "osm-bright", 3U);
    MapStyleCatalog first(torn); CHECK(first.discover() == MapStyleCatalogResult::OK); torn.fail_commit = true;
    CHECK(first.activate(first.generation(), "dark-matter") == MapStyleCatalogResult::WRITE_FAILED);
    File* prior = torn.find(MapStyleCatalog::ACTIVE_SLOT_0_PATH); ActiveMapSetView view = {};
    CHECK(prior != NULL && ActiveMapSetCodec::decode(prior->bytes, prior->size, view));
    CHECK(view.generation == 3U && std::strcmp(view.map_set_id, "osm-bright") == 0);
    CHECK(torn.abort_calls == 1U);

    FakeStorage corrupt; addStyle(corrupt, "dark-matter"); addActive(corrupt, 0U, "osm-bright", 3U);
    MapStyleCatalog second(corrupt); CHECK(second.discover() == MapStyleCatalogResult::OK);
    corrupt.corrupt_after_commit = true;
    CHECK(second.activate(second.generation(), "dark-matter") == MapStyleCatalogResult::READBACK_MISMATCH);
    prior = corrupt.find(MapStyleCatalog::ACTIVE_SLOT_0_PATH);
    CHECK(prior != NULL && ActiveMapSetCodec::decode(prior->bytes, prior->size, view));
    CHECK(view.generation == 3U);
}
void testCatalogIsFixedBoundedAndCopiesSummaries() {
    beginTest(); FakeStorage storage; addStyle(storage, "osm-bright"); addStyle(storage, "dark-matter");
    addStyle(storage, "positron"); addStyle(storage, "toner"); MapStyleCatalog catalog(storage);
    CHECK(catalog.discover() == MapStyleCatalogResult::OK); CHECK(catalog.count() == MapStyleCatalog::MAX_STYLES);
    for (std::size_t index = 0U; index < catalog.count(); ++index) {
        CHECK(catalog.style(index) != NULL); CHECK(catalog.style(index)->pack_count == 1U);
        CHECK(std::strcmp(catalog.style(index)->attribution,
                          attributionFor(catalog.style(index)->id)) == 0);
    }
    CHECK(catalog.style(MapStyleCatalog::MAX_STYLES) == NULL); CHECK(storage.list_calls == 0U);
}
}  // namespace

int main() {
    testCodecAcceptsCanonicalAndResequences();
    testCodecRejectsMalformedCanonicalFields();
    testDiscoveryUsesOnlyAllowlistedExactPaths();
    testDiscoveryRejectsMismatchedAndMalformedStyleRecords();
    testMissingCurrentRecordIsSynthesizedWithoutWrite();
    testStaleAndUnknownActivationAreRejectedWithoutWrite();
    testCancellationAbortsBeforePublishingActiveSlot();
    testActivationTargetsMissingThenOlderSlotAndVerifiesBytes();
    testActivationSemanticallyValidatesBeforeAnySlotWrite();
    testActivationRejectsIndeterminateConflictAndExhaustion();
    testWriteAndReadbackFailuresPreservePriorValidSlot();
    testCatalogIsFixedBoundedAndCopiesSummaries();
    std::cout << "map style catalog: " << tests_run << " tests passed\n";
    return 0;
}
