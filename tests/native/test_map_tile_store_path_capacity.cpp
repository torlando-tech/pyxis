// Regression test: MapTileStoreSD mount-path capacity with a maximum-length
// pack ID.
//
// The SD store maps every firmware path onto the mounted card by prepending
// the "/sd" prefix (makeMountedPath in MapTileStoreSD.cpp). With the real
// 31-character world pack ID ("world-osm-bright-z0-z9-20260801"), any tile
// whose x or y coordinate is two digits makes the mounted path longer than
// the PATH_CAPACITY + 4 mount buffer, so beginRead rejects it before the file
// lookup with INVALID_ARGUMENT -- which MapTilePack::beginGet surfaces on the
// device as "Tile I/O error". Observed on the physical T-Deck at zoom 5
// (single-digit quadrants rendered, two-digit quadrants failed) and on x86
// against the real firmware build.
//
// The core regression section drives the UNMODIFIED production MapTileStoreSD
// (compiled against the sdhostshim headers): a path whose mounted form
// overflowed the old 68-byte buffer must be accepted once the buffer is
// sized correctly. The result is file-independent: overflow is rejected
// before stat(), and the control path (which fits) reaches the file lookup
// and reports MISS when no card content is present. The optional
// end-to-end section additionally initializes a real MapTilePack and reads
// tiles byte-for-byte, but only when "/sd" is a live card mount (e.g. the
// bind mount set up for the device harness).
//
// NOTE: the test is written to the FIXED behavior. On the pre-fix tree it
// fails in the core section (the two-digit z4/z5 reads return
// INVALID_ARGUMENT instead of OK) -- that failure is the TDD red step.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#include "Hardware/TDeck/MapTilePack.h"
#include "Hardware/TDeck/MapTileStore.h"
#include "Hardware/TDeck/MapTileStoreSD.h"
#include "Hardware/TDeck/SDAccess.h"
#include "Hardware/TDeck/ActiveMapSetCodec.h"
#include "UI/LXMF/MapPackManifest.h"
#include <SD.h>

using Hardware::TDeck::ActiveMapSetCodec;
using Hardware::TDeck::MapTilePack;
using Hardware::TDeck::MapTilePackResult;
using Hardware::TDeck::MapTileStorage;
using Hardware::TDeck::MapTileStore;
using Hardware::TDeck::MapTileStoreSD;
using Hardware::TDeck::SDAccess;
using Hardware::TDeck::TileKey;
using Hardware::TDeck::TileStoreResult;
using Pyxis::MapPackManifest;

namespace {

std::size_t tests_run = 0U;
void fail(const char* expression, int line) {
    std::fprintf(stderr, "line %d: %s\n", line, expression);
    std::exit(1);
}
#define CHECK(expression) do { if (!(expression)) fail(#expression, __LINE__); } while (false)
void beginTest() { ++tests_run; }

const char* const kPackId = "world-osm-bright-z0-z9-20260801";  // 31 chars (max)
const char* const kMapSetId = "osm-bright";
const char* const kAttribution = "(c) OpenMapTiles (c) OpenStreetMap contributors";

// The production mount buffer (MapTileStoreSD.cpp): PATH_CAPACITY + 4 bytes
// for the "/sd" prefix. The model store below pins the pre-fix width so the
// overflow boundary stays exercised no matter how PATH_CAPACITY changes.
const std::size_t kModelMountCapacity = 68U;

std::uint32_t crc32_ieee(const std::uint8_t* data, std::size_t length) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0U; i < length; ++i) {
        crc ^= data[i];
        for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xedb88320U : 0U);
        }
    }
    return ~crc;
}

// Storage model of the pre-fix MapTileStoreSD mount-prefix arithmetic:
// paths whose "/sd" form overflowed the 68-byte mount buffer are rejected
// with INVALID_ARGUMENT before stat(), exactly as the real store rejected
// them on the device.
class ModelSDStorage : public MapTileStorage {
public:
    struct Entry { std::string path; std::vector<std::uint8_t> bytes; };

    ModelSDStorage() : open_index(-1), position(0U), open_length(0U) {}

    bool isAvailable() const override { return true; }

