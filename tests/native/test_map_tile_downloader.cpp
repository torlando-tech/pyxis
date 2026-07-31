#include "Hardware/TDeck/MapTileDownloader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace Hardware::TDeck;

namespace {
std::size_t tests_run = 0U;
void fail(const char* expression, int line) { std::cerr << "line " << line << ": " << expression << '\n'; std::exit(1); }
#define CHECK(e) do { if (!(e)) fail(#e, __LINE__); } while (false)
void beginTest() { ++tests_run; }
bool same(const TileKey& a, const TileKey& b) { return a.zoom == b.zoom && a.x == b.x && a.y == b.y; }

class FakeClock : public MapTileDownloadClock {
public:
    std::uint64_t value;
    FakeClock() : value(100U) {}
    virtual std::uint64_t nowMs() const { return value; }
};

class FakeStore : public MapTileDownloadStore {
public:
    std::uint32_t maximum;
    bool available;
    TileStoreResult begin_result;
    TileStoreResult write_result;
    TileStoreResult finish_result;
    bool short_write;
    bool open;
    int begins;
    int finishes;
    int aborts;
    std::vector<std::size_t> chunks;
    std::vector<std::uint8_t> bytes;
    FakeStore() : maximum(9000U), available(true), begin_result(TileStoreResult::OK),
        write_result(TileStoreResult::OK), finish_result(TileStoreResult::OK), short_write(false),
        open(false), begins(0), finishes(0), aborts(0) {}
    virtual bool isAvailable() const { return available; }
    virtual std::uint32_t maxTileBytes() const { return maximum; }
    virtual TileStoreResult beginPut(const TileKey&) { ++begins; if (begin_result == TileStoreResult::OK) open = true; return begin_result; }
    virtual TileStoreResult writePutChunk(const std::uint8_t* data, std::size_t size) {
        if (write_result != TileStoreResult::OK) return write_result;
        chunks.push_back(size);
        const std::size_t written = (short_write && size != 0U) ? size - 1U : size;
        bytes.insert(bytes.end(), data, data + written);
        return short_write ? TileStoreResult::IO_ERROR : TileStoreResult::OK;
    }
    virtual TileStoreResult finishPut() { ++finishes; if (finish_result == TileStoreResult::OK) open = false; return finish_result; }
    virtual void abortPut() { ++aborts; open = false; bytes.clear(); }
};

class FakeTransport : public MapTileTransport {
public:
    TileTransportResult start_result;
    std::vector<TileTransportResult> start_results;
    TileTransportResult read_result;
    int status;
    std::int64_t length;
    const char* type;
    std::vector<std::uint8_t> body;
    std::size_t position;
    std::size_t forced_chunk;
    int starts;
    int reads;
    int closes;
    std::string url;
    std::string agent;
    std::string ca;
    std::uint32_t connect_timeout;
    std::uint32_t read_timeout;
    FakeClock* clock;
    std::uint64_t advance_on_start;
    std::uint64_t advance_on_read;
    FakeTransport() : start_result(TileTransportResult::OK), read_result(TileTransportResult::OK),
        status(200), length(-1), type("image/png"), position(0U), forced_chunk(0U), starts(0), reads(0), closes(0),
        connect_timeout(0U), read_timeout(0U), clock(NULL), advance_on_start(0U), advance_on_read(0U) {}
    virtual TileTransportResult start(const char* u, const char* a, const char* c,
                                      std::uint32_t ct, std::uint32_t rt, TileHttpResponse& response) {
        ++starts; url = u == NULL ? "" : u; agent = a == NULL ? "" : a; ca = c == NULL ? "" : c;
        if (clock != NULL) clock->value += advance_on_start;
        connect_timeout = ct; read_timeout = rt; position = 0U;
        response.status_code = status; response.content_length = length; response.content_type = type;
        const std::size_t attempt = static_cast<std::size_t>(starts - 1);
        return attempt < start_results.size() ? start_results[attempt] : start_result;
    }
    virtual TileTransportResult read(std::uint8_t* output, std::size_t capacity, std::size_t& count, bool& eof) {
        ++reads;
        if (clock != NULL) clock->value += advance_on_read;
        if (read_result != TileTransportResult::OK) { count = 0U; eof = false; return read_result; }
        std::size_t amount = std::min(capacity, body.size() - position);
        if (forced_chunk != 0U) amount = std::min(amount, forced_chunk);
        if (amount != 0U) std::memcpy(output, &body[position], amount);
        position += amount; count = amount; eof = position == body.size(); return TileTransportResult::OK;
    }
    virtual void close() { ++closes; }
};

MapTileDownloadConfig config(const char* endpoint = "https://tile.openstreetmap.org") {
    MapTileDownloadConfig c;
    c.endpoint = endpoint; c.ca_certificate = "TEST CA"; c.firmware_version = "1.2.3";
    c.overall_timeout_ms = 10000U; c.connect_timeout_ms = 321U; c.read_timeout_ms = 654U;
    return c;
}
MapTileDownloadPolicy enabled() { MapTileDownloadPolicy p; p.enabled = true; return p; }
TileKey key(std::uint32_t x = 1U) { TileKey k = {3U, x, 2U}; return k; }
std::vector<std::uint8_t> bytes(std::size_t n) { return std::vector<std::uint8_t>(n, 42U); }
void runUntilIdle(MapTileDownloader& d, FakeClock& clock, int limit = 40) {
    for (int i = 0; i < limit && d.isBusy(); ++i) { CHECK(d.pump() == MapTilePumpResult::PROGRESSED); ++clock.value; }
    CHECK(!d.isBusy());
}
MapTileDownloadResult take(MapTileDownloader& d) { MapTileDownloadResult r; CHECK(d.takeResult(r)); return r; }

void testDisabledByDefault() { beginTest(); FakeStore s; FakeTransport t; FakeClock c; MapTileDownloadPolicy p; MapTileDownloader d(s,t,c,p,config());
    CHECK(!p.enabled); CHECK(d.enqueue(key(),7U)==MapTileEnqueueResult::POLICY_DISABLED); CHECK(d.queuedCount()==0U); CHECK(t.starts==0); }
void testCanonicalUrlAndBounds() { beginTest(); char out[MapTileDownloader::URL_CAPACITY];
    CHECK(MapTileDownloader::canonicalUrl("https://tile.openstreetmap.org/",TileKey{22U,4194303U,4194303U},out,sizeof(out))==MapTileUrlResult::OK);
    CHECK(std::string(out)=="https://tile.openstreetmap.org/22/4194303/4194303.png");
    char tiny[12]; std::memset(tiny,'X',sizeof(tiny)); CHECK(MapTileDownloader::canonicalUrl("https://tile.openstreetmap.org",key(),tiny,sizeof(tiny))==MapTileUrlResult::TOO_LONG); CHECK(tiny[0]=='\0');
    CHECK(MapTileDownloader::canonicalUrl("https://tile.openstreetmap.org",TileKey{23U,0U,0U},out,sizeof(out))==MapTileUrlResult::INVALID_KEY);
    CHECK(MapTileDownloader::canonicalUrl("http://tile.openstreetmap.org",key(),out,sizeof(out))==MapTileUrlResult::INVALID_ARGUMENT);
    CHECK(MapTileDownloader::canonicalUrl("https://user@tile.example",key(),out,sizeof(out))==MapTileUrlResult::INVALID_ARGUMENT);
    std::string huge("https://"); huge.append(MapTileDownloader::URL_CAPACITY,'a'); CHECK(MapTileDownloader::canonicalUrl(huge.c_str(),key(),out,sizeof(out))==MapTileUrlResult::TOO_LONG); }
void testDedupeAndQueueFullNoEviction() { beginTest(); FakeStore s; FakeTransport t; FakeClock c; MapTileDownloader d(s,t,c,enabled(),config());
    for(std::uint32_t i=0;i<MapTileDownloader::QUEUE_CAPACITY;++i) CHECK(d.enqueue(key(i),9U)==MapTileEnqueueResult::ACCEPTED);
    CHECK(d.enqueue(key(2U),9U)==MapTileEnqueueResult::DUPLICATE); CHECK(d.enqueue(key(0U),10U)==MapTileEnqueueResult::DUPLICATE);
    CHECK(d.enqueue(TileKey{3U,7U,7U},9U)==MapTileEnqueueResult::QUEUE_FULL); CHECK(d.queuedCount()==MapTileDownloader::QUEUE_CAPACITY); }
void testSuccessExactChunksAndPublicContract() { beginTest(); FakeStore s; s.maximum=8192U; FakeTransport t; t.body=bytes(8192U); t.length=8192; FakeClock c; MapTileDownloader d(s,t,c,enabled(),config());
    CHECK(d.enqueue(key(),11U)==MapTileEnqueueResult::ACCEPTED); runUntilIdle(d,c); MapTileDownloadResult r=take(d);
    CHECK(r.code==MapTileResultCode::SUCCESS && r.bytes==8192U && same(r.key,key()) && r.generation==11U);
    CHECK(s.chunks.size()==2U && s.chunks[0]==4096U && s.chunks[1]==4096U); CHECK(t.url=="https://tile.openstreetmap.org/3/1/2.png");
    CHECK(t.agent=="Pyxis/1.2.3 (+https://github.com/torlando-tech/pyxis)"); CHECK(t.ca=="TEST CA"); CHECK(t.connect_timeout==321U && t.read_timeout==654U); CHECK(t.starts==1 && t.closes==1); }
void testStatusAndContentTypeFailures() { beginTest(); const char* bad[]={"text/plain","image/pngx","image/ png",NULL};
    for(int i=0;i<4;++i){ FakeStore s; FakeTransport t; t.type=bad[i]; FakeClock c; MapTileDownloader d(s,t,c,enabled(),config()); CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); runUntilIdle(d,c); CHECK(take(d).code==MapTileResultCode::CONTENT_TYPE_ERROR); CHECK(s.begins==0); }
    FakeStore s; FakeTransport t; t.status=204; FakeClock c; MapTileDownloader d(s,t,c,enabled(),config()); CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); runUntilIdle(d,c); CHECK(take(d).code==MapTileResultCode::HTTP_STATUS_ERROR); CHECK(s.begins==0);
    FakeStore s2; FakeTransport t2; t2.type="IMAGE/PNG; charset=binary"; t2.body=bytes(24U); FakeClock c2; MapTileDownloader d2(s2,t2,c2,enabled(),config()); CHECK(d2.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); runUntilIdle(d2,c2); CHECK(take(d2).code==MapTileResultCode::SUCCESS); }
