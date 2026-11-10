#pragma once
#include "brawl_dirty_memory.h"
#include "brawl_snapshot_jobs.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

namespace BrawlRollback {
// Explicit portable instrumentation, not OS page protection. The simulation
// boundary must quiesce writers before Capture/Reset or destroying the tracker.
class WriteWatch {
    struct Span {uint32_t guest;size_t size,packed;};
    std::vector<Span> spans;
    std::map<uint32_t,std::unique_ptr<BrawlDirtyMemory::Page>> owned;
    std::vector<std::pair<uint32_t,BrawlDirtyMemory::Page*>> scanPages;
    std::array<std::vector<uint32_t>,SnapshotJobs::Chunks> scanResults;
public:
    size_t metadataBytes=0;
    uint64_t resetNs=0,captureNs=0;
    explicit WriteWatch(uintptr_t flatBase=0) {
        if(BrawlDirtyMemory::active.load())throw std::runtime_error("duplicate guest dirty tracker");
        BrawlDirtyMemory::flatBase=flatBase;
    }
    ~WriteWatch(){
        BrawlDirtyMemory::active.store(false,std::memory_order_relaxed);
        for(auto& p:BrawlDirtyMemory::pages)p=nullptr;
        BrawlDirtyMemory::hosts.clear();
    }
    WriteWatch(const WriteWatch&)=delete;
    WriteWatch& operator=(const WriteWatch&)=delete;
    void Add(const void* host,size_t size,size_t packed,uint32_t guest=0) {
        if(!size)return;
        const uint32_t physical=guest&0x3fffffffu;
        for(const auto& s:spans)if(uint64_t(physical)<uint64_t(s.guest)+s.size && uint64_t(s.guest)<uint64_t(physical)+size)
            throw std::runtime_error("overlapping selected snapshot regions");
        spans.push_back({physical,size,packed});
        std::sort(spans.begin(),spans.end(),[](const Span& a,const Span& b){return a.guest<b.guest;});
        for(size_t i=1;i<spans.size();++i)
            if(spans[i-1].packed+spans[i-1].size>spans[i].packed)
                throw std::runtime_error("snapshot packing must preserve physical address order");
        const auto hostStart=reinterpret_cast<uintptr_t>(host);
        bool registered=false;
        for(const auto& span:BrawlDirtyMemory::hosts) {
            if(hostStart<span.start || hostStart-span.start>span.size || size>span.size-(hostStart-span.start))continue;
            if(((span.guest+uint32_t(hostStart-span.start))&0x3fffffffu)!=physical)
                throw std::runtime_error("inconsistent guest mapping for host memory");
            registered=true;break;
        }
        if(!registered)BrawlDirtyMemory::hosts.push_back({hostStart,size,guest});
        for(uint64_t addr=uint64_t(guest)&~uint64_t(65535);addr<uint64_t(guest)+size;addr+=65536) {
            // MEM1/MEM2 physical/cached/uncached aliases share the same mark.
            const uint32_t canonical=uint32_t(addr)&0x3fffffffu;
            auto& page=owned[canonical>>16];
            if(!page)page=std::make_unique<BrawlDirtyMemory::Page>();
            BrawlDirtyMemory::pages[uint32_t(addr)>>16]=page.get();
            if(canonical<0x20000000u) {
                BrawlDirtyMemory::pages[canonical>>16]=page.get();
                BrawlDirtyMemory::pages[(canonical|0x80000000u)>>16]=page.get();
                BrawlDirtyMemory::pages[(canonical|0xc0000000u)>>16]=page.get();
            }
        }
        scanPages.clear();
        for(const auto& [index,page]:owned)scanPages.emplace_back(index,page.get());
    }
    void Reset(){
        const auto start=std::chrono::steady_clock::now();
        for(auto& [index,p]:owned)if(p->touched.exchange(0,std::memory_order_relaxed))
            for(auto& block:p->blocks)block.store(0,std::memory_order_relaxed);
        BrawlDirtyMemory::active.store(true,std::memory_order_relaxed);
        resetNs=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
    }
    std::vector<uint32_t> Capture(size_t bytes,SnapshotJobs& jobs){
        const auto start=std::chrono::steady_clock::now();
        std::vector<uint32_t> dirty;dirty.reserve(8192);metadataBytes=0;
        // Same cache-line scan/skip and clear-as-consumed ordering as Dolphin's
        // JITDirtyBitmap::ScanAndClear. Writers are quiescent at this boundary,
        // so inspecting atomic byte representations cannot race a live store.
        static_assert(sizeof(std::atomic<uint8_t>)==1,"dirty marks must be byte sized");
        std::array<size_t,SnapshotJobs::Chunks> metadata{};
        jobs.Run([&](size_t chunk) {
          auto& dirty=scanResults[chunk];dirty.clear();
          const size_t begin=scanPages.size()*chunk/SnapshotJobs::Chunks;
          const size_t end=scanPages.size()*(chunk+1)/SnapshotJobs::Chunks;
          auto spanCursor=spans.begin();
          if(begin<end)spanCursor=std::lower_bound(spans.begin(),spans.end(),uint64_t(scanPages[begin].first)<<16,
              [](const Span& span,uint64_t address){return uint64_t(span.guest)+span.size<=address;});
          for(size_t pageNumber=begin;pageNumber<end;++pageNumber) {
            const auto [pageIndex,page]=scanPages[pageNumber];
            ++metadata[chunk];
            if(!page->touched.load(std::memory_order_relaxed))continue;
            for(size_t first=0;first<1024;first+=64) {
                uint8_t marks[64];uint64_t words[8];
                std::memcpy(marks,page->blocks+first,sizeof(marks));metadata[chunk]+=sizeof(marks);
                std::memcpy(words,marks,sizeof(words));
                if(!(words[0]|words[1]|words[2]|words[3]|words[4]|words[5]|words[6]|words[7]))continue;
                for(size_t wordIndex=0;wordIndex<8;++wordIndex) {
                  auto word=words[wordIndex];
                  while(word) {
                    const size_t byte=std::countr_zero(word)/8;
                    word&=~(uint64_t{0xff}<<(byte*8));
                    const size_t i=wordIndex*8+byte;
                    page->blocks[first+i].store(0,std::memory_order_relaxed);
                    const uint64_t block=(uint64_t(pageIndex)<<16)+(first+i)*64;
                    while(spanCursor!=spans.end() && uint64_t(spanCursor->guest)+spanCursor->size<=block)++spanCursor;
                    auto at=spanCursor;
                    for(;at!=spans.end() && at->guest<block+64;++at) {
                        const auto& span=*at;
                        const uint64_t loGuest=std::max(block,uint64_t(span.guest));
                        const uint64_t hiGuest=std::min(block+64,uint64_t(span.guest)+span.size);
                        const size_t lo=(span.packed+loGuest-span.guest)/64;
                        const size_t hi=(span.packed+hiGuest-span.guest+63)/64;
                        if(hi>(bytes+63)/64)throw std::runtime_error("dirty span outside packed memory");
                        for(size_t key=lo;key<hi;++key)
                            if(dirty.empty() || dirty.back()!=key)dirty.push_back(uint32_t(key));
                    }
                  }
                }
            }
            page->touched.store(0,std::memory_order_relaxed);
        }
        });
        for(size_t chunk=0;chunk<scanResults.size();++chunk) {
            const auto& part=scanResults[chunk];
            auto begin=part.begin();
            if(begin!=part.end() && !dirty.empty() && dirty.back()==*begin)++begin;
            dirty.insert(dirty.end(),begin,part.end());
            metadataBytes+=metadata[chunk];
        }
        captureNs=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
        return dirty;
    }
};
}
