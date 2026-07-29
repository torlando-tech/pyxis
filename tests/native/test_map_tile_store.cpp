#include "Hardware/TDeck/MapTileStore.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using Hardware::TDeck::MapTileStore;
using Hardware::TDeck::MapTileStorage;
using Hardware::TDeck::TileKey;
using Hardware::TDeck::TileStoreConfig;
using Hardware::TDeck::TileStoreResult;

namespace {
std::size_t tests_run = 0U;
void fail(const char* e, int line) { std::cerr << "line " << line << ": " << e << '\n'; std::exit(1); }
#define CHECK(e) do { if (!(e)) fail(#e, __LINE__); } while (false)
void beginTest() { ++tests_run; }

struct File { std::string path; std::vector<std::uint8_t> bytes; };

class FakeStorage : public MapTileStorage {
public:
    bool available;
    bool short_write;
    bool fail_remove;
    int fail_remove_call;
    int fail_rename_call;
    int power_cut_after_rename_call;
    std::string fail_remove_path;
    std::vector<File> files;
    std::string open_path;
    std::size_t position;
    std::size_t list_position;
    int rename_calls;
    int remove_calls;

    FakeStorage() : available(true), short_write(false), fail_remove(false), fail_remove_call(0),
                    fail_rename_call(0), power_cut_after_rename_call(0), position(0U), list_position(0U), rename_calls(0),
                    remove_calls(0) {}

