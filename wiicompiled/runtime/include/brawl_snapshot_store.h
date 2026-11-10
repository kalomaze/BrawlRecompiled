#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace BrawlRollback {
// XOR edges between retained states. Replacing a state repairs its successor's
// edge, so previously predicted future states remain independently loadable.
// A single full base is shared by the history; partial tails are zero padded.
class SnapshotStore {
public:
    static constexpr size_t BlockSize=64, Capacity=16;
    struct Block { uint32_t index; uint8_t bytes[BlockSize]; };
    using Delta=std::vector<Block>;
    struct State { int frame; Delta delta; };
    std::vector<uint8_t> base, latest, scratch;
    std::vector<State> states;
    size_t lastPayload=0, peakResident=0, lastReadBytes=0;

    static void Apply(std::vector<uint8_t>& memory,const Delta& delta) {
        for(const auto& b:delta) {
            const size_t offset=size_t(b.index)*BlockSize;
            if(offset>=memory.size()) throw std::runtime_error("snapshot delta outside memory");
            for(size_t j=0;j<std::min(BlockSize,memory.size()-offset);++j) memory[offset+j]^=b.bytes[j];
        }
    }
    static Delta Combine(const Delta& a,const Delta& b) {
        Delta out;out.reserve(a.size()+b.size());size_t i=0,j=0;
        while(i<a.size() || j<b.size()) {
            if(j==b.size() || (i<a.size() && a[i].index<b[j].index))out.push_back(a[i++]);
            else if(i==a.size() || b[j].index<a[i].index)out.push_back(b[j++]);
            else {
                Block block{};block.index=a[i].index;bool changed=false;
                for(size_t k=0;k<BlockSize;++k){block.bytes[k]=a[i].bytes[k]^b[j].bytes[k];changed|=block.bytes[k]!=0;}
                if(changed)out.push_back(block);++i;++j;
            }
        }
        out.shrink_to_fit();return out;
    }
    std::vector<uint8_t> Materialize(int frame) const {
        auto out=base;
        for(const auto& s:states){Apply(out,s.delta);if(s.frame==frame)return out;}
        throw std::runtime_error("rollback: snapshot outside retained history");
    }
    const std::vector<uint8_t>& View(int frame) {
        if(!states.empty() && states.back().frame==frame)return latest;
        scratch=base;
        for(const auto& s:states){Apply(scratch,s.delta);if(s.frame==frame)return scratch;}
        throw std::runtime_error("rollback: snapshot outside retained history");
    }
    bool Contains(int frame) const {
        return std::any_of(states.begin(),states.end(),[&](const auto& s){return s.frame==frame;});
    }
    // Reader fills exactly size bytes of the packed, selected guest regions.
    template<class Reader> void Save(int frame,size_t size,Reader read,
                                     std::vector<uint8_t>* dirty=nullptr,int anchor=0) {
        lastReadBytes=0;
        if(states.empty()) {
            base.resize(size);read(0,base.data(),size);lastReadBytes=size;latest=base;
            states.push_back({frame,{}});lastPayload=size;Measure();return;
        }
        if(size!=base.size())throw std::runtime_error("snapshot region layout changed during match");
        auto at=std::lower_bound(states.begin(),states.end(),frame,[](const State& s,int f){return s.frame<f;});
        size_t index=at-states.begin();const bool replace=at!=states.end() && at->frame==frame;
        if(!replace && index==0)throw std::runtime_error("cannot insert older than snapshot base");
        const size_t reference=replace?index:index-1;
        if(dirty) {
            if(dirty->size()!=(size+BlockSize-1)/BlockSize)throw std::runtime_error("invalid dirty bitmap size");
            auto from=std::find_if(states.begin(),states.end(),[&](const State& state){return state.frame==anchor;});
            if(from==states.end())throw std::runtime_error("dirty tracker anchor was evicted");
            const size_t fromIndex=from-states.begin(),toIndex=replace?index:index-1;
            // A corrected re-run can differ from the old prediction without
            // writing those bytes again. Include history edges between the
            // actual memory anchor and the reference used for this delta.
            for(size_t i=std::min(fromIndex,toIndex)+1;i<=std::max(fromIndex,toIndex);++i)
                for(const auto& block:states[i].delta)(*dirty)[block.index]=1;
        }
        Delta change;
        // Reserve only a small estimate; never allocate a full-region delta array.
        change.reserve(8192);
        uint8_t page[4096],previousPage[4096];
        for(size_t pageOffset=0;pageOffset<size;pageOffset+=sizeof(page)) {
            const size_t pageSize=std::min(sizeof(page),size-pageOffset);
            if(dirty && std::none_of(dirty->begin()+pageOffset/BlockSize,
                dirty->begin()+(pageOffset+pageSize+BlockSize-1)/BlockSize,[](uint8_t b){return b!=0;}))continue;
            if(dirty) {
                // Copy contiguous marked runs, not untouched neighbors in a
                // 4 KiB page. The bitmap's granularity is the copy granularity.
                size_t within=0;
                while(within<pageSize) {
                    if(!(*dirty)[(pageOffset+within)/BlockSize]) {within+=BlockSize;continue;}
                    const size_t start=within;
                    do {within+=BlockSize;} while(within<pageSize && (*dirty)[(pageOffset+within)/BlockSize]);
                    const size_t count=std::min(within,pageSize)-start;
                    read(pageOffset+start,page+start,count);lastReadBytes+=count;
                    std::memcpy(previousPage+start,latest.data()+pageOffset+start,count);
                }
            } else {
                read(pageOffset,page,pageSize);lastReadBytes+=pageSize;
                std::memcpy(previousPage,latest.data()+pageOffset,pageSize);
            }
            // Reconstruct candidate blocks only; corrected saves must not
            // materialize a full historical image just to compare sparse writes.
            for(size_t i=reference+1;i<states.size();++i) {
                const auto& edge=states[i].delta;
                auto b=std::lower_bound(edge.begin(),edge.end(),pageOffset/BlockSize,
                    [](const Block& b,size_t index){return b.index<index;});
                for(;b!=edge.end() && size_t(b->index)*BlockSize<pageOffset+pageSize;++b) {
                    if(dirty && !(*dirty)[b->index])continue;
                    size_t within=size_t(b->index)*BlockSize-pageOffset;
                    for(size_t k=0;k<std::min(BlockSize,pageSize-within);++k)previousPage[within+k]^=b->bytes[k];
                }
            }
            for(size_t within=0;within<pageSize;within+=BlockSize) {
                const size_t offset=pageOffset+within,count=std::min(BlockSize,pageSize-within);
                if(dirty && !(*dirty)[offset/BlockSize])continue;
                const auto* bytes=page+within;
                if(std::memcmp(previousPage+within,bytes,count)==0)continue;
                Block block{};block.index=uint32_t(offset/BlockSize);
                for(size_t k=0;k<count;++k)block.bytes[k]=bytes[k]^previousPage[within+k];
                change.push_back(block);
            }
        }
        change.shrink_to_fit();lastPayload=change.size()*sizeof(Block);
        if(replace) {
            if(index==0)Apply(base,change);
            else states[index].delta=Combine(states[index].delta,change);
            if(index+1<states.size())states[index+1].delta=Combine(states[index+1].delta,change);
            else Apply(latest,change);
        } else {
            if(index<states.size())states[index].delta=Combine(states[index].delta,change);
            else Apply(latest,change);
            states.insert(states.begin()+index,{frame,std::move(change)});
        }
        // Match the old modulo-16 ring's frame window, not the number of save
        // events: repeated saves during resimulation must not shorten history.
        while(states.size()>1 && (states.size()>Capacity || states.back().frame-states.front().frame>=int(Capacity))) {
            Apply(base,states[1].delta);states[1].delta.clear();states[1].delta.shrink_to_fit();states.erase(states.begin());
        }
        Measure();
    }
    // Reconstruct only candidate blocks, never a full target image. The dirty
    // bitmap covers writes since anchor; history edges cover anchor -> target.
    size_t lastRestoreReadBytes=0,lastRestoreWriteBytes=0;
    template<class Reader,class Writer> void Restore(int frame,int anchor,
            std::vector<uint8_t> dirty,Reader read,Writer write) {
        auto target=std::find_if(states.begin(),states.end(),[&](const State& s){return s.frame==frame;});
        auto source=std::find_if(states.begin(),states.end(),[&](const State& s){return s.frame==anchor;});
        if(target==states.end() || source==states.end())throw std::runtime_error("restore frame outside history");
        if(dirty.size()!=(base.size()+BlockSize-1)/BlockSize)throw std::runtime_error("invalid restore bitmap");
        const size_t t=target-states.begin(),a=source-states.begin();
        for(size_t i=std::min(a,t)+1;i<=std::max(a,t);++i)
            for(const auto& b:states[i].delta)dirty[b.index]=1;
        Delta desired;
        for(size_t i=0;i<dirty.size();++i)if(dirty[i]) {
            Block b{};b.index=uint32_t(i);
            std::memcpy(b.bytes,latest.data()+i*BlockSize,std::min(BlockSize,base.size()-i*BlockSize));
            desired.push_back(b);
        }
        // Reverse XOR edges from latest to target, merging sorted block lists.
        for(size_t i=t+1;i<states.size();++i) {
            size_t j=0;
            for(const auto& b:states[i].delta) {
                while(j<desired.size() && desired[j].index<b.index)++j;
                if(j<desired.size() && desired[j].index==b.index)
                    for(size_t k=0;k<BlockSize;++k)desired[j].bytes[k]^=b.bytes[k];
            }
        }
        lastRestoreReadBytes=lastRestoreWriteBytes=0;
        uint8_t current[BlockSize];
        for(const auto& b:desired) {
            const size_t offset=size_t(b.index)*BlockSize,count=std::min(BlockSize,base.size()-offset);
            read(offset,current,count);lastRestoreReadBytes+=count;
            if(std::memcmp(current,b.bytes,count)) {write(offset,b.bytes,count);lastRestoreWriteBytes+=count;}
        }
    }
    size_t DeltaBytes() const {size_t n=0;for(const auto& s:states)n+=s.delta.size()*sizeof(Block);return n;}
    size_t ResidentBytes() const {size_t n=base.capacity()+latest.capacity()+scratch.capacity()+states.capacity()*sizeof(State);for(const auto& s:states)n+=s.delta.capacity()*sizeof(Block);return n;}
    void Measure(){peakResident=std::max(peakResident,ResidentBytes());}
};
}