void testLengthOverUnderAndChunkOverCap() { beginTest();
    { FakeStore s; s.maximum=40U; FakeTransport t; t.length=41; FakeClock c; MapTileDownloader d(s,t,c,enabled(),config()); CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); runUntilIdle(d,c); CHECK(take(d).code==MapTileResultCode::TOO_LARGE); CHECK(s.begins==0); }
    { FakeStore s; FakeTransport t; t.length=30; t.body=bytes(29U); FakeClock c; MapTileDownloader d(s,t,c,enabled(),config()); CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); runUntilIdle(d,c); CHECK(take(d).code==MapTileResultCode::LENGTH_MISMATCH); CHECK(s.aborts==1); }
    { FakeStore s; s.maximum=4096U; FakeTransport t; t.length=-1; t.body=bytes(4097U); FakeClock c; MapTileDownloader d(s,t,c,enabled(),config()); CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); runUntilIdle(d,c); CHECK(take(d).code==MapTileResultCode::TOO_LARGE); CHECK(s.aborts==1); CHECK(s.bytes.empty()); }
}
void testTransportAndStoreFailuresAbort() { beginTest();
    { FakeStore s; FakeTransport t; t.start_result=TileTransportResult::ERROR; FakeClock c; MapTileDownloader d(s,t,c,enabled(),config()); CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); runUntilIdle(d,c); CHECK(take(d).code==MapTileResultCode::TRANSPORT_ERROR); CHECK(s.aborts==0); }
    { FakeStore s; FakeTransport t; t.body=bytes(30U); t.read_result=TileTransportResult::ERROR; FakeClock c; MapTileDownloader d(s,t,c,enabled(),config()); CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); runUntilIdle(d,c); CHECK(take(d).code==MapTileResultCode::READ_ERROR); CHECK(s.aborts==1); }
    { FakeStore s; s.short_write=true; FakeTransport t; t.body=bytes(30U); FakeClock c; MapTileDownloader d(s,t,c,enabled(),config()); CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); runUntilIdle(d,c); CHECK(take(d).code==MapTileResultCode::STORE_ERROR); CHECK(s.aborts==1); }
    { FakeStore s; s.finish_result=TileStoreResult::IO_ERROR; FakeTransport t; t.body=bytes(30U); FakeClock c; MapTileDownloader d(s,t,c,enabled(),config()); CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); runUntilIdle(d,c); CHECK(take(d).code==MapTileResultCode::STORE_ERROR); CHECK(s.aborts==1); }
}
void testTransportStartFailureHardClosesAndRetriesOnce() { beginTest();
    FakeStore s; FakeTransport t; t.start_results.push_back(TileTransportResult::ERROR);
    t.start_results.push_back(TileTransportResult::OK); t.body=bytes(30U);
    FakeClock c; MapTileDownloader d(s,t,c,enabled(),config());
    CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); runUntilIdle(d,c);
    CHECK(take(d).code==MapTileResultCode::SUCCESS); CHECK(t.starts==2); CHECK(t.closes==2);
}
void testTransportStartRetryIsBounded() { beginTest();
    FakeStore s; FakeTransport t; t.start_result=TileTransportResult::ERROR;
    FakeClock c; MapTileDownloader d(s,t,c,enabled(),config());
    CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); runUntilIdle(d,c);
    CHECK(take(d).code==MapTileResultCode::TRANSPORT_ERROR); CHECK(t.starts==2); CHECK(t.closes==2);
}
void testTransportStartTimeoutIsNotRetried() { beginTest();
    FakeStore s; FakeTransport t; t.start_result=TileTransportResult::TIMEOUT;
    FakeClock c; MapTileDownloader d(s,t,c,enabled(),config());
    CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); runUntilIdle(d,c);
    CHECK(take(d).code==MapTileResultCode::TIMEOUT); CHECK(t.starts==1); CHECK(t.closes==1);
}
void testTransportRetryHonorsOverallDeadline() { beginTest();
    FakeStore s; FakeTransport t; t.start_result=TileTransportResult::ERROR;
    FakeClock c; t.clock=&c; t.advance_on_start=20U; MapTileDownloadConfig cfg=config();
    cfg.overall_timeout_ms=10U; MapTileDownloader d(s,t,c,enabled(),cfg);
    CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); runUntilIdle(d,c);
    CHECK(take(d).code==MapTileResultCode::TIMEOUT); CHECK(t.starts==1); CHECK(t.closes==1);
}
void testSecondTransportFailureHonorsOverallDeadline() { beginTest();
    FakeStore s; FakeTransport t; t.start_result=TileTransportResult::ERROR;
    FakeClock c; t.clock=&c; t.advance_on_start=6U; MapTileDownloadConfig cfg=config();
    cfg.overall_timeout_ms=10U; MapTileDownloader d(s,t,c,enabled(),cfg);
    CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); runUntilIdle(d,c);
    CHECK(take(d).code==MapTileResultCode::TIMEOUT); CHECK(t.starts==2); CHECK(t.closes==2);
}
void testTransportRetryCanBeCanceledBetweenAttempts() { beginTest();
    FakeStore s; FakeTransport t; t.start_result=TileTransportResult::ERROR;
    FakeClock c; MapTileDownloader d(s,t,c,enabled(),config());
    CHECK(d.enqueue(key(),5U)==MapTileEnqueueResult::ACCEPTED);
    CHECK(d.pump()==MapTilePumpResult::PROGRESSED);
    CHECK(d.pump()==MapTilePumpResult::PROGRESSED); CHECK(t.starts==1); CHECK(t.closes==1);
    CHECK(d.cancelGeneration(5U)==1U); runUntilIdle(d,c);
    CHECK(take(d).code==MapTileResultCode::CANCELED); CHECK(t.starts==1);
}
void testTransportRetryBudgetResetsForNextRequest() { beginTest();
    FakeStore s; FakeTransport t; t.start_results.push_back(TileTransportResult::ERROR);
    t.start_results.push_back(TileTransportResult::OK); t.start_results.push_back(TileTransportResult::ERROR);
    t.start_results.push_back(TileTransportResult::OK); t.body=bytes(30U);
    FakeClock c; MapTileDownloader d(s,t,c,enabled(),config());
    CHECK(d.enqueue(key(1U),1U)==MapTileEnqueueResult::ACCEPTED);
    CHECK(d.enqueue(key(2U),2U)==MapTileEnqueueResult::ACCEPTED); runUntilIdle(d,c);
    CHECK(take(d).code==MapTileResultCode::SUCCESS); CHECK(take(d).code==MapTileResultCode::SUCCESS);
    CHECK(t.starts==4); CHECK(t.closes==4);
}
void testCancellationAtStagesAndGenerationIsolation() { beginTest();
    for(int stage=0;stage<3;++stage){ FakeStore s; FakeTransport t; t.body=bytes(5000U); FakeClock c; MapTileDownloader d(s,t,c,enabled(),config()); CHECK(d.enqueue(key(),5U)==MapTileEnqueueResult::ACCEPTED); for(int i=0;i<stage;++i) CHECK(d.pump()==MapTilePumpResult::PROGRESSED); CHECK(d.cancelGeneration(5U)==1U); runUntilIdle(d,c); CHECK(take(d).code==MapTileResultCode::CANCELED); if(s.begins!=0) CHECK(s.aborts==1); }
    FakeStore s; FakeTransport t; FakeClock c; MapTileDownloader d(s,t,c,enabled(),config()); CHECK(d.enqueue(key(0U),1U)==MapTileEnqueueResult::ACCEPTED); CHECK(d.enqueue(key(1U),2U)==MapTileEnqueueResult::ACCEPTED); CHECK(d.cancelGeneration(1U)==1U); CHECK(d.queuedCount()==1U); CHECK(take(d).code==MapTileResultCode::CANCELED); }