    int find(const char* path) const {
        for (std::size_t i = 0U; i < files.size(); ++i) if (files[i].path == path) return static_cast<int>(i);
        return -1;
    }
    void add(const char* path, const std::vector<std::uint8_t>& bytes) {
        int i = find(path); if (i >= 0) files[static_cast<std::size_t>(i)].bytes = bytes;
        else files.push_back(File{path, bytes});
    }
    virtual bool isAvailable() const { return available; }
    virtual TileStoreResult beginRead(const char* path, std::uint32_t& size) {
        if (!available) return TileStoreResult::STORAGE_UNAVAILABLE;
        const int i = find(path); if (i < 0) return TileStoreResult::MISS;
        open_path = path; position = 0U; size = static_cast<std::uint32_t>(files[static_cast<std::size_t>(i)].bytes.size());
        return TileStoreResult::OK;
    }
    virtual TileStoreResult readChunk(std::uint8_t* out, std::size_t capacity, std::size_t& count) {
        if (!available) return TileStoreResult::STORAGE_UNAVAILABLE;
        const int i = find(open_path.c_str()); if (i < 0) return TileStoreResult::IO_ERROR;
        const std::vector<std::uint8_t>& b = files[static_cast<std::size_t>(i)].bytes;
        count = std::min(capacity, b.size() - position);
        if (count != 0U) std::memcpy(out, &b[position], count);
        position += count; return TileStoreResult::OK;
    }
    virtual void endRead() { open_path.clear(); }
    virtual TileStoreResult beginWrite(const char* path) {
        if (!available) return TileStoreResult::STORAGE_UNAVAILABLE;
        add(path, std::vector<std::uint8_t>()); open_path = path; return TileStoreResult::OK;
    }
    virtual TileStoreResult writeChunk(const std::uint8_t* data, std::size_t size, std::size_t& written) {
        if (!available) return TileStoreResult::STORAGE_UNAVAILABLE;
        const int i = find(open_path.c_str()); if (i < 0) return TileStoreResult::IO_ERROR;
        written = (short_write && size != 0U) ? size - 1U : size;
        files[static_cast<std::size_t>(i)].bytes.insert(files[static_cast<std::size_t>(i)].bytes.end(), data, data + written);
        return TileStoreResult::OK;
    }
    virtual TileStoreResult commitWrite() { open_path.clear(); return available ? TileStoreResult::OK : TileStoreResult::STORAGE_UNAVAILABLE; }
    virtual void abortWrite() { if (!open_path.empty()) remove(open_path.c_str()); open_path.clear(); }
    virtual TileStoreResult remove(const char* path) {
        if (!available) return TileStoreResult::STORAGE_UNAVAILABLE;
        ++remove_calls;
        if (fail_remove || (fail_remove_call == remove_calls) || fail_remove_path == path) return TileStoreResult::IO_ERROR;
        const int i = find(path); if (i < 0) return TileStoreResult::MISS;
        files.erase(files.begin() + i); return TileStoreResult::OK;
    }
    virtual TileStoreResult rename(const char* from, const char* to) {
        if (!available) return TileStoreResult::STORAGE_UNAVAILABLE;
        ++rename_calls; if (fail_rename_call == rename_calls) return TileStoreResult::IO_ERROR;
        const int i = find(from); if (i < 0) return TileStoreResult::MISS;
        remove(to); files[static_cast<std::size_t>(i >= find(from) ? find(from) : 0)].path = to;
        if (power_cut_after_rename_call == rename_calls) available = false;
        return TileStoreResult::OK;
    }
    virtual TileStoreResult stat(const char* path, std::uint32_t& size) {
        if (!available) return TileStoreResult::STORAGE_UNAVAILABLE;
        const int i = find(path); if (i < 0) return TileStoreResult::MISS;
        size = static_cast<std::uint32_t>(files[static_cast<std::size_t>(i)].bytes.size()); return TileStoreResult::OK;
    }
    virtual TileStoreResult beginList() { list_position = 0U; return available ? TileStoreResult::OK : TileStoreResult::STORAGE_UNAVAILABLE; }
    virtual TileStoreResult nextList(char* path, std::size_t capacity, bool& done) {
        if (!available) return TileStoreResult::STORAGE_UNAVAILABLE;
        if (list_position >= files.size()) { done = true; return TileStoreResult::OK; }
        done = false; const std::string& p = files[list_position++].path;
        if (p.size() + 1U > capacity) return TileStoreResult::INDEX_MISMATCH;
        std::memcpy(path, p.c_str(), p.size() + 1U); return TileStoreResult::OK;
    }
    virtual void endList() {}
};

std::vector<std::uint8_t> png(std::size_t size = 40U, std::uint32_t w = 256U, std::uint32_t h = 256U) {
    std::vector<std::uint8_t> b(size, 0U);
    const std::uint8_t sig[] = {137U,80U,78U,71U,13U,10U,26U,10U};
    if (size >= 24U) {
        std::memcpy(&b[0], sig, 8U); b[11] = 13U; b[12]='I'; b[13]='H'; b[14]='D'; b[15]='R';
        b[16]=static_cast<std::uint8_t>(w>>24); b[17]=static_cast<std::uint8_t>(w>>16); b[18]=static_cast<std::uint8_t>(w>>8); b[19]=static_cast<std::uint8_t>(w);
        b[20]=static_cast<std::uint8_t>(h>>24); b[21]=static_cast<std::uint8_t>(h>>16); b[22]=static_cast<std::uint8_t>(h>>8); b[23]=static_cast<std::uint8_t>(h);
    }
    return b;
}
TileStoreConfig config(std::uint16_t entries=3U, std::uint32_t quota=120U, std::uint32_t maximum=80U) {
    TileStoreConfig c = {entries, quota, maximum}; return c;
}
TileStoreResult put(MapTileStore& s, const TileKey& k, const std::vector<std::uint8_t>& b, std::size_t split=17U) {
    TileStoreResult r=s.beginPut(k); if (r!=TileStoreResult::OK) return r;
    for (std::size_t p=0U;p<b.size();) { const std::size_t n=std::min(split,b.size()-p); r=s.writePutChunk(&b[p],n); if(r!=TileStoreResult::OK)return r; p+=n; }
    return s.finishPut();
}
void drain(MapTileStore& s, const TileKey& k, std::size_t expected) {
    std::uint32_t size=0U; CHECK(s.beginGet(k,size)==TileStoreResult::OK); CHECK(size==expected);
    std::uint8_t b[13]; std::size_t total=0U,n=0U; do { CHECK(s.readGetChunk(b,sizeof(b),n)==TileStoreResult::OK); total+=n; } while(n!=0U);
    CHECK(total==expected); s.endGet();
}

void testKeyAndCanonicalPath() { beginTest(); FakeStorage fs; MapTileStore s(fs,config()); CHECK(s.initialize()==TileStoreResult::OK);
    char p[MapTileStore::PATH_CAPACITY]; CHECK(MapTileStore::canonicalPath(TileKey{22U,4194303U,4194303U},p,sizeof(p))==TileStoreResult::OK);
    CHECK(std::string(p)=="/pyxis-map/tiles/22/4194303/4194303.png");
    CHECK(MapTileStore::canonicalPath(TileKey{23U,0U,0U},p,sizeof(p))==TileStoreResult::INVALID_KEY);
    CHECK(MapTileStore::canonicalPath(TileKey{1U,2U,0U},p,sizeof(p))==TileStoreResult::INVALID_KEY);
}
void testMissHitAndRemoval() { beginTest(); FakeStorage fs; MapTileStore s(fs,config()); CHECK(s.initialize()==TileStoreResult::OK);
    const TileKey key={0U,0U,0U}; std::uint32_t z=99U; CHECK(s.beginGet(key,z)==TileStoreResult::MISS); CHECK(put(s,key,png())==TileStoreResult::OK); drain(s,key,40U);
    CHECK(s.removeTile(key)==TileStoreResult::OK); CHECK(s.entryCount()==0U); CHECK(s.totalBytes()==0U); CHECK(s.beginGet(key,z)==TileStoreResult::MISS);
    CHECK(put(s,key,png())==TileStoreResult::OK);
    fs.available=false; CHECK(s.beginGet(TileKey{0U,0U,0U},z)==TileStoreResult::STORAGE_UNAVAILABLE);
}
void testMalformedPngs() { beginTest(); FakeStorage fs; MapTileStore s(fs,config()); CHECK(s.initialize()==TileStoreResult::OK);
    std::vector<std::uint8_t> bad=png(); bad[0]=0U; CHECK(put(s,TileKey{0U,0U,0U},bad)==TileStoreResult::INVALID_PNG);
    CHECK(put(s,TileKey{0U,0U,0U},png(20U))==TileStoreResult::INVALID_PNG);
    CHECK(put(s,TileKey{0U,0U,0U},png(40U,255U,256U))==TileStoreResult::INVALID_PNG);
    CHECK(put(s,TileKey{0U,0U,0U},png(81U))==TileStoreResult::TOO_LARGE);
}
void testShortWriteAbortsTemp() { beginTest(); FakeStorage fs; MapTileStore s(fs,config()); CHECK(s.initialize()==TileStoreResult::OK); fs.short_write=true;
    CHECK(put(s,TileKey{0U,0U,0U},png())==TileStoreResult::IO_ERROR); CHECK(fs.files.empty());
}
void testExactQuotaAndLruEviction() { beginTest(); FakeStorage fs; MapTileStore s(fs,config(3U,80U,80U)); CHECK(s.initialize()==TileStoreResult::OK);
    CHECK(put(s,TileKey{1U,0U,0U},png())==TileStoreResult::OK); CHECK(put(s,TileKey{1U,1U,0U},png())==TileStoreResult::OK);
    std::uint32_t n=0U; CHECK(s.beginGet(TileKey{1U,0U,0U},n)==TileStoreResult::OK); s.endGet();
    CHECK(put(s,TileKey{1U,0U,1U},png())==TileStoreResult::OK); CHECK(s.beginGet(TileKey{1U,1U,0U},n)==TileStoreResult::MISS); CHECK(s.totalBytes()==80U);
}
void testDuplicateAtomicReplacement() { beginTest(); FakeStorage fs; MapTileStore s(fs,config(2U,100U,80U)); CHECK(s.initialize()==TileStoreResult::OK); TileKey k={0U,0U,0U};
    CHECK(put(s,k,png(40U))==TileStoreResult::OK); CHECK(put(s,k,png(60U))==TileStoreResult::OK); CHECK(s.entryCount()==1U); CHECK(s.totalBytes()==60U); drain(s,k,60U);
}
void testInterruptedFilesRecover() { beginTest(); FakeStorage fs; fs.add("/pyxis-map/tiles/1/0/0.png.bak",png(40U)); fs.add("/pyxis-map/tiles/1/1/0.png.tmp",png(40U));
    MapTileStore s(fs,config()); CHECK(s.initialize()==TileStoreResult::OK); CHECK(fs.find("/pyxis-map/tiles/1/0/0.png")>=0); CHECK(fs.find("/pyxis-map/tiles/1/1/0.png.tmp")<0); CHECK(s.entryCount()==1U);
}
void testLiveWinsRecovery() { beginTest(); FakeStorage fs; fs.add("/pyxis-map/tiles/0/0/0.png",png(40U)); fs.add("/pyxis-map/tiles/0/0/0.png.bak",png(60U)); fs.add("/pyxis-map/tiles/0/0/0.png.tmp",png(50U));
    MapTileStore s(fs,config()); CHECK(s.initialize()==TileStoreResult::OK); CHECK(fs.files.size()==1U); CHECK(s.totalBytes()==40U);
}
void testCorruptLiveRecoversValidBackup() { beginTest(); FakeStorage fs; std::vector<std::uint8_t> bad=png(40U); bad[0]=0U;
    fs.add("/pyxis-map/tiles/0/0/0.png",bad); fs.add("/pyxis-map/tiles/0/0/0.png.bak",png(60U)); MapTileStore s(fs,config());
    CHECK(s.initialize()==TileStoreResult::OK); CHECK(s.entryCount()==1U); CHECK(s.totalBytes()==60U); drain(s,TileKey{0U,0U,0U},60U);
}
void testCorruptLiveWithoutBackupIsRemoved() { beginTest(); FakeStorage fs; std::vector<std::uint8_t> bad=png(40U); bad[0]=0U;
    fs.add("/pyxis-map/tiles/0/0/0.png",bad); MapTileStore s(fs,config()); CHECK(s.initialize()==TileStoreResult::OK);
    CHECK(s.entryCount()==0U); CHECK(fs.files.empty());
}
void testStaleTempRemovalFailureAbortsPut() { beginTest(); FakeStorage fs; MapTileStore s(fs,config()); CHECK(s.initialize()==TileStoreResult::OK);
    fs.add("/pyxis-map/tiles/0/0/0.png.tmp",png()); fs.fail_remove=true;
    CHECK(s.beginPut(TileKey{0U,0U,0U})==TileStoreResult::IO_ERROR); CHECK(fs.find("/pyxis-map/tiles/0/0/0.png.tmp")>=0);
}
void testRecoveryRejectsMalformedAndExhaustion() { beginTest(); FakeStorage fs; fs.add("/pyxis-map/tiles/0/0/../evil.png",png()); MapTileStore a(fs,config()); CHECK(a.initialize()==TileStoreResult::INDEX_MISMATCH);
    FakeStorage fs2; fs2.add("/pyxis-map/tiles/1/0/0.png",png()); fs2.add("/pyxis-map/tiles/1/1/0.png",png()); MapTileStore b(fs2,config(1U,100U,80U)); CHECK(b.initialize()==TileStoreResult::INDEX_FULL);
}
void testRecoveryQuotaFailsClosed() { beginTest(); FakeStorage fs; fs.add("/pyxis-map/tiles/1/0/0.png",png(60U)); fs.add("/pyxis-map/tiles/1/1/0.png",png(60U)); MapTileStore s(fs,config(3U,100U,80U)); CHECK(s.initialize()==TileStoreResult::QUOTA_EXCEEDED); }
void testRenameFailureRestoresDuplicate() { beginTest(); FakeStorage fs; MapTileStore s(fs,config()); CHECK(s.initialize()==TileStoreResult::OK); TileKey k={0U,0U,0U}; CHECK(put(s,k,png())==TileStoreResult::OK);
    fs.fail_rename_call=fs.rename_calls+2; CHECK(put(s,k,png(50U))==TileStoreResult::IO_ERROR); drain(s,k,40U);
}
void testPromotionFailureDoesNotEvictVictims() { beginTest(); FakeStorage fs; MapTileStore s(fs,config(3U,80U,80U)); CHECK(s.initialize()==TileStoreResult::OK);
    const TileKey a={1U,0U,0U}, b={1U,1U,0U}, c={1U,0U,1U};
    CHECK(put(s,a,png())==TileStoreResult::OK); CHECK(put(s,b,png())==TileStoreResult::OK);
    fs.fail_rename_call=fs.rename_calls+1; CHECK(put(s,c,png())==TileStoreResult::IO_ERROR);
    CHECK(s.entryCount()==2U); CHECK(s.totalBytes()==80U); drain(s,a,40U); drain(s,b,40U);
}
void testEvictionPreflightFailurePreservesAllVictims() { beginTest(); FakeStorage fs; MapTileStore s(fs,config(3U,80U,80U)); CHECK(s.initialize()==TileStoreResult::OK);
    const TileKey a={1U,0U,0U}, b={1U,1U,0U}, c={1U,0U,1U};
    CHECK(put(s,a,png())==TileStoreResult::OK); CHECK(put(s,b,png())==TileStoreResult::OK);
    fs.fail_remove_path="/pyxis-map/tiles/.evict.txn.tmp"; CHECK(put(s,c,png(80U))==TileStoreResult::IO_ERROR); fs.fail_remove_path.clear();
    CHECK(s.entryCount()==2U); CHECK(s.totalBytes()==80U); drain(s,a,40U); drain(s,b,40U);
}
void testEvictionStageFailureRollsBackAllVictims() { beginTest(); FakeStorage fs; MapTileStore s(fs,config(3U,80U,80U)); CHECK(s.initialize()==TileStoreResult::OK);
    const TileKey a={1U,0U,0U}, b={1U,1U,0U}, c={1U,0U,1U};
    CHECK(put(s,a,png())==TileStoreResult::OK); CHECK(put(s,b,png())==TileStoreResult::OK);
    fs.fail_rename_call=fs.rename_calls+4; CHECK(put(s,c,png(80U))==TileStoreResult::IO_ERROR);
    CHECK(s.entryCount()==2U); CHECK(s.totalBytes()==80U); drain(s,a,40U); drain(s,b,40U);
}
void testEvictionPowerCutsRestoreWholeOldGeneration() { beginTest();
    for (int cut=2;cut<=4;++cut) { FakeStorage fs; MapTileStore s(fs,config(3U,80U,80U)); CHECK(s.initialize()==TileStoreResult::OK);
        const TileKey a={1U,0U,0U}, b={1U,1U,0U}, c={1U,0U,1U};
        CHECK(put(s,a,png())==TileStoreResult::OK); CHECK(put(s,b,png())==TileStoreResult::OK);
        fs.power_cut_after_rename_call=fs.rename_calls+cut;
        CHECK(put(s,c,png(80U))!=TileStoreResult::OK);
        fs.available=true; fs.power_cut_after_rename_call=0;
        MapTileStore recovered(fs,config(3U,80U,80U)); CHECK(recovered.initialize()==TileStoreResult::OK);
        CHECK(recovered.entryCount()==2U); CHECK(recovered.totalBytes()==80U);
        drain(recovered,a,40U); drain(recovered,b,40U);
    }
}
void testDuplicateEvictionPowerCutsRestoreOldCandidateAndVictim() { beginTest();
    for (int cut=1;cut<=4;++cut) { FakeStorage fs; MapTileStore s(fs,config(2U,80U,80U)); CHECK(s.initialize()==TileStoreResult::OK);
        const TileKey a={1U,0U,0U}, b={1U,1U,0U};
        CHECK(put(s,a,png())==TileStoreResult::OK); CHECK(put(s,b,png())==TileStoreResult::OK);
        fs.power_cut_after_rename_call=fs.rename_calls+cut;
        CHECK(put(s,a,png(80U))!=TileStoreResult::OK);
        fs.available=true; fs.power_cut_after_rename_call=0;
        MapTileStore recovered(fs,config(2U,80U,80U)); CHECK(recovered.initialize()==TileStoreResult::OK);
        CHECK(recovered.entryCount()==2U); CHECK(recovered.totalBytes()==80U);
        drain(recovered,a,40U); drain(recovered,b,40U);
    }
}
void testMalformedEvictionManifestFailsClosed() { beginTest(); FakeStorage fs;
    fs.add("/pyxis-map/tiles/.evict.txn",std::vector<std::uint8_t>(21U,0U));
    MapTileStore s(fs,config()); CHECK(s.initialize()==TileStoreResult::INDEX_MISMATCH);
}
void testDeterministicStress() { beginTest(); FakeStorage fs; MapTileStore s(fs,config(3U,120U,80U)); CHECK(s.initialize()==TileStoreResult::OK); CHECK(put(s,TileKey{2U,0U,0U},png())==TileStoreResult::OK);
    std::uint32_t size=0U; for(std::uint32_t i=0U;i<100000U;++i) { const TileKey k={2U,i&3U,(i>>2)&3U}; TileStoreResult r=s.beginGet(k,size); CHECK(r==TileStoreResult::OK||r==TileStoreResult::MISS); if(r==TileStoreResult::OK)s.endGet(); }
}
}
int main() { testKeyAndCanonicalPath(); testMissHitAndRemoval(); testMalformedPngs(); testShortWriteAbortsTemp(); testExactQuotaAndLruEviction(); testDuplicateAtomicReplacement(); testInterruptedFilesRecover(); testLiveWinsRecovery(); testCorruptLiveRecoversValidBackup(); testCorruptLiveWithoutBackupIsRemoved(); testStaleTempRemovalFailureAbortsPut(); testRecoveryRejectsMalformedAndExhaustion(); testRecoveryQuotaFailsClosed(); testRenameFailureRestoresDuplicate(); testPromotionFailureDoesNotEvictVictims(); testEvictionPreflightFailurePreservesAllVictims(); testEvictionStageFailureRollsBackAllVictims(); testEvictionPowerCutsRestoreWholeOldGeneration(); testDuplicateEvictionPowerCutsRestoreOldCandidateAndVictim(); testMalformedEvictionManifestFailsClosed(); testDeterministicStress(); std::cout<<"map tile store: "<<tests_run<<" tests passed\n"; }