    TileStoreResult beginRead(const char* name, std::uint32_t& size) override {
        if (!fits(name)) return TileStoreResult::INVALID_ARGUMENT;
        const std::string n(name);
        for (std::size_t i = 0U; i < files.size(); ++i) {
            if (files[i].path == n) {
                open_index = static_cast<int>(i);
                position = 0U;
                open_length = files[i].bytes.size();
                size = static_cast<std::uint32_t>(open_length);
                return TileStoreResult::OK;
            }
        }
        return TileStoreResult::MISS;
    }
    TileStoreResult readChunk(std::uint8_t* out, std::size_t capacity, std::size_t& count) override {
        if (open_index < 0) return TileStoreResult::IO_ERROR;
        const std::size_t remaining = open_length - position;
        count = capacity < remaining ? capacity : remaining;
        if (count != 0U) {
            std::memcpy(out, &files[static_cast<std::size_t>(open_index)].bytes[position], count);
        }
        position += count;
        return TileStoreResult::OK;
    }
    void endRead() override { open_index = -1; }
    TileStoreResult beginWrite(const char*) override { return TileStoreResult::IO_ERROR; }
    TileStoreResult writeChunk(const std::uint8_t*, std::size_t, std::size_t&) override {
        return TileStoreResult::IO_ERROR;
    }
    TileStoreResult commitWrite() override { return TileStoreResult::IO_ERROR; }
    void abortWrite() override {}
    TileStoreResult remove(const char*) override { return TileStoreResult::IO_ERROR; }
    TileStoreResult rename(const char*, const char*) override { return TileStoreResult::IO_ERROR; }
    TileStoreResult stat(const char* name, std::uint32_t& size) override {
        if (!fits(name)) return TileStoreResult::INVALID_ARGUMENT;
        const std::string n(name);
        for (std::size_t i = 0U; i < files.size(); ++i) {
            if (files[i].path == n) {
                size = static_cast<std::uint32_t>(files[i].bytes.size());
                return TileStoreResult::OK;
            }
        }
        return TileStoreResult::MISS;
    }
    TileStoreResult beginList() override { return TileStoreResult::OK; }
    TileStoreResult nextList(char*, std::size_t, bool& done) override {
        done = true;
        return TileStoreResult::OK;
    }
    void endList() override {}

    void add(const char* name, const std::uint8_t* bytes, std::size_t length) {
        Entry e;
        e.path = name;
        e.bytes.assign(bytes, bytes + length);
        files.push_back(e);
    }
    // Mirrors makeMountedPath: snprintf(buffer, PATH_CAPACITY+4, "/sd%s", name).
    bool fits(const char* name) const {
        return (std::strlen(name) + 3U) < kModelMountCapacity;
    }

private:
    std::vector<Entry> files;
    int open_index;
    std::size_t position;
    std::size_t open_length;
};

std::string tilePathFor(const char* pack_id, std::uint8_t zoom, std::uint32_t x, std::uint32_t y) {
    // Larger than MapTileStore::PATH_CAPACITY on purpose: with the 31-character
    // pack ID, two-digit z5 tile paths are 69 bytes -- exactly the overflow
    // class this test pins.
    char buffer[128] = {};
    CHECK(MapTilePack::tilePath(pack_id, TileKey{zoom, x, y}, buffer, sizeof(buffer))
          == MapTilePackResult::OK);
    return buffer;
}

