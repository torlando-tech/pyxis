#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include "NomadNetStorage.h"

using namespace UI::LXMF::NomadNet;

struct ScriptedStorage final : NomadNetStorage {
    bool available=true, busy=false, full=false, open=false, aborted=false, committed=false, poisoned=false;
    std::size_t max_write=SIZE_MAX; bool zero_progress=false;
    std::vector<uint8_t> bytes;
    bool isAvailable() const override { return available; }
    StorageResult beginRead(const char*, uint32_t& size) override { if(!available)return StorageResult::UNAVAILABLE; size=bytes.size();open=true;return StorageResult::OK; }
    StorageResult readChunk(uint8_t* out,std::size_t cap,std::size_t& count) override { if(busy)return StorageResult::BUSY; count=std::min(cap,bytes.size());std::memcpy(out,bytes.data(),count);return StorageResult::OK; }
    StorageResult endRead() override {open=false;return StorageResult::OK;}
    StorageResult beginWrite(const char*) override { if(!available)return StorageResult::UNAVAILABLE;if(busy)return StorageResult::BUSY;if(full)return StorageResult::FULL;open=true;poisoned=false;bytes.clear();return StorageResult::OK; }
    StorageResult writeChunk(const uint8_t* data,std::size_t size,std::size_t& written) override { if(busy){written=0;poisoned=true;return StorageResult::BUSY;}if(full){written=0;poisoned=true;return StorageResult::FULL;}if(zero_progress){written=0;poisoned=true;return StorageResult::NO_PROGRESS;}written=std::min(size,max_write);bytes.insert(bytes.end(),data,data+written);if(written!=size)poisoned=true;return written==size?StorageResult::OK:StorageResult::PARTIAL_WRITE; }
    StorageResult commitWrite() override { if(!open||poisoned)return StorageResult::INVALID_STATE;open=false;committed=true;return StorageResult::OK; }
    StorageResult abortWrite() override {open=false;poisoned=false;aborted=true;return StorageResult::OK;}
    StorageResult remove(const char*) override{return StorageResult::OK;}
    StorageResult rename(const char*,const char*) override{return StorageResult::OK;}
    StorageResult stat(const char*,uint32_t&) override{return StorageResult::MISS;}
    StorageResult beginList(const char*) override{return StorageResult::OK;}
    StorageResult nextList(char*,std::size_t,bool& done) override{done=true;return StorageResult::OK;}
    StorageResult endList() override{return StorageResult::OK;}
};

int main(){int failed=0;auto check=[&](bool c,const char*n){if(!c){++failed;std::cerr<<"FAIL: "<<n<<"\n";}};
 check(storage_result_is_transient(StorageResult::BUSY),"busy typed transient");
 check(storage_result_is_unavailable(StorageResult::UNAVAILABLE),"unavailable typed");
 check(storage_result_is_write_failure(StorageResult::FULL),"full typed write failure");
 ScriptedStorage s; const uint8_t data[]={1,2,3,4};std::size_t n=0;
 check(s.beginWrite("/cache/x")==StorageResult::OK,"begin write");
 s.max_write=2;check(s.writeChunk(data,4,n)==StorageResult::PARTIAL_WRITE&&n==2,"partial write observable");
 check(s.commitWrite()==StorageResult::INVALID_STATE&&!s.committed,"partial write poisons descriptor");
 check(s.abortWrite()==StorageResult::OK&&s.aborted&&!s.committed,"partial write aborts");
 s.zero_progress=true;check(s.beginWrite("/cache/y")==StorageResult::OK&&s.writeChunk(data,4,n)==StorageResult::NO_PROGRESS&&n==0,"zero progress typed");
 check(s.abortWrite()==StorageResult::OK,"zero progress abort");
 s.available=false;uint32_t size=0;check(s.beginRead("x",size)==StorageResult::UNAVAILABLE,"missing card falls through");
 s.available=true;s.busy=true;check(s.beginWrite("x")==StorageResult::BUSY,"busy card typed");
 s.busy=false;s.full=true;check(s.beginWrite("x")==StorageResult::FULL,"full card typed");
 std::cout<<(failed?"failed":"passed")<<"\n";return failed?1:0;}
