#include "brawl_write_watch.h"
#include <cassert>
#include <thread>
#include <iostream>
using BrawlRollback::WriteWatch;
int main() {
    BrawlRollback::SnapshotJobs jobs;
    uint8_t a[77]{},b[91]{};
    {
        WriteWatch watch;
        watch.Add(a,sizeof(a),0,0x80000003);
        watch.Add(b,sizeof(b),sizeof(a),0x90010007);
        watch.Reset();
        auto clean=watch.Capture(sizeof(a)+sizeof(b),jobs);
        assert(clean.empty());
        BrawlDirtyMemory::Mark(0xc0000040,8);
        auto dirty=watch.Capture(sizeof(a)+sizeof(b),jobs);
        assert((dirty==std::vector<uint32_t>{0,1}));
        assert(watch.Capture(sizeof(a)+sizeof(b),jobs).empty());
        watch.Reset();
        BrawlDirtyMemory::MarkHost(b+70,2);
        dirty=watch.Capture(sizeof(a)+sizeof(b),jobs);
        assert((dirty==std::vector<uint32_t>{2}));
    }
    assert(!BrawlDirtyMemory::active.load());
    std::vector<uint8_t> memory(65536);
    {
        WriteWatch watch;watch.Add(memory.data(),memory.size(),0,0x90000000);watch.Reset();
        std::vector<std::thread> threads;
        for(unsigned t=0;t<4;++t)threads.emplace_back([t]{
            for(unsigned block=t;block<1024;block+=4)
                BrawlDirtyMemory::Mark(0xd0000000+block*64,1);
        });
        for(auto& t:threads)t.join();
        auto dirty=watch.Capture(memory.size(),jobs);
        assert(dirty.size()==1024);
        for(size_t i=0;i<dirty.size();++i)assert(dirty[i]==i);
        watch.Reset();dirty=watch.Capture(memory.size(),jobs);
        assert(dirty.empty());
    }
    {
        std::vector<uint8_t> backing(131072);
        WriteWatch watch;
        BrawlDirtyMemory::hosts.push_back({reinterpret_cast<uintptr_t>(backing.data()),backing.size(),0x80000000});
        watch.Add(backing.data()+3,77,0,0x80000003);
        watch.Add(backing.data()+65543,91,77,0x80010007);
        assert(BrawlDirtyMemory::hosts.size()==1);
        watch.Reset();
        BrawlDirtyMemory::MarkHost(backing.data()+65543+70,2);
        assert((watch.Capture(168,jobs)==std::vector<uint32_t>{2}));
        BrawlDirtyMemory::MarkHost(backing.data()+4000,2);
        assert(watch.Capture(168,jobs).empty());
    }
    for(auto* p:BrawlDirtyMemory::pages)assert(p==nullptr);
    std::cout<<"Portable tracker: unaligned packed ranges, aliases, host writes, concurrent marking and lifecycle passed\n";
}
