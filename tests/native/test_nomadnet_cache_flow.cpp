#include <iostream>
#include <map>
#include <vector>
#include <cstring>
#include "NomadNetCacheFlow.h"
using namespace UI::LXMF::NomadNet;
struct Mem:NomadNetStorage{std::map<std::string,std::vector<uint8_t>>f;std::string a;size_t p=0;bool w=false;bool isAvailable()const override{return true;}StorageResult beginRead(const char*n,uint32_t&s)override{auto i=f.find(n);if(i==f.end())return StorageResult::MISS;a=n;p=0;s=i->second.size();return StorageResult::OK;}StorageResult readChunk(uint8_t*o,size_t c,size_t&n)override{auto&v=f[a];n=std::min(c,v.size()-p);memcpy(o,v.data()+p,n);p+=n;return StorageResult::OK;}StorageResult endRead()override{return StorageResult::OK;}StorageResult beginWrite(const char*n)override{a=n;f[a].clear();w=true;return StorageResult::OK;}StorageResult writeChunk(const uint8_t*d,size_t z,size_t&n)override{n=z;f[a].insert(f[a].end(),d,d+z);return StorageResult::OK;}StorageResult commitWrite()override{w=false;return StorageResult::OK;}StorageResult abortWrite()override{w=false;return StorageResult::OK;}StorageResult remove(const char*n)override{return f.erase(n)?StorageResult::OK:StorageResult::MISS;}StorageResult rename(const char*x,const char*y)override{auto i=f.find(x);if(i==f.end())return StorageResult::MISS;f[y]=i->second;f.erase(i);return StorageResult::OK;}StorageResult stat(const char*,uint32_t&)override{return StorageResult::MISS;}StorageResult beginList(const char*)override{return StorageResult::OK;}StorageResult nextList(char*,size_t,bool&d)override{d=true;return StorageResult::OK;}StorageResult endList()override{return StorageResult::OK;}};
int main(){int f=0;auto ck=[&](bool x,const char*n){if(!x){f++;std::cerr<<"FAIL "<<n<<"\n";}};Mem s;NomadNetCache c(s);NomadNetCacheFlow flow(c);CacheKey k{"0123456789abcdef0123456789abcdef","/page/index.mu",RequestDataClass::NIL};
 ck(flow.begin(k,100,false)==CacheFlowState::LOOKUP,"lookup first");for(int i=0;i<10&&flow.state()==CacheFlowState::LOOKUP;i++)flow.service();ck(flow.state()==CacheFlowState::NEED_LIVE,"miss needs live");std::vector<uint8_t>b={'o','k'};CacheEligibility e{true,true,false,false,false,false,RequestDataClass::NIL};ck(flow.acceptLive(b,e,100),"valid live accepted");ck(flow.pageReady()&&flow.status()=="Page loaded (live)","render ready before commit");for(int i=0;i<20;i++)flow.service();
 NomadNetCacheFlow hit(c);ck(hit.begin(k,101,false)==CacheFlowState::LOOKUP,"second lookup");for(int i=0;i<10&&hit.state()==CacheFlowState::LOOKUP;i++)hit.service();ExternalVector<uint8_t>out;ck(hit.state()==CacheFlowState::READY&&hit.takePage(out)&&std::equal(out.begin(),out.end(),b.begin(),b.end())&&hit.status()=="Cached page; current reachability not checked","hit without peer and without internal-vector copy");
 // A new lookup arriving while unrelated cache work is active must wait for
 // cancellation cleanup, admit its own lookup, and still use the available hit.
 CacheKey other{"fedcba9876543210fedcba9876543210","/page/other.mu",RequestDataClass::NIL};
 ck(c.beginCommit(other,b,101,43200)==CacheResult::PENDING,"overlap commit admitted");
 NomadNetCacheFlow overlap(c);ck(overlap.begin(k,102,false)==CacheFlowState::LOOKUP,"overlap lookup waits");
 for(int i=0;i<200&&overlap.state()==CacheFlowState::LOOKUP;++i)overlap.service();
 ExternalVector<uint8_t>overlap_out;
 ck(overlap.state()==CacheFlowState::READY&&overlap.takePage(overlap_out)&&std::equal(overlap_out.begin(),overlap_out.end(),b.begin(),b.end()),"busy cache retries requested lookup and preserves hit");
 NomadNetCacheFlow fields(c);k.request_data=RequestDataClass::FIELDS;fields.begin(k,101,false);fields.service();ck(fields.state()==CacheFlowState::NEED_LIVE,"request data bypass");
 k.request_data=RequestDataClass::NIL;NomadNetCacheFlow reload(c);reload.begin(k,101,true);while(reload.state()==CacheFlowState::INVALIDATE)reload.service();ck(reload.state()==CacheFlowState::NEED_LIVE,"reload bypass invalidates without history-side effects");
 NomadNetCacheFlow malformed(c);malformed.begin(k,101,false);while(malformed.state()==CacheFlowState::LOOKUP)malformed.service();CacheEligibility bad{true,false,false,true,false,false,RequestDataClass::NIL};ck(!malformed.acceptLive(b,bad,101)&&malformed.state()==CacheFlowState::FAILED,"malformed not committed");
 NomadNetCacheFlow cancelled(c);cancelled.begin(k,101,false);cancelled.cancel();cancelled.service();ck(cancelled.state()==CacheFlowState::CANCELLED&&!cancelled.pageReady(),"navigation cancels lookup");
 // Reload during startup recovery remains in explicit invalidation until the old generation is physically gone.
 NomadNetCache recovering(s);NomadNetCacheFlow recovering_reload(recovering);k.request_data=RequestDataClass::NIL;
 ck(recovering_reload.begin(k,102,true)==CacheFlowState::INVALIDATE,"reload during recovery waits for invalidation admission");
 for(int i=0;i<200&&recovering_reload.state()==CacheFlowState::INVALIDATE;++i)recovering_reload.service();
 ck(recovering_reload.state()==CacheFlowState::NEED_LIVE,"reload starts exactly one live fetch only after terminal invalidation");
 ck(recovering.beginLookup(k,102)==CacheResult::PENDING,"post reload invalidation lookup");for(int i=0;i<50&&recovering.busy();++i)recovering.service();
 ck(recovering.lastResult()!=CacheResult::HIT,"reload during recovery removed stale generation");
 std::cout<<(f?"failed":"passed")<<"\n";return f?1:0;}
