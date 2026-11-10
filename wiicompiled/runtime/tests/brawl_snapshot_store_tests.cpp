#include "brawl_snapshot_store.h"
#include <cassert>
#include <map>
#include <random>
#include <iostream>
using BrawlRollback::SnapshotStore;
int main() {
    SnapshotStore store;
    std::map<int,std::vector<uint8_t>> reference;
    std::vector<uint8_t> ram(1024*1024+13);
    std::mt19937 random(713);
    auto read=[&](size_t o,uint8_t* out,size_t n){std::memcpy(out,ram.data()+o,n);};
    auto write=[&](size_t o,const uint8_t* in,size_t n){std::memcpy(ram.data()+o,in,n);};
    int anchor=0;
    store.Save(0,ram.size(),read);reference[0]=ram;
    auto save=[&](int frame) {
        std::vector<uint8_t> dirty((ram.size()+63)/64);
        for(size_t i=0;i<200;++i){size_t o=random()%ram.size();ram[o]^=1;dirty[o/64]=1;}
        ram.back()^=7;dirty.back()=1;
        std::vector<uint32_t> keys;for(size_t i=0;i<dirty.size();++i)if(dirty[i])keys.push_back(uint32_t(i));
        store.Save(frame,ram.size(),read,&keys,anchor);anchor=frame;reference[frame]=ram;
        while(reference.size()>16)reference.erase(reference.begin());
        assert(store.Count()==reference.size());
        for(const auto& [f,bytes]:reference)assert(store.Materialize(f)==bytes);
    };
    for(int frame=1;frame<120;++frame) {
        save(frame);
        if(frame>4 && frame%3==0) {
            const int back=frame-3;
            std::vector<uint8_t> dirty((ram.size()+63)/64);
            ram[64]^=9;dirty[1]=1;
            store.Restore(back,anchor,{1},read,write);anchor=back;
            assert(ram==reference[back]);
            reference.erase(reference.upper_bound(back),reference.end());
            assert(!store.Contains(frame)); // Dolphin discards the predicted future.
            for(int f=back+1;f<=frame;++f)save(f);
        }
    }
    bool rejected=false;
    try {store.Materialize(-99);}catch(const std::runtime_error&){rejected=true;}
    assert(rejected);
    // Destruction and move/reset must join a pending base eviction.
    for(int iteration=0;iteration<10;++iteration) {
        SnapshotStore lifecycle;
        for(int f=0;f<40;++f){ram[0]=uint8_t(f);lifecycle.Save(f,ram.size(),read);}
        SnapshotStore moved=std::move(lifecycle);
        assert(moved.Materialize(39)==ram);
        moved=SnapshotStore{};
    }
    BrawlRollback::SnapshotJobs jobs;
    bool workerError=false;
    try {jobs.Run([](size_t i){if(i==2)throw std::runtime_error("worker test");});}
    catch(const std::runtime_error&){workerError=true;}
    assert(workerError);
    bool preparationError=false;
    std::atomic<bool> childRan{false};
    try {jobs.RunPrepared([]{throw std::runtime_error("prepare test");},[&](size_t){childRan.store(true);});}
    catch(const std::runtime_error&){preparationError=true;}
    assert(preparationError && !childRan.load());
    jobs.Run([cursor=size_t{0}](size_t) mutable {assert(cursor==0);++cursor;});
    std::cout<<"Raw snapshots: ring eviction, sparse saves, unsaved writes, rollback truncation, corrected resimulation, partial tails and pool lifecycle passed\n";
}