std::vector<std::uint8_t> build_pmas_v3(const char* map_set_id, const char* attribution,
                                        const char* pack_id) {
    // PMAS v3 layout (must match ActiveMapSetCodec::decode byte-for-byte):
    //   0..3  "PMAS"
    //   4     format_version (3 = indexless)
    //   5     reserved (0)
    //   6..7  u16 total record length (little-endian)
    //   8..11 u32 generation (non-zero)
    //   12..  u8 len + map_set_id
    //         u8 len + attribution
    //         u8 pack_count
    //         [u8 len + pack_id] * pack_count   (no spans in v3)
    //   tail  u32 CRC-32 (IEEE) over everything before it
    std::vector<std::uint8_t> payload;
    const auto put_str = [&payload](const char* text) {
        const std::size_t n = std::strlen(text);
        payload.push_back(static_cast<std::uint8_t>(n));
        payload.insert(payload.end(), text, text + n);
    };
    const auto put_u32 = [&payload](std::uint32_t v) {
        payload.push_back(static_cast<std::uint8_t>(v));
        payload.push_back(static_cast<std::uint8_t>(v >> 8U));
        payload.push_back(static_cast<std::uint8_t>(v >> 16U));
        payload.push_back(static_cast<std::uint8_t>(v >> 24U));
    };
    put_u32(1U);            // generation
    put_str(map_set_id);
    put_str(attribution);
    payload.push_back(1U);  // pack_count
    put_str(pack_id);

    const std::size_t total = 12U + payload.size();
    CHECK(total <= ActiveMapSetCodec::MAX_SERIALIZED_SIZE);

    std::vector<std::uint8_t> out;
    out.reserve(total);
    out.insert(out.end(), {'P', 'M', 'A', 'S', 3U, 0U});
    out.push_back(static_cast<std::uint8_t>(total));
    out.push_back(static_cast<std::uint8_t>(total >> 8U));
    out.insert(out.end(), payload.begin(), payload.end());
    const std::uint32_t crc = crc32_ieee(out.data(), out.size());
    out.push_back(static_cast<std::uint8_t>(crc));
    out.push_back(static_cast<std::uint8_t>(crc >> 8U));
    out.push_back(static_cast<std::uint8_t>(crc >> 16U));
    out.push_back(static_cast<std::uint8_t>(crc >> 24U));
    CHECK(out.size() == total);
    return out;
}

std::vector<std::uint8_t> build_manifest_v3(const char* pack_id) {
    MapPackManifest m = {};
    std::strcpy(m.pack_id, pack_id);
    std::strcpy(m.name, "World OSM Bright z0-z9");
    std::strcpy(m.attribution, kAttribution);
    std::strcpy(m.source, "Oxed's Map Tile Downloader (OSM Bright)");
    std::strcpy(m.license, "OSM ODbL; style CC-BY-4.0/BSD-3-Clause");
    m.min_zoom = 0U;
    m.max_zoom = 9U;
    m.tile_count = 83567U;
    m.format_version = MapPackManifest::INDEXLESS_FORMAT_VERSION;
    std::uint8_t out[MapPackManifest::MAX_SERIALIZED_SIZE] = {};
    std::size_t written = 0U;
    CHECK(MapPackManifest::serializeIndexless(m, out, sizeof(out), written)
          == Pyxis::ManifestResult::OK);
    CHECK(written <= sizeof(out));
    return std::vector<std::uint8_t>(out, out + written);
}

void read_tile_via(MapTilePack& pack, const TileKey& key, std::vector<std::uint8_t>& out) {
    std::uint32_t size = 0U;
    CHECK(pack.beginGet(key, size) == MapTilePackResult::OK);
    out.clear();
    while (out.size() < static_cast<std::size_t>(size)) {
        std::uint8_t chunk[256];
        std::size_t count = 0U;
        const MapTilePackResult r = pack.readGetChunk(chunk, sizeof(chunk), count);
        CHECK(r == MapTilePackResult::OK);
        CHECK(count > 0U);
        out.insert(out.end(), chunk, chunk + count);
    }
    CHECK(out.size() == static_cast<std::size_t>(size));
    pack.endGet();
}

std::string shell_dir(const std::string& dir) {
    // The dir is generated by this test from a compiler-provided temp path.
    std::string quoted;
    for (std::size_t i = 0U; i < dir.size(); ++i) {
        const char c = dir[i];
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    return "'" + quoted + "'";
}

void write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    const std::size_t slash = path.find_last_of('/');
    const std::string dir = path.substr(0U, slash);
    if (!dir.empty() && dir != "/") {
        (void)system(("mkdir -p " + shell_dir(dir)).c_str());
    }
    FILE* fp = std::fopen(path.c_str(), "wb");
    CHECK(fp != NULL);
    if (!bytes.empty()) {
        CHECK(std::fwrite(bytes.data(), 1U, bytes.size(), fp) == bytes.size());
    }
    std::fclose(fp);
}

