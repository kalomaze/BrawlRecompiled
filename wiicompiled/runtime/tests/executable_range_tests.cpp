// Exercise the real registry while interrupting its writer lock. Including the
// implementation exposes the private mutex without adding a production test API.
#include "../src/recomp_mod_loader.cpp"
#include <csignal>
#include <thread>

namespace MemoryInline { uintptr_t g_fullWritablePageBias[kPageCount]{}; }
void Memory::RefreshWritableFastPathsForExecutableRanges() {}
namespace GuestCodePatch { bool NoteWrite(uint32_t) { return false; } }
#if MKW_FLAT_MEMORY
namespace GuestFlat { void RegisterExecutableRange(uint32_t, uint32_t) {} }
#endif
namespace RuntimePlatform {
std::optional<std::filesystem::path> ExecutableDirectory() noexcept { return std::nullopt; }
}
void SystemBridge::DumpCpuState(const CpuContext*) { std::abort(); }
void ShowRuntimeFatalPopup(std::string_view, std::string_view) noexcept { std::abort(); }

namespace {
volatile std::sig_atomic_t recovered = 0;
void LookupInterrupted(int) {
    const auto hit = FindExecutableRange(0x80001010, 4);
    recovered = hit && hit->start == 0x80001000 && hit->name.size() == 128;
}
void Require(bool condition) { if (!condition) std::abort(); }
}

int main() {
    Require(!FindExecutableRange(0x80001000, 4));
    const std::string name(128, 'a');
    RecompMod::RegisterExecutableRange(0x80001000, 0x80002000, name);
    const auto count = ExecutableRanges().size();
    RecompMod::RegisterExecutableRange(0x80001000, 0x80002000, name);
    Require(ExecutableRanges().size() == count);
    RecompMod::RegisterExecutableRange(0x80001800, 0x80002800, "overlap");
    Require(FindExecutableRange(0x80001800, 4)->name == name);
    Require(FindExecutableRange(0xC0001010, 4)->start == 0xC0001000);
    Require(!FindExecutableRange(0x80002800, 4));
    Require(FindExecutableRange(0x80000fff, 2)->start == 0x80001000);

    std::signal(SIGUSR1, LookupInterrupted);
    {
        std::lock_guard<std::mutex> interrupted(ModMutex());
        std::raise(SIGUSR1);
    }
    std::signal(SIGUSR1, SIG_DFL);
    Require(recovered);

    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (uint32_t i = 0; i < 1024; ++i)
            RecompMod::RegisterExecutableRange(0x50000000 + i * 4096, 0x50000004 + i * 4096, "concurrent");
        done.store(true, std::memory_order_release);
    });
    do {
        Require(FindExecutableRange(0x80001800, 4)->name == name);
        for (uint32_t i = 0; i < 1024; ++i) {
            const auto hit = FindExecutableRange(0x50000000 + i * 4096, 4);
            if (hit) Require(hit->name == "concurrent" && hit->start == 0x50000000 + i * 4096);
        }
    } while (!done.load(std::memory_order_acquire));
    writer.join();
    for (uint32_t i = 0; i < 1024; ++i)
        Require(FindExecutableRange(0x50000000 + i * 4096, 4) != nullptr);
}
