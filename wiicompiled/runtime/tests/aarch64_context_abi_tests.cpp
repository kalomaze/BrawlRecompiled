#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

extern "C" void mkw_co_switch(void** targetSp, void** sourceSp);
extern "C" void* mkw_co_init(void* stackTop, void (*entry)(void*), void* argument);

namespace {
// Exercise the raw AArch64 context ABI independently of HostContext so a
// callee-saved-register or stack-frame regression is localized to this layer.

std::array<std::byte, 64 * 1024> g_workerStack{};
void* g_schedulerSp = nullptr;
void* g_workerSp = nullptr;
std::vector<int> g_events;

void SwitchAndCheck(void** target, void** source, uint64_t seed) {
    register uint64_t r19 asm("x19") = seed;
    register uint64_t r20 asm("x20") = seed + 1;
    register uint64_t r28 asm("x28") = seed + 2;
    register double v8 asm("d8") = static_cast<double>(seed + 3);
    register double v15 asm("d15") = static_cast<double>(seed + 4);
    // Keep distinct live values in callee-saved integer and FP registers on
    // both stacks; neither a lost restore nor a swapped frame may pass.
    asm volatile("" : "+r"(r19), "+r"(r20), "+r"(r28), "+w"(v8), "+w"(v15));
    mkw_co_switch(target, source);
    asm volatile("" : "+r"(r19), "+r"(r20), "+r"(r28), "+w"(v8), "+w"(v15));
    if (r19 != seed || r20 != seed + 1 || r28 != seed + 2 ||
        v8 != static_cast<double>(seed + 3) || v15 != static_cast<double>(seed + 4)) {
        std::cerr << "AArch64 context switch corrupted callee-saved registers\n";
        std::abort();
    }
}

void Worker(void*) {
    g_events.push_back(1);
    SwitchAndCheck(&g_schedulerSp, &g_workerSp, 200);
    g_events.push_back(2);
    SwitchAndCheck(&g_schedulerSp, &g_workerSp, 200);
    std::abort();
}

} // namespace

int main() {
    g_workerSp = mkw_co_init(g_workerStack.data() + g_workerStack.size(), Worker, nullptr);
    if (!g_workerSp) {
        std::cerr << "failed to create AArch64 context frame\n";
        return 1;
    }
    SwitchAndCheck(&g_workerSp, &g_schedulerSp, 100);
    if (g_events != std::vector<int>{1}) {
        std::cerr << "worker did not yield to scheduler\n";
        return 1;
    }
    SwitchAndCheck(&g_workerSp, &g_schedulerSp, 100);
    if (g_events != std::vector<int>{1, 2}) {
        std::cerr << "worker did not resume from saved context\n";
        return 1;
    }
    return 0;
}
