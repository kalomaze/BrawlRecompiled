#include "save_states.h"
#include "host_context.h"
#include "abi_bridge.h"
#include "fiber_manager.h"
#include "hle/os/os_internal.h"
#include "guest_code_patch.h"
#include <mutex>
#include <unordered_set>
#include <vector>
#include <cstdio>
#include <cstdlib>

namespace {
// Lightweight stand-ins for the guest machine around the real save-state code.
std::vector<uint8_t> mem1(Memory::kMem1Size), mem2(Memory::kMem2Size);
CpuContext cpu{};
HostContext::Handle scheduler, worker;
int observed = 0, phase = 0;
void require(bool yes) { if (!yes) std::abort(); }
[[gnu::noinline]] void SaveFromDeepStack() {
    volatile char padding[128u << 10];
    padding[0] = 0;
    asm volatile("" : : "r"(padding) : "memory");
    SaveStates::RequestSave();
    SaveStates::FrameBoundary();
    asm volatile("" : : "r"(padding) : "memory");
}
void Worker(void*) {
    int top;
    SaveStates::MarkStackTop(&top + 1);
    volatile int value = 6;
    for (;;) {
        value = value + 1;
        observed = value;
        SaveStates::SwitchFiber(scheduler);
    }
}
}
namespace MemoryInline {
uintptr_t g_fullPageBias[kPageCount]{};
uint32_t Read32Slow(uint32_t) { return 0; }
}
uint8_t* Memory::GetPointer(uint32_t address, size_t size) {
    if (address == 0x80000000u && size == mem1.size()) return mem1.data();
    if (address == 0x90000000u && size == mem2.size()) return mem2.data();
    std::abort();
}
bool Memory::Contains(uint32_t, size_t) { return false; }
CpuContext& GetPersistentCpuContext() { return cpu; }
size_t DvdPendingCallbackCount() { return 0; }
namespace DetClock::detail { bool g_enabled = false; }
namespace GuestCodePatch { void Sync() {} }
namespace OsHleInternal {
std::mutex gSleepTimerMutex, gOutstandingParkMutex;
std::vector<SleepTimerEntry> gSleepTimers;
std::unordered_set<uint32_t> gOutstandingParks;
}
std::vector<std::pair<uint32_t, Fiber::GuestFiber>> Fiber::GuestFiberManager::CopyFibers(uint32_t& current) {
    current = 1;
    return {{1, {.fiber = worker}}};
}
void Fiber::GuestFiberManager::RestoreFibers(const std::vector<std::pair<uint32_t, Fiber::GuestFiber>>&, uint32_t) {}

int main() {
    int top;
    require(HostContext::InitializeScheduler(&scheduler));
    SaveStates::MarkStackTop(&top + 1);
    worker = HostContext::Create(256u << 10, Worker, nullptr);
    require(worker != nullptr);
    SaveStates::SwitchFiber(worker);
    require(observed == 7);
    mem1[7] = 11;
    mem2[9] = 12;
    cpu.gpr[3] = 42;
    SaveStates::RequestSave();
    SaveStates::FrameBoundary();
    if (phase == 0) {
        require(SaveStates::Exists(0));
        phase = 1;
        mem1[7] = 99;
        mem2[9] = 98;
        cpu.gpr[3] = 100;
        SaveStates::SwitchFiber(worker);
        require(observed == 8);
        SaveStates::RequestLoad();
        SaveStates::FrameBoundary();
        std::abort(); // A load must resume at the saved FrameBoundary, not here.
    }
    require(mem1[7] == 11 && mem2[9] == 12 && cpu.gpr[3] == 42);
    SaveStates::SwitchFiber(worker);
    require(observed == 8); // Restored worker's local value was 7, not 8.
    SaveStates::SetSlot(1);
    SaveFromDeepStack();
    mem1[7] = 77;
    SaveStates::RequestLoad();
    SaveStates::FrameBoundary();
    require(mem1[7] == 77);
    require(SaveStates::TakeMessage().find("stack depth") != std::string::npos);
    SaveStates::ForgetFiber(worker);
    HostContext::Destroy(worker);
    SaveStates::ForgetFiber(scheduler);
    HostContext::ShutdownScheduler(scheduler);
    std::puts("Guest RAM, CPU state, and suspended fiber continuation restored.");
}
