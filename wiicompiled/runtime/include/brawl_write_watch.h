#pragma once
#include "brawl_dirty_memory.h"
#include <algorithm>
#include <chrono>
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
        spans.push_back({guest,size,packed});
        BrawlDirtyMemory::hosts.push_back({reinterpret_cast<uintptr_t>(host),size,guest});
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
    }
    void Reset(){
        const auto start=std::chrono::steady_clock::now();
        for(auto& [index,p]:owned)if(p->touched.exchange(0,std::memory_order_relaxed))
            for(auto& block:p->blocks)block.store(0,std::memory_order_relaxed);
        BrawlDirtyMemory::active.store(true,std::memory_order_relaxed);
        resetNs=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
    }
    std::vector<uint8_t> Capture(size_t bytes){
        const auto start=std::chrono::steady_clock::now();
        std::vector<uint8_t> dirty((bytes+63)/64,0);metadataBytes=0;
        for(const auto& s:spans) {
            const uint64_t end=uint64_t(s.guest)+s.size;
            for(uint64_t pageStart=uint64_t(s.guest)&~uint64_t(65535);pageStart<end;pageStart+=65536) {
                auto* page=BrawlDirtyMemory::pages[pageStart>>16];++metadataBytes;
                if(!page || !page->touched.load(std::memory_order_relaxed))continue;
                const uint64_t first=std::max(pageStart,uint64_t(s.guest)),last=std::min(pageStart+65536,end);
                for(uint64_t block=first&~uint64_t(63);block<last;block+=64) {
                    ++metadataBytes;
                    if(!page->blocks[(block-pageStart)/64].load(std::memory_order_relaxed))continue;
                    const size_t lo=(s.packed+std::max(block,first)-s.guest)/64;
                    const size_t hi=(s.packed+std::min(block+64,last)-s.guest+63)/64;
                    if(hi>dirty.size())throw std::runtime_error("dirty span outside packed memory");
                    std::fill(dirty.begin()+lo,dirty.begin()+hi,1);
                }
            }
        }
        captureNs=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
        return dirty;
    }
};
}
