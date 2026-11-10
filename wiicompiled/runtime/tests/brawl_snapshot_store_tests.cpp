#include "brawl_snapshot_store.h"
#include <cassert>
#include <map>
#include <random>
#include <iostream>
using BrawlRollback::SnapshotStore;
int main(){
    SnapshotStore store;std::map<int,std::vector<uint8_t>> reference;
    std::vector<uint8_t> ram(131072+13);std::mt19937 random(713);
    auto save=[&](int frame){
        store.Save(frame,ram.size(),[&](size_t offset,uint8_t* out,size_t count){std::memcpy(out,ram.data()+offset,count);});
        reference[frame]=ram;
        while(reference.rbegin()->first-reference.begin()->first>=16)reference.erase(reference.begin());
        assert(store.states.size()==reference.size());
        for(const auto& [f,bytes]:reference){assert(store.Materialize(f)==bytes);assert(store.View(f)==bytes);}
    };
    save(-1);
    for(int frame=0;frame<300;++frame){
        for(int i=0;i<7;++i)ram[random()%ram.size()]=uint8_t(random());
        save(frame);
        if(frame>8 && frame%3==0){
            int back=frame-int(random()%7)-1;ram=store.Materialize(back);
            // Correct a prediction, then overwrite already-saved future frames.
            for(int f=back;f<=frame;++f){
                for(int i=0;i<3;++i)ram[random()%ram.size()]^=uint8_t(random());
                save(f);
            }
        }
    }
    SnapshotStore gaps;std::vector<uint8_t> x(129,0);
    auto put=[&](int f){gaps.Save(f,x.size(),[&](size_t o,uint8_t* p,size_t n){std::memcpy(p,x.data()+o,n);});};
    put(0);x[128]=9;put(4);x[0]=7;put(2);
    assert(gaps.Materialize(4)[0]==0 && gaps.Materialize(4)[128]==9);
    x[128]=5;put(0);assert(gaps.Materialize(2)[128]==9 && gaps.Materialize(4)[0]==0);
    bool rejected=false;try{gaps.Materialize(99);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    std::cout<<"Snapshot eviction, sparse saves, corrected predictions, base replacement, tails and all retained full images passed\n";
}
