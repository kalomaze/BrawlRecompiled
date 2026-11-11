#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// Portable guest write tracking. Aliases share a Page allocated by the match
// tracker. Publication/removal happens at the simulation's snapshot safe point.
namespace BrawlDirtyMemory {
struct Page {
    std::atomic<uint8_t> touched{0};
    std::atomic<uint8_t> blocks[1024]{};
};
inline std::atomic<bool> active{false};
inline Page* pages[65536]{};
inline void Mark(uint32_t address,size_t size) {
    if(!active.load(std::memory_order_relaxed) || !size)return;
    while(size) {
        const size_t offset=address&65535u;
        const size_t count=size<65536-offset?size:65536-offset;
        if(auto* p=pages[address>>16]) {
            p->touched.store(1,std::memory_order_relaxed);
            for(size_t i=offset/64;i<=(offset+count-1)/64;++i)
                p->blocks[i].store(1,std::memory_order_relaxed);
        }
        address+=uint32_t(count);size-=count;
    }
}
// Bulk helpers holding a host backing pointer use this at the actual write,
// never at pointer acquisition (which would also mark reads and miss caching).
struct HostSpan {uintptr_t start;size_t size;uint32_t guest;};
inline uintptr_t flatBase=0;
inline std::vector<HostSpan> hosts;
inline void MarkHost(const void* pointer,size_t size) {
    if(!active.load(std::memory_order_relaxed) || !size)return;
    const uintptr_t start=reinterpret_cast<uintptr_t>(pointer);
    if(start>=flatBase && start-flatBase<=UINT32_MAX && flatBase) {
        Mark(uint32_t(start-flatBase),size);return;
    }
    for(const auto& h:hosts) {
        const uintptr_t lo=start>h.start?start:h.start;
        const uintptr_t hi=start+size<h.start+h.size?start+size:h.start+h.size;
        if(lo<hi)Mark(h.guest+uint32_t(lo-h.start),hi-lo);
    }
}
}
