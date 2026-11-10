#include "brawl_write_watch.h"
#include <cassert>
#include <thread>
#include <iostream>
using BrawlRollback::WriteWatch;
int main() {
    uint8_t a[77]{},b[91]{};
    {
        WriteWatch watch;
        watch.Add(a,sizeof(a),0,0x80000003);
        watch.Add(b,sizeof(b),sizeof(a),0x90010007);
        watch.Reset();
        auto clean=watch.Capture(sizeof(a)+sizeof(b));
        for(auto v:clean)assert(v==0);
        BrawlDirtyMemory::Mark(0xc0000040,8);
        auto dirty=watch.Capture(sizeof(a)+sizeof(b));
        assert(dirty.size()==3 && dirty[0] && dirty[1] && !dirty[2]);
        watch.Reset();
        BrawlDirtyMemory::MarkHost(b+70,2);
        dirty=watch.Capture(sizeof(a)+sizeof(b));
        assert(!dirty[0] && !dirty[1] && dirty[2]);
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
        auto dirty=watch.Capture(memory.size());
        for(auto v:dirty)assert(v==1);
        watch.Reset();dirty=watch.Capture(memory.size());
        for(auto v:dirty)assert(v==0);
    }
    for(auto* p:BrawlDirtyMemory::pages)assert(p==nullptr);
    std::cout<<"Portable tracker: unaligned packed ranges, aliases, host writes, concurrent marking and lifecycle passed\n";
}