void testTimeoutRollbackAndSaturation() { beginTest();
    { FakeStore s; FakeTransport t; t.body=bytes(5000U); t.forced_chunk=1U; FakeClock c; MapTileDownloadConfig cfg=config(); cfg.overall_timeout_ms=5U; MapTileDownloader d(s,t,c,enabled(),cfg); CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); CHECK(d.pump()==MapTilePumpResult::PROGRESSED); ++c.value; CHECK(d.pump()==MapTilePumpResult::PROGRESSED); ++c.value; CHECK(d.pump()==MapTilePumpResult::PROGRESSED); c.value+=4U; runUntilIdle(d,c); CHECK(take(d).code==MapTileResultCode::TIMEOUT); CHECK(s.aborts==1); }
    { FakeStore s; FakeTransport t; t.body=bytes(30U); FakeClock c; MapTileDownloader d(s,t,c,enabled(),config()); CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); CHECK(d.pump()==MapTilePumpResult::PROGRESSED); --c.value; runUntilIdle(d,c); CHECK(take(d).code==MapTileResultCode::CLOCK_ERROR); }
    { FakeStore s; FakeTransport t; t.body=bytes(30U); FakeClock c; c.value=UINT64_MAX-2U; MapTileDownloadConfig cfg=config(); cfg.overall_timeout_ms=100U; MapTileDownloader d(s,t,c,enabled(),cfg); CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); for(int i=0;i<8 && d.isBusy();++i) CHECK(d.pump()==MapTilePumpResult::PROGRESSED); CHECK(!d.isBusy()); CHECK(take(d).code==MapTileResultCode::SUCCESS); CHECK(d.lastDeadline()==UINT64_MAX); }
    { FakeStore s; FakeTransport t; t.body=bytes(30U); FakeClock c; t.clock=&c; t.advance_on_read=20U; MapTileDownloadConfig cfg=config(); cfg.overall_timeout_ms=10U; MapTileDownloader d(s,t,c,enabled(),cfg); CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); runUntilIdle(d,c); CHECK(take(d).code==MapTileResultCode::TIMEOUT); CHECK(s.aborts==1); CHECK(s.bytes.empty()); }
}
void testDestructorAbortsOwnedResources() { beginTest(); FakeStore s; FakeTransport t; t.body=bytes(5000U); FakeClock c;
    { MapTileDownloader d(s,t,c,enabled(),config()); CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); CHECK(d.pump()==MapTilePumpResult::PROGRESSED); CHECK(d.pump()==MapTilePumpResult::PROGRESSED); CHECK(d.pump()==MapTilePumpResult::PROGRESSED); CHECK(s.open); }
    CHECK(s.aborts==1); CHECK(t.closes==1); CHECK(!s.open);
}
void testRuntimeDisableCancelsAllWork() { beginTest(); FakeStore s; FakeTransport t; t.body=bytes(5000U); FakeClock c; MapTileDownloader d(s,t,c,enabled(),config());
    CHECK(d.enqueue(key(1U),7U)==MapTileEnqueueResult::ACCEPTED); CHECK(d.enqueue(key(2U),8U)==MapTileEnqueueResult::ACCEPTED);
    CHECK(d.pump()==MapTilePumpResult::PROGRESSED); CHECK(d.pump()==MapTilePumpResult::PROGRESSED); CHECK(d.pump()==MapTilePumpResult::PROGRESSED); CHECK(s.open);
    d.setEnabled(false); runUntilIdle(d,c); CHECK(s.aborts==1); CHECK(t.closes==1); CHECK(d.queuedCount()==0U);
    CHECK(take(d).code==MapTileResultCode::CANCELED); CHECK(take(d).code==MapTileResultCode::CANCELED);
    CHECK(d.enqueue(key(),9U)==MapTileEnqueueResult::POLICY_DISABLED);
}
void testSdDisappearanceAndMailboxBound() { beginTest(); FakeStore s; FakeTransport t; t.body=bytes(5000U); FakeClock c; MapTileDownloader d(s,t,c,enabled(),config()); CHECK(d.enqueue(key(),1U)==MapTileEnqueueResult::ACCEPTED); CHECK(d.pump()==MapTilePumpResult::PROGRESSED); CHECK(d.pump()==MapTilePumpResult::PROGRESSED); s.available=false; runUntilIdle(d,c); CHECK(take(d).code==MapTileResultCode::STORE_UNAVAILABLE);
    FakeStore s2; FakeTransport t2; FakeClock c2; MapTileDownloader d2(s2,t2,c2,enabled(),config()); for(std::uint32_t i=0;i<6U;++i) CHECK(d2.enqueue(key(i),i)==MapTileEnqueueResult::ACCEPTED); for(std::uint32_t i=0;i<6U;++i) CHECK(d2.cancelGeneration(i)==1U); CHECK(d2.resultCount()==MapTileDownloader::RESULT_CAPACITY); CHECK(d2.droppedResultCount()==0U); }
