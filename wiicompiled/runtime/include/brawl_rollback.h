#pragma once
#include <cstdint>
struct CpuContext;
namespace BrawlRollback {
using GuestFunction = void (*)(CpuContext*);
void Run(CpuContext* ctx, GuestFunction gameProc);
void RunLoop(CpuContext* ctx, GuestFunction gameProc);
void SeedCrossplayStage();
bool DrivesLoop();
void AnchorPads();
void UpdateSystem(CpuContext* ctx, GuestFunction update);
}
