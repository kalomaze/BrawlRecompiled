#include "brawl_snapshot_store.h"
#include "brawl_write_watch.h"

#include <cassert>
#include <iostream>
#include <map>
using namespace BrawlRollback;
int main(){
    constexpr size_t bytes=131072;
    std::vector<uint8_t> memory(bytes);auto* a=memory.data();auto* b=a;
    WriteWatch watch;watch.Add(a,bytes,0,0x80000000);
    SnapshotStore store;std::map<int,std::vector<uint8_t>> reference;int anchor=0;
    auto save=[&](int frame){
        auto dirty=store.Empty()?std::vector<uint32_t>{}:watch.Capture(bytes,store.Jobs());
        store.Save(frame,bytes,[&](size_t o,uint8_t* p,size_t n){std::memcpy(p,a+o,n);},store.Empty()?nullptr:&dirty,anchor);
        reference[frame]=std::vector<uint8_t>(a,a+bytes);
        while(reference.rbegin()->first-reference.begin()->first>=16)reference.erase(reference.begin());
        for(const auto& [f,v]:reference)assert(store.View(f)==v);
        watch.Enable();anchor=frame;
    };
    save(0);
    for(int f=1;f<80;++f){
        BrawlDirtyMemory::Mark(0xc0000000u+(f*913)%bytes,1);
        b[(f*913)%bytes]^=uint8_t(f); // retained writable alias, bypassing all APIs
        BrawlDirtyMemory::MarkHost(a+4090,20);
        std::memset(a+4090,f,20); // cross-page bulk helper write
        save(f);assert(store.lastReadBytes<bytes/2);
        if(f>7 && f%9==0){
            int target=f-5;
            // Include unsaved writes, including bytes absent from history deltas.
            BrawlDirtyMemory::Mark(120001,1);BrawlDirtyMemory::MarkHost(a+65530,19);
            b[120001]^=0x5a;std::memset(a+65530,0xcc,19);
            store.Restore(target,anchor,watch.Capture(bytes,store.Jobs()),
                [&](size_t o,uint8_t* p,size_t n){std::memcpy(p,a+o,n);},
                [&](size_t o,const uint8_t* p,size_t n){std::memcpy(a+o,p,n);});
            assert(std::memcmp(a,reference.at(target).data(),bytes)==0);
            assert(store.lastRestoreWriteBytes<bytes/2);
            watch.Enable();anchor=target;
            reference.erase(reference.upper_bound(target),reference.end());
            for(int replay=target+1;replay<=f;++replay){
                // The new timeline doesn't rewrite every predicted byte. The
                // discarded future must not leak the old prediction.
                BrawlDirtyMemory::MarkHost(b+90000+replay,1);
                b[90000+replay]^=uint8_t(replay);save(replay);
            }
        }
    }
    std::cout<<"Portable dirty bitmap: guest aliases, retained host pointers, cross-page bulk writes and corrected-history restores passed\n";

}