void testStress() { beginTest(); FakeStore s; FakeTransport t; FakeClock c; MapTileDownloader d(s,t,c,enabled(),config());
    for(std::uint32_t i=0;i<100000U;++i){ TileKey k=key(i&3U); const std::uint32_t g=i&7U; MapTileEnqueueResult r=d.enqueue(k,g); CHECK(r==MapTileEnqueueResult::ACCEPTED||r==MapTileEnqueueResult::DUPLICATE||r==MapTileEnqueueResult::QUEUE_FULL); if((i&3U)==0U)d.cancelGeneration(g); MapTileDownloadResult ignored; while(d.takeResult(ignored)){} } CHECK(d.queuedCount()<=MapTileDownloader::QUEUE_CAPACITY); }
}
int main(){ testDisabledByDefault(); testCanonicalUrlAndBounds(); testDedupeAndQueueFullNoEviction(); testSuccessExactChunksAndPublicContract(); testStatusAndContentTypeFailures(); testLengthOverUnderAndChunkOverCap(); testTransportAndStoreFailuresAbort(); testTransportStartFailureHardClosesAndRetriesOnce(); testTransportStartRetryIsBounded(); testTransportStartTimeoutIsNotRetried(); testTransportRetryHonorsOverallDeadline(); testSecondTransportFailureHonorsOverallDeadline(); testTransportRetryCanBeCanceledBetweenAttempts(); testTransportRetryBudgetResetsForNextRequest(); testCancellationAtStagesAndGenerationIsolation(); testTimeoutRollbackAndSaturation(); testDestructorAbortsOwnedResources(); testRuntimeDisableCancelsAllWork(); testSdDisappearanceAndMailboxBound(); testStress(); std::cout<<"map tile downloader: "<<tests_run<<" tests passed\n"; }
