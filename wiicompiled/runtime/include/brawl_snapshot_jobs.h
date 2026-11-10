#pragma once
#include "brawl_snapshot_job.h"
#include <array>
#include <exception>
#include <vector>

namespace BrawlRollback {
// Brawl Online's persistent pool: six helpers, five chunks per RAM operation.
class SnapshotJobs {
    job::JobSysCtx context;
    job::JobTaskThread* dispatch;
    std::vector<std::thread> workers;
public:
    static constexpr size_t Chunks=5, SaveChunks=2*Chunks; // Five save chunks for each of Dolphin's two RAM banks.
    SnapshotJobs() {
        context.activate();
        dispatch=context.initialize_worker(0,nullptr);
        try {
            for(uint64_t i=1;i<=6;++i) {
                auto* worker=context.initialize_worker(static_cast<int64_t>(i*0x9e3779b97f4a7c15ULL),nullptr);
                workers.emplace_back([worker]{worker->wait_for_termination();});
            }
        } catch(...) {
            context.deactivate();
            for(auto& worker:workers)worker.join();
            context.release_workers();
            throw;
        }
    }
    ~SnapshotJobs() {
        context.deactivate();
        for(auto& worker:workers)worker.join();
        context.release_workers();
    }
    SnapshotJobs(const SnapshotJobs&)=delete;
    SnapshotJobs& operator=(const SnapshotJobs&)=delete;
    job::Job* Start(job::JobFunction work) { return job::KickRootJob(dispatch,std::move(work)); }
    void Wait(job::Job*& work) {
        if(work){job::DrainJobsUntilComplete(dispatch,work);work=nullptr;}
    }
    template<size_t Count=Chunks,class Work> void Run(Work work) { RunPrepared<Count>([]{},std::move(work)); }
    template<size_t Count=Chunks,class Prepare,class Work> void RunPrepared(Prepare prepare,Work work) {
        std::array<std::exception_ptr,Count> errors{};
        std::exception_ptr preparationError;
        auto* root=job::KickRootJob(dispatch,[&](job::JobTaskThread& thread,job::Job& parent) {
            try {prepare();} catch(...) {preparationError=std::current_exception();return;}
            std::array<job::Job*,Count> jobs{};
            for(size_t i=0;i<Count;++i)
                jobs[i]=thread.create_job_as_child(parent,[work,i,&errors](job::JobTaskThread&,job::Job&) mutable {
                    try {work(i);} catch(...) {errors[i]=std::current_exception();}
                });
            thread.do_work_and_kick_jobs(jobs.data(),static_cast<uint16_t>(jobs.size()));
        });
        job::DrainJobsUntilComplete(dispatch,root);
        if(preparationError)std::rethrow_exception(preparationError);
        for(const auto& error:errors)if(error)std::rethrow_exception(error);
    }
    // Called only after joining the operation; no jobs remain outstanding.
    size_t ResidentBytes() const {
        size_t bytes=sizeof(*this)+workers.capacity()*sizeof(std::thread);
        for(size_t i=0;i<context.task_count.load();++i) {
            bytes+=sizeof(job::JobTaskThread);
            if(context.task[i]->cur_block)bytes+=sizeof(job::JobAllocBlock);
        }
        return bytes;
    }
};
}
