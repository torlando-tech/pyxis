#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include "NomadNetCache.h"

using namespace UI::LXMF::NomadNet;
struct MemoryStorage final:NomadNetStorage{
 std::map<std::string,std::vector<uint8_t>> files;std::string active,remove_fail_substring,read_fail_path,stat_fail_path;size_t pos=0;bool writing=false;bool available=true,busy=false,full=false;size_t max_write=SIZE_MAX;int fail_rename_at=0,renames=0,commit_calls=0,abort_calls=0;mutable size_t operations=0;StorageResult remove_result=StorageResult::OK,read_fail_result=StorageResult::OK,stat_fail_result=StorageResult::OK;int remove_failures=0,end_read_failures=0,abort_failures=0;StorageResult list_result=StorageResult::OK;int next_list_fail_at=0;
 bool isAvailable()const override{++operations;return available;}
 StorageResult beginRead(const char*n,uint32_t&s)override{++operations;if(n==read_fail_path&&read_fail_result!=StorageResult::OK)return read_fail_result;if(!available)return StorageResult::UNAVAILABLE;if(busy)return StorageResult::BUSY;auto i=files.find(n);if(i==files.end())return StorageResult::MISS;active=n;pos=0;s=i->second.size();return StorageResult::OK;}
 StorageResult readChunk(uint8_t*out,size_t cap,size_t&n)override{++operations;if(busy){n=0;return StorageResult::BUSY;}auto&i=files[active];n=std::min(cap,i.size()-pos);if(n)std::memcpy(out,i.data()+pos,n);pos+=n;return StorageResult::OK;}
 StorageResult endRead()override{++operations;if(end_read_failures-->0)return StorageResult::BUSY;active.clear();return StorageResult::OK;}
 StorageResult beginWrite(const char*n)override{++operations;if(!available)return StorageResult::UNAVAILABLE;if(busy)return StorageResult::BUSY;if(full)return StorageResult::FULL;active=n;files[active].clear();writing=true;return StorageResult::OK;}
 StorageResult writeChunk(const uint8_t*d,size_t z,size_t&n)override{++operations;if(full){n=0;return StorageResult::FULL;}n=std::min(z,max_write);files[active].insert(files[active].end(),d,d+n);return n==z?StorageResult::OK:(n?StorageResult::PARTIAL_WRITE:StorageResult::NO_PROGRESS);}
 StorageResult commitWrite()override{++operations;++commit_calls;writing=false;active.clear();return StorageResult::OK;}
 StorageResult abortWrite()override{++operations;++abort_calls;if(abort_failures-->0)return StorageResult::BUSY;if(writing)files.erase(active);writing=false;active.clear();return StorageResult::OK;}
 StorageResult remove(const char*n)override{++operations;if(remove_failures>0&&(remove_fail_substring.empty()||std::string(n).find(remove_fail_substring)!=std::string::npos)){--remove_failures;return remove_result;}return files.erase(n)?StorageResult::OK:StorageResult::MISS;}
 StorageResult rename(const char*a,const char*b)override{++operations;if(fail_rename_at&&++renames==fail_rename_at)return StorageResult::IO_ERROR;auto i=files.find(a);if(i==files.end())return StorageResult::MISS;if(files.count(b))return StorageResult::INVALID_STATE;files[b]=i->second;files.erase(i);return StorageResult::OK;}
 StorageResult stat(const char*n,uint32_t&s)override{++operations;if(n==stat_fail_path&&stat_fail_result!=StorageResult::OK)return stat_fail_result;auto i=files.find(n);if(i==files.end())return StorageResult::MISS;s=i->second.size();return StorageResult::OK;}
 StorageResult beginList(const char*d)override{++operations;list.clear();for(auto&f:files)if(f.first.rfind(std::string(d)+"/",0)==0)list.push_back(f.first);li=0;return available?list_result:StorageResult::UNAVAILABLE;}
 StorageResult nextList(char*n,size_t c,bool&done)override{++operations;if(next_list_fail_at&&static_cast<int>(li+1)==next_list_fail_at)return StorageResult::IO_ERROR;if(li==list.size()){done=true;return StorageResult::OK;}done=false;if(list[li].size()+1>c){++li;return StorageResult::TOO_LARGE;}std::memcpy(n,list[li].c_str(),list[li].size()+1);++li;return StorageResult::OK;}
 StorageResult endList()override{++operations;return StorageResult::OK;}std::vector<std::string>list;size_t li=0;
};
static CacheKey key(const char*path="/page/index.mu"){return CacheKey{"0123456789abcdef0123456789abcdef",path,RequestDataClass::NIL};}
static void drain(NomadNetCache&c,MemoryStorage*s=nullptr){for(int i=0;i<1000&&c.busy();++i){const auto before=s?s->operations:0;c.service();if(s&&s->operations-before>1)throw std::runtime_error("more than one storage operation per service tick");}}
int main(){int f=0;auto ck=[&](bool x,const char*n){if(!x){++f;std::cerr<<"FAIL "<<n<<"\n";}};MemoryStorage s;CacheConfig cfg;cfg.max_entries=2;cfg.max_bytes=4096;cfg.max_scan_records=8;NomadNetCache c(s,cfg);drain(c,&s);const std::vector<uint8_t> body={'h','e','l','l','o'};
 ck(canonical_cache_key(key())=="0123456789abcdef0123456789abcdef\n/page/index.mu\nnil","canonical key");
 ck(cache_directive_ttl(body.data(),body.size())==CacheConfig::DEFAULT_TTL_SECONDS,"default 12h");const char no[]="#!c=0\nhello";ck(cache_directive_ttl((const uint8_t*)no,sizeof(no)-1)==0,"c zero disables");
 ck(cache_eligible({true,true,false,false,false,false,RequestDataClass::NIL}),"ordinary success eligible");ck(!cache_eligible({true,true,false,false,false,false,RequestDataClass::FIELDS}),"request data bypass");ck(!cache_eligible({true,true,true,false,false,false,RequestDataClass::NIL}),"partial bypass");ck(!cache_eligible({true,false,false,false,false,false,RequestDataClass::NIL}),"malformed bypass");
 ck(c.beginCommit(key(),body,100,60)==CacheResult::PENDING,"begin commit");drain(c,&s);ck(c.lastResult()==CacheResult::STORED,"commit stored");
 ck(c.beginLookup(key(),159)==CacheResult::PENDING,"lookup start");drain(c,&s);ExternalVector<uint8_t> got;ck(c.takeBody(got)&&std::equal(got.begin(),got.end(),body.begin(),body.end())&&c.lastResult()==CacheResult::HIT,"fresh hit uses external ownership");
 ck(c.beginLookup(key(),160)==CacheResult::PENDING,"ttl boundary start");drain(c);ck(c.lastResult()==CacheResult::EXPIRED,"ttl exact boundary expired");
 ck(c.beginCommit(key(),body,100,60)==CacheResult::PENDING,"restore");drain(c);ck(c.beginLookup(key(),99)==CacheResult::PENDING,"back clock");drain(c);ck(c.lastResult()==CacheResult::MISS,"backward clock conservative miss");
 ck(c.beginLookup(key(),0)==CacheResult::PENDING,"invalid clock");drain(c);ck(c.lastResult()==CacheResult::MISS,"invalid clock miss");
 CacheKey form_key=key();form_key.request_data=RequestDataClass::FORM;const std::vector<uint8_t> empty_map={0x80};ck(c.beginLookup(form_key,159)==CacheResult::BYPASS&&empty_map.front()==0x80,"empty-map form request bypasses cache");
 // Corrupt newest metadata and require prior generation fallback.
 ck(c.beginCommit(key(),std::vector<uint8_t>{'o','l','d'},200,60)==CacheResult::PENDING,"old gen");drain(c);ck(c.beginCommit(key(),std::vector<uint8_t>{'n','e','w'},201,60)==CacheResult::PENDING,"new gen");drain(c);auto newest=c.debugMetadataPath(key(),c.debugGeneration());s.files[newest].resize(5);ck(c.beginLookup(key(),202)==CacheResult::PENDING,"corrupt lookup");drain(c);got.clear();const std::vector<uint8_t> old={'o','l','d'};ck(c.takeBody(got)&&std::equal(got.begin(),got.end(),old.begin(),old.end()),"corrupt newest falls back");
 // Truncated body and key collision metadata are rejected.
 auto bp=c.debugBodyPath(key(),c.debugGeneration());s.files[bp].resize(1);ck(c.beginLookup(key(),202)==CacheResult::PENDING,"truncated lookup");drain(c);ck(c.lastResult()!=CacheResult::HIT,"truncated rejected");
 CacheKey collision=key("/page/other.mu");s.files[c.debugMetadataPath(collision,0)]=s.files[c.debugMetadataPath(key(),0)];ck(c.beginLookup(collision,202)==CacheResult::PENDING,"collision lookup");drain(c);ck(c.lastResult()!=CacheResult::HIT,"full key validates collision");
 // Interrupted promotion keeps a valid prior generation.
 MemoryStorage s2;NomadNetCache c2(s2,cfg);drain(c2,&s2);ck(c2.beginCommit(key(),body,300,60)==CacheResult::PENDING,"seed");drain(c2);s2.fail_rename_at=2;ck(c2.beginCommit(key(),std::vector<uint8_t>{'x'},301,60)==CacheResult::PENDING,"crash commit");drain(c2);ck(c2.lastResult()==CacheResult::STORAGE_ERROR,"rename crash reported");s2.fail_rename_at=0;ck(c2.beginLookup(key(),302)==CacheResult::PENDING,"fallback after crash");drain(c2);ck(c2.takeBody(got)&&std::equal(got.begin(),got.end(),body.begin(),body.end()),"prior generation survives");
 // SD faults are cache misses to caller, never page failures.
 s2.available=false;ck(c2.beginLookup(key(),302)==CacheResult::PENDING,"unavailable begins");drain(c2);ck(c2.lastResult()==CacheResult::BYPASS,"unavailable bypass");s2.available=true;s2.busy=true;ck(c2.beginLookup(key(),302)==CacheResult::PENDING,"busy begins");drain(c2);ck(c2.lastResult()==CacheResult::BYPASS,"busy bypass");
 // Deterministic expired-first then oldest quota eviction.
 MemoryStorage s3;NomadNetCache c3(s3,cfg);drain(c3,&s3);for(int i=0;i<3;i++){auto k=key((std::string("/page/")+char('a'+i)).c_str());ck(c3.beginCommit(k,body,400+i,i==0?1:100)==CacheResult::PENDING,"quota commit");drain(c3);}ck(c3.entryCount()<=2&&c3.totalBytes()<=cfg.max_bytes,"quotas bounded");ck(c3.beginLookup(key("/page/a"),500)==CacheResult::PENDING,"evicted lookup");drain(c3);ck(c3.lastResult()!=CacheResult::HIT,"expired evicted first");

 // A fresh cache reconstructs both generations from only the exact namespace.
 s2.busy=false;
 s2.files["/other/cache/foreign.0.meta"]={1,2,3};
 s2.files["/pyxis-nomadnet/cache/unknown.tmp"]={4,5,6};
 const auto unknown=s2.files["/pyxis-nomadnet/cache/unknown.tmp"];
 NomadNetCache rebooted(s2,cfg);
 ck(rebooted.beginLookup(key(),159)==CacheResult::BYPASS,"navigation during recovery bypasses live");
 drain(rebooted,&s2);

 ck(rebooted.recoveryComplete(),"recovery completes");
 ck(rebooted.entryCount()==1,"recovery reconstructs logical key");
 ck(s2.files["/pyxis-nomadnet/cache/unknown.tmp"]==unknown&&s2.files.count("/other/cache/foreign.0.meta"),"unknown and foreign files untouched");
 ck(rebooted.beginLookup(key(),302)==CacheResult::PENDING,"reboot lookup starts");drain(rebooted,&s2);ck(rebooted.lastResult()==CacheResult::HIT,"reboot record hits");

 // Interrupted stage files are ignored, and only explicit valid-clock recovery cleans them.
 s.files["/pyxis-nomadnet/cache/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.stage.body"]={9};
 NomadNetCache staged(s,cfg);drain(staged,&s);
 ck(s.files.count("/pyxis-nomadnet/cache/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.stage.body"),"boot scan preserves stage");
 ck(staged.beginRecovery(0,true)==CacheResult::BYPASS,"invalid clock refuses cleanup");
 ck(staged.beginRecovery(1000,true)==CacheResult::PENDING,"explicit recovery cleanup starts");drain(staged,&s);
 ck(!s.files.count("/pyxis-nomadnet/cache/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.stage.body"),"explicit recovery cleans stage incrementally");

 // Physical quota includes metadata, retained fallback, and candidate staging headroom.
 MemoryStorage tight;CacheConfig tight_cfg=cfg;tight_cfg.max_bytes=400;tight_cfg.max_stage_reserve=160;NomadNetCache tc(tight,tight_cfg);drain(tc,&tight);
 ck(tc.beginCommit(key(),std::vector<uint8_t>(40,'a'),100,60)==CacheResult::PENDING,"tight seed starts");drain(tc,&tight);const auto before=tight.files;
 ck(tc.totalBytes()>40,"physical accounting includes metadata");
 ck(tc.beginCommit(key(),std::vector<uint8_t>(80,'b'),101,60)==CacheResult::FULL,"headroom rejects oversized candidate");
 ck(tight.files==before,"headroom failure preserves old files");
 ck(tc.beginLookup(key(),102)==CacheResult::PENDING,"old lookup after full");drain(tc,&tight);ck(tc.lastResult()==CacheResult::HIT,"old record survives full");

 // Both retained generations and both metadata files count as physical bytes.
 MemoryStorage retained;NomadNetCache rc(retained,cfg);drain(rc,&retained);
 ck(rc.beginCommit(key(),std::vector<uint8_t>(20,'a'),1000,100)==CacheResult::PENDING,"retained first");drain(rc,&retained);const auto one_generation=rc.totalBytes();
 ck(rc.beginCommit(key(),std::vector<uint8_t>(30,'b'),1001,100)==CacheResult::PENDING,"retained second");drain(rc,&retained);
 ck(rc.entryCount()==1&&rc.totalBytes()>one_generation+30,"both generations physically accounted");

 // No logical victim is touched until a candidate is fully promoted.
 MemoryStorage victims;NomadNetCache vc(victims,cfg);drain(vc,&victims);
 ck(vc.beginCommit(key("/page/v1"),body,2000,100)==CacheResult::PENDING,"victim one seed");drain(vc,&victims);
 ck(vc.beginCommit(key("/page/v2"),body,2001,100)==CacheResult::PENDING,"victim two seed");drain(vc,&victims);
 const auto victim_files=victims.files;victims.renames=0;victims.fail_rename_at=1;
 ck(vc.beginCommit(key("/page/candidate"),body,2002,100)==CacheResult::PENDING,"failing candidate starts");drain(vc,&victims);
 ck(vc.lastResult()==CacheResult::STORAGE_ERROR,"promotion failure reported");
 for(const auto&file:victim_files)ck(victims.files.count(file.first)&&victims.files[file.first]==file.second,"promotion failure preserves victim");

 // Cancellation closes the active writer, then removes at most one stage file per tick.
 MemoryStorage cancelled;NomadNetCache cc(cancelled,cfg);drain(cc,&cancelled);
 ck(cc.beginCommit(key(),std::vector<uint8_t>(200,'z'),3000,100)==CacheResult::PENDING,"cancel commit starts");
 for(int i=0;i<5;++i)cc.service();
 ck(cancelled.writing,"writer open before cancel");cc.cancel();ck(cc.busy(),"cancel cleanup deferred");
 const auto cancel_ops=cancelled.operations;cc.service();ck(cancelled.operations==cancel_ops+1&&!cancelled.writing,"cancel closes one handle in one tick");drain(cc,&cancelled);
 ck(cc.lastResult()==CacheResult::CANCELLED,"cancel completes without leaked handle");
 // Cancellation retains handle ownership while close/abort is transiently busy.
 MemoryStorage close_retry;NomadNetCache close_cache(close_retry,cfg);drain(close_cache,&close_retry);
 ck(close_cache.beginCommit(key(),body,4000,100)==CacheResult::PENDING,"close retry seed");drain(close_cache,&close_retry);
 ck(close_cache.beginLookup(key(),4001)==CacheResult::PENDING,"close retry lookup");close_cache.service();
 close_retry.end_read_failures=1;close_cache.cancel();close_cache.service();
 ck(close_cache.busy()&&!close_retry.active.empty(),"busy endRead retains read ownership");drain(close_cache,&close_retry);
 ck(close_cache.lastResult()==CacheResult::CANCELLED&&close_retry.active.empty(),"endRead retry closes before cancellation completes");
 MemoryStorage abort_retry;NomadNetCache abort_cache(abort_retry,cfg);drain(abort_cache,&abort_retry);
 ck(abort_cache.beginCommit(key(),std::vector<uint8_t>(200,'q'),4100,100)==CacheResult::PENDING,"abort retry starts");
 for(int i=0;i<5;++i){abort_cache.service();}abort_retry.abort_failures=1;abort_cache.cancel();abort_cache.service();
 ck(abort_cache.busy()&&abort_retry.writing,"busy abort retains write ownership");drain(abort_cache,&abort_retry);
 ck(abort_cache.lastResult()==CacheResult::CANCELLED&&!abort_retry.writing,"abort retry closes before cancellation completes");

 // Commit admission itself performs no storage availability transaction; service owns cadence.
 MemoryStorage cadence;NomadNetCache cadence_cache(cadence,cfg);drain(cadence_cache,&cadence);const auto admission_ops=cadence.operations;
 ck(cadence_cache.beginCommit(key(),body,4200,100)==CacheResult::PENDING,"cadence commit admitted");
 ck(cadence.operations==admission_ops,"beginCommit has no storage preflight operation");drain(cadence_cache,&cadence);

 // Any short write aborts the active transaction before fsync or promotion.
 MemoryStorage short_write;NomadNetCache short_cache(short_write,cfg);drain(short_cache,&short_write);short_write.max_write=2;
 ck(short_cache.beginCommit(key(),body,4250,100)==CacheResult::PENDING,"short write candidate admitted");drain(short_cache,&short_write);
 ck(short_cache.lastResult()==CacheResult::STORAGE_ERROR,"short write poisons cache transaction");
 ck(short_write.abort_calls==1&&short_write.commit_calls==0,"short write aborts without fsync");
 ck(short_write.files.empty(),"short write never promotes partial bytes");

 // Invalidation retries transient unlink failures and cannot acknowledge while bytes remain.
 MemoryStorage invalidate_retry;NomadNetCache invalidate_cache(invalidate_retry,cfg);drain(invalidate_cache,&invalidate_retry);
 ck(invalidate_cache.beginCommit(key(),body,4300,100)==CacheResult::PENDING,"invalidate retry seed");drain(invalidate_cache,&invalidate_retry);
 const auto invalidate_files=invalidate_retry.files;invalidate_retry.remove_result=StorageResult::BUSY;invalidate_retry.remove_failures=1;
 ck(invalidate_cache.invalidate(key())==CacheResult::PENDING,"invalidate starts");invalidate_cache.service();
 ck(invalidate_cache.busy()&&invalidate_retry.files==invalidate_files,"busy invalidation does not advance");drain(invalidate_cache,&invalidate_retry);
 ck(invalidate_cache.lastResult()==CacheResult::MISS&&invalidate_retry.files.empty(),"invalidation succeeds only after all unlinks close");

 // A permanent eviction unlink failure reports durable degradation and preserves the RAM victim.
 MemoryStorage eviction_failure;CacheConfig one_cfg=cfg;one_cfg.max_entries=1;NomadNetCache eviction_cache(eviction_failure,one_cfg);drain(eviction_cache,&eviction_failure);
 ck(eviction_cache.beginCommit(key("/page/victim"),body,4400,100)==CacheResult::PENDING,"eviction failure victim seed");drain(eviction_cache,&eviction_failure);
 eviction_failure.remove_result=StorageResult::IO_ERROR;eviction_failure.remove_failures=100;eviction_failure.remove_fail_substring=eviction_cache.debugMetadataPath(key("/page/victim"),0).substr(0,55);
 ck(eviction_cache.beginCommit(key("/page/new"),body,4401,100)==CacheResult::PENDING,"eviction failure candidate starts");drain(eviction_cache,&eviction_failure);
 ck(eviction_cache.lastResult()==CacheResult::STORAGE_ERROR&&eviction_cache.entryCount()==2,"permanent eviction failure never reports stored or erases RAM victim");

 // Recovery under tighter quotas reconciles expired/oldest entries before becoming complete.
 MemoryStorage recovery_source;CacheConfig three_cfg=cfg;three_cfg.max_entries=3;NomadNetCache source_cache(recovery_source,three_cfg);drain(source_cache,&recovery_source);
 for(int i=0;i<3;++i){auto k=key((std::string("/page/r")+char('a'+i)).c_str());ck(source_cache.beginCommit(k,body,4500+i,i==0?1:100)==CacheResult::PENDING,"recovery quota seed");drain(source_cache,&recovery_source);}
 CacheConfig recovered_cfg=three_cfg;recovered_cfg.max_entries=2;NomadNetCache recovered_quota(recovery_source,recovered_cfg);drain(recovered_quota,&recovery_source);
 ck(recovered_quota.recoveryComplete()&&recovered_quota.entryCount()<=2&&recovered_quota.totalBytes()<=recovered_cfg.max_bytes,"recovery cannot finish over entry or byte quota");

 // An ambiguous/incomplete namespace scan is not authoritative for slot overwrite.
 MemoryStorage uncertain_storage;NomadNetCache certain_cache(uncertain_storage,cfg);drain(certain_cache,&uncertain_storage);
 ck(certain_cache.beginCommit(key(),body,4600,100)==CacheResult::PENDING,"uncertain seed");drain(certain_cache,&uncertain_storage);const auto certain_files=uncertain_storage.files;
 uncertain_storage.next_list_fail_at=1;NomadNetCache uncertain_cache(uncertain_storage,cfg);drain(uncertain_cache,&uncertain_storage);
 ck(uncertain_cache.beginCommit(key(),std::vector<uint8_t>{'x'},4601,100)==CacheResult::BYPASS,"incomplete recovery refuses unknown generation selection");
 ck(uncertain_storage.files==certain_files,"incomplete recovery preserves last valid generation");

 // A permanent per-record metadata read error makes every enumerated slot unknown.
 MemoryStorage unread;NomadNetCache unread_source(unread,cfg);drain(unread_source,&unread);
 ck(unread_source.beginCommit(key(),std::vector<uint8_t>{'a'},4650,100)==CacheResult::PENDING,"unread first slot seed");drain(unread_source,&unread);
 ck(unread_source.beginCommit(key(),std::vector<uint8_t>{'b'},4651,100)==CacheResult::PENDING,"unread second slot seed");drain(unread_source,&unread);
 const auto unread_snapshot=unread.files;unread.read_fail_path=unread_source.debugMetadataPath(key(),0);unread.read_fail_result=StorageResult::IO_ERROR;
 NomadNetCache unread_reboot(unread,cfg);drain(unread_reboot,&unread);
 ck(!unread_reboot.recoveryComplete(),"permanent metadata read error prevents authoritative recovery");
 ck(unread_reboot.beginCommit(key(),std::vector<uint8_t>{'x'},4652,100)==CacheResult::BYPASS,"unread generation is never selected for replacement");
 ck(unread.files==unread_snapshot,"metadata read error preserves exact unknown slot bytes");

 // The same ownership rule applies when metadata is readable but body stat fails.
 MemoryStorage unstat=unread;unstat.read_fail_path.clear();unstat.read_fail_result=StorageResult::OK;
 unstat.stat_fail_path=unread_source.debugBodyPath(key(),1);unstat.stat_fail_result=StorageResult::IO_ERROR;const auto unstat_snapshot=unstat.files;
 NomadNetCache unstat_reboot(unstat,cfg);drain(unstat_reboot,&unstat);
 ck(!unstat_reboot.recoveryComplete(),"permanent body stat error prevents authoritative recovery");
 ck(unstat_reboot.beginCommit(key(),std::vector<uint8_t>{'y'},4653,100)==CacheResult::BYPASS,"unstatted generation is never selected for replacement");
 ck(unstat.files==unstat_snapshot,"body stat error preserves exact unknown slot bytes");

 // Equal-sequence slots are accepted only when their canonical metadata bytes match exactly.
 MemoryStorage conflict_storage;NomadNetCache conflict_cache(conflict_storage,cfg);drain(conflict_cache,&conflict_storage);
 ck(conflict_cache.beginCommit(key(),body,4700,100)==CacheResult::PENDING,"conflict seed");drain(conflict_cache,&conflict_storage);
 const auto g=conflict_cache.debugGeneration();const auto other_g=g^1U;
 conflict_storage.files[conflict_cache.debugBodyPath(key(),other_g)]=conflict_storage.files[conflict_cache.debugBodyPath(key(),g)];
 auto conflicting_meta=conflict_storage.files[conflict_cache.debugMetadataPath(key(),g)];
 conflicting_meta[16]^=1U;const auto exact_hash=NomadNetCache::hash(conflicting_meta.data(),conflicting_meta.size()-8);
 for(unsigned i=0;i<8;++i)conflicting_meta[conflicting_meta.size()-8+i]=static_cast<uint8_t>(exact_hash>>(8U*i));
 conflict_storage.files[conflict_cache.debugMetadataPath(key(),other_g)]=conflicting_meta;
 ck(conflict_cache.beginLookup(key(),4701)==CacheResult::PENDING,"equal sequence conflict lookup");drain(conflict_cache,&conflict_storage);
 ck(conflict_cache.lastResult()==CacheResult::MISS,"equal sequence conflicting metadata rejected conservatively");

 // Current-key-only recovery sheds the older slot rather than finishing over physical quota.
 MemoryStorage current_only;NomadNetCache current_source(current_only,cfg);drain(current_source,&current_only);
 ck(current_source.beginCommit(key(),std::vector<uint8_t>(20,'a'),4800,100)==CacheResult::PENDING,"current only first");drain(current_source,&current_only);
 ck(current_source.beginCommit(key(),std::vector<uint8_t>(30,'b'),4801,100)==CacheResult::PENDING,"current only second");drain(current_source,&current_only);
 const auto two_slot_bytes=current_source.totalBytes();CacheConfig current_tight=cfg;current_tight.max_bytes=two_slot_bytes-1;
 NomadNetCache current_reboot(current_only,current_tight);drain(current_reboot,&current_only);
 ck(current_reboot.recoveryComplete()&&current_reboot.totalBytes()<=current_tight.max_bytes,"current-key fallback physical bytes reconciled");
 ck(current_reboot.beginLookup(key(),4802)==CacheResult::PENDING,"current-key post-reconcile lookup");drain(current_reboot,&current_only);ck(current_reboot.lastResult()==CacheResult::HIT,"current-key reconciliation preserves newest data");

 std::cout<<(f?"failed":"passed")<<"\n";return f?1:0;}