// True when "/sd" is a writable directory the production store can use for
// the end-to-end card-content section (a bind mount set up by the test
// harness, or a user-owned dir). A root-owned empty dir (stat-able but not
// writable) is NOT ready: the fixture write would fail.
bool sd_mount_ready() {
    struct stat st;
    if (::stat("/sd", &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    return ::access("/sd", W_OK) == 0;
}

} // namespace

// Shim globals: SDAccess.cpp references the Arduino globals `SD` and `SPI`;
// the FreeRTOS shim needs a mutex factory.
HostSD SD;
HostSPI SPI;
std::string HostSD::root_ = "/tmp";
bool HostSD::present_ = false;
SemaphoreHandle_t hostsd_create_mutex() { return (void*)1; }

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string root = argv[1];
    HostSD::set_root(root.c_str());

    CHECK(std::strlen(kPackId) == 31U);
    CHECK(std::strlen(kPackId) == MapPackManifest::PACK_ID_CAPACITY - 1U);

    // -- Wire records decode through the production codecs -------------------
    std::vector<std::uint8_t> pmas = build_pmas_v3(kMapSetId, kAttribution, kPackId);
    beginTest();
    {
        Hardware::TDeck::ActiveMapSetView view = {};
        CHECK(ActiveMapSetCodec::decode(pmas.data(), pmas.size(), view));
        CHECK(view.format_version == ActiveMapSetCodec::INDEXLESS_FORMAT_VERSION);
        CHECK(view.generation == 1U);
        CHECK(view.pack_count == 1U);
        CHECK(std::strcmp(view.packs[0].pack_id, kPackId) == 0);
    }
    std::vector<std::uint8_t> manifest = build_manifest_v3(kPackId);
    beginTest();
    {
        MapPackManifest parsed = {};
        CHECK(MapPackManifest::parse(manifest.data(), manifest.size(), parsed)
              == Pyxis::ManifestResult::OK);
        CHECK(std::strcmp(parsed.pack_id, kPackId) == 0);
    }

    // -- The boundary itself, derived from the real path builders ------------
    beginTest();
    {
        // Single-digit quadrants fit the pre-fix 68-byte mount buffer...
        CHECK(tilePathFor(kPackId, 4U, 9U, 9U).size() + 3U < kModelMountCapacity);
        // ...and the two-digit quadrants that failed on the device do not.
        CHECK(tilePathFor(kPackId, 4U, 15U, 15U).size() + 3U >= kModelMountCapacity);
        CHECK(tilePathFor(kPackId, 5U, 10U, 0U).size() + 3U >= kModelMountCapacity);
    }

    // The fix: the production mount buffer must hold the worst case. With a
    // 31-character pack ID the longest z0-z22 mounted tile path is 82 bytes,
    // so PATH_CAPACITY must be at least 80 (giving an 84-byte buffer). Pre-fix
    // PATH_CAPACITY is 64, so this section is red until the fix lands.
    beginTest();
    {
        CHECK(MapTileStore::PATH_CAPACITY >= 80U);
        CHECK(MapTileStore::PATH_CAPACITY + 4U >= 84U);
    }

    // -- Model store: the pre-fix arithmetic, pinned --------------------------
    beginTest();
    {
        ModelSDStorage store;
        const std::vector<std::uint8_t> tile(64U, 0x5AU);
        store.add(tilePathFor(kPackId, 4U, 9U, 9U).c_str(), tile.data(), tile.size());
        std::uint32_t size = 0U;
        CHECK(store.beginRead(tilePathFor(kPackId, 4U, 9U, 9U).c_str(), size)
              == TileStoreResult::OK);
        CHECK(size == tile.size());
        store.endRead();
        // Two-digit x or y: rejected before the file lookup, pre-fix.
        CHECK(store.beginRead(tilePathFor(kPackId, 4U, 15U, 15U).c_str(), size)
              == TileStoreResult::INVALID_ARGUMENT);
        CHECK(store.beginRead(tilePathFor(kPackId, 5U, 10U, 0U).c_str(), size)
              == TileStoreResult::INVALID_ARGUMENT);
    }

    // -- CORE REGRESSION: the real, unmodified MapTileStoreSD ------------------
    // Production makeMountedPath rejects an overflowing mounted path with
    // INVALID_ARGUMENT before stat(); a path that fits reaches the file
    // lookup. Neither depends on card content, so this section runs
    // everywhere. Pre-fix, the two-digit reads below return
    // INVALID_ARGUMENT instead of OK and this test fails -- the TDD red.
    beginTest();
    {
        CHECK(SDAccess::init(xSemaphoreCreateMutex()));
        MapTileStoreSD store;
        CHECK(store.isAvailable());
        std::uint32_t size = 0U;
        // Fits the old buffer: passes the overflow check, reaches the lookup,
        // and reports MISS because no file exists on the (empty) mount.
        CHECK(store.beginRead(tilePathFor(kPackId, 4U, 9U, 9U).c_str(), size)
              == TileStoreResult::MISS);
        // The device repro class. With the fixed buffer width these pass the
        // overflow check and reach the lookup as well.
        CHECK(store.beginRead(tilePathFor(kPackId, 4U, 15U, 15U).c_str(), size)
              == TileStoreResult::MISS);
        CHECK(store.beginRead(tilePathFor(kPackId, 5U, 10U, 0U).c_str(), size)
              == TileStoreResult::MISS);
        CHECK(store.beginRead(tilePathFor(kPackId, 5U, 0U, 10U).c_str(), size)
              == TileStoreResult::MISS);
        CHECK(store.beginRead(tilePathFor(kPackId, 5U, 31U, 31U).c_str(), size)
              == TileStoreResult::MISS);
    }

    // -- Optional end-to-end: real MapTilePack over card content ---------------
    // Only meaningful when "/sd" holds a pack (device-harness bind mount).
    if (sd_mount_ready()) {
        // The production store stats the literal "/sd" prefix and opens via
        // SD.open(device_path); point the shim at the same mount point.
        HostSD::set_root("/sd");
        const auto make_tile = [](std::uint8_t seed) {
            std::vector<std::uint8_t> v(1024U);
            for (std::size_t i = 0U; i < v.size(); ++i) {
                v[i] = static_cast<std::uint8_t>((i & 0xffU) ^ seed);
            }
            return v;
        };
        const std::string pmap = "/sd/pyxis-map";
        {
            std::string cmd = "mkdir -p " + shell_dir(pmap + "/map-sets");
            CHECK(system(cmd.c_str()) == 0);
        }
        write_file(pmap + "/active-pack.0", pmas);
        write_file(pmap + "/map-sets/" + std::string(kMapSetId) + ".pmas", pmas);
        write_file(pmap + "/packs/" + kPackId + "/manifest.pmp", manifest);
        const auto write_tile = [&](std::uint8_t z, std::uint32_t x, std::uint32_t y,
                                    std::uint8_t seed) {
            // The production store reads the literal "/sd" mount point, so the
            // fixture must place tiles under /sd (device-relative path + prefix).
            write_file(std::string("/sd") + tilePathFor(kPackId, z, x, y), make_tile(seed));
        };
        write_tile(3U, 7U, 7U, 0xA1U);
        write_tile(4U, 9U, 9U, 0xA4U);
        write_tile(4U, 15U, 15U, 0xA5U);
        write_tile(5U, 10U, 0U, 0xA6U);
        write_tile(5U, 0U, 10U, 0xA7U);
        write_tile(5U, 31U, 31U, 0xA8U);

        MapTileStoreSD sd_store;
        MapTilePack pack(sd_store);
        beginTest();
        CHECK(pack.initialize() == MapTilePackResult::OK);
        CHECK(pack.hasSelection());
        CHECK(pack.selectionGeneration() == 1U);

        beginTest();
        {
            std::vector<std::uint8_t> data;
            read_tile_via(pack, TileKey{3U, 7U, 7U}, data);
            CHECK(data[0] == static_cast<std::uint8_t>(0x00U ^ 0xA1U));
        }
        beginTest();
        {
            std::vector<std::uint8_t> data;
            read_tile_via(pack, TileKey{4U, 15U, 15U}, data);
            CHECK(data[0] == static_cast<std::uint8_t>(0x00U ^ 0xA5U));
        }
        beginTest();
        {
            std::vector<std::uint8_t> data;
            read_tile_via(pack, TileKey{5U, 31U, 31U}, data);
            CHECK(data[0] == static_cast<std::uint8_t>(0x00U ^ 0xA8U));
        }
    } else {
        std::fprintf(stderr, "note: /sd not present; end-to-end card section skipped\n");
    }

    std::printf("map tile store path capacity: %lu tests passed\n",
                static_cast<unsigned long>(tests_run));
    return 0;
}
