// Build twice: exercise the actual memory and mod reporters, not a formatting
// facsimile. No guest execution or renderer is needed.
#if defined(TEST_MOD_FAULT_REPORT)
#include "../src/recomp_mod_loader.cpp"
namespace MemoryInline { uintptr_t g_fullWritablePageBias[kPageCount]{}; }
void Memory::RefreshWritableFastPathsForExecutableRanges() {}
#if MKW_FLAT_MEMORY
namespace GuestFlat { void RegisterExecutableRange(uint32_t, uint32_t) {} }
#endif
namespace RuntimePlatform {
std::optional<std::filesystem::path> ExecutableDirectory() noexcept { return std::nullopt; }
}
#else
#include "../src/guest_flat_memory.cpp"
namespace RecompMod {
uint32_t CurrentTranslatedExecutionAddress() noexcept { return g_currentTranslatedExecutionAddress; }
bool HandleExecutableWrite(uint32_t, size_t, uint64_t) { return false; }
}
#endif
namespace GuestCodePatch { bool NoteWrite(uint32_t) { return false; } }

#include <csignal>
#include <new>
#include <sys/resource.h>
#include <sys/wait.h>

namespace {
volatile std::sig_atomic_t forbidAllocation = 0;
volatile std::sig_atomic_t unknownStore = 0;
void Require(bool condition) { if (!condition) std::abort(); }
[[noreturn]] void Trigger(int) {
    forbidAllocation = 1;
#if defined(TEST_MOD_FAULT_REPORT)
    // The record is already published in production. Prepare it before
    // entering signal context here too.
    if (unknownStore) RecompMod::HandleExecutableWrite(0x80001004, 1, 0);
    else RecompMod::CheckExecutableWrite(0x80001004, 4, 0x123456789abcdef0ULL);
    std::_Exit(80); // An unsupported executable write must never be permitted.
#else
    GuestFlat::ReportFatalGuestFault("MMIO read blocked", 0xCC000000, false, "hardware has no backing memory");
#endif
}
class LockedStream final : public std::streambuf {
    std::mutex mutex_;
    std::streamsize xsputn(const char*, std::streamsize) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::raise(SIGUSR1); // Reporter must not reenter this locked stream.
        std::_Exit(78);
    }
};
}

void* operator new(std::size_t size) {
    if (forbidAllocation) std::_Exit(74);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void SystemBridge::DumpCpuState(const CpuContext*) { std::_Exit(75); }
void ShowRuntimeFatalPopup(std::string_view, std::string_view) noexcept { std::_Exit(76); }

int main() {
#if defined(TEST_MOD_FAULT_REPORT)
    RecompMod::RegisterExecutableRange(0x80001000, 0x80002000, "sora_scene");
#endif
#if defined(TEST_MOD_FAULT_REPORT)
    constexpr int modeCount = 4;
#else
    constexpr int modeCount = 2;
#endif
    for (int mode = 0; mode < modeCount; ++mode) {
        const bool locked = (mode & 1) != 0;
        unknownStore = (mode & 2) != 0;
        int capture[2];
        Require(pipe(capture) == 0);
        const pid_t child = fork();
        Require(child >= 0);
        if (child == 0) {
            close(capture[0]);
            Require(dup2(capture[1], STDERR_FILENO) >= 0);
            close(capture[1]);
            const rlimit noCore{0, 0};
            setrlimit(RLIMIT_CORE, &noCore);
            alarm(3); // Old locked-stream reporting deadlocks; fail boundedly.
            std::signal(SIGUSR1, Trigger);
            std::signal(SIGABRT, [](int) { std::_Exit(77); }); // Unsafe runtime abort handler must be bypassed.
            RecompMod::g_currentTranslatedExecutionAddress = 0x80012345;
            // A diagnostic must survive a corrupt saved context pointer.
            g_currentCpuContext = reinterpret_cast<CpuContext*>(uintptr_t{1});
            if (locked) {
                LockedStream buffer;
                std::cerr.rdbuf(&buffer);
                std::cerr << "interrupted write";
            }
            std::raise(SIGUSR1);
            std::_Exit(79);
        }
        close(capture[1]);
        std::string output;
        char bytes[1024];
        ssize_t n;
        while ((n = read(capture[0], bytes, sizeof(bytes))) > 0) output.append(bytes, static_cast<size_t>(n));
        close(capture[0]);
        int status;
        Require(waitpid(child, &status, 0) == child);
        Require(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE);
        Require(output.find("FATAL") != std::string::npos);
        Require(output.find("0x80012345") != std::string::npos);
        Require(output.find("host CPU context: 0x") != std::string::npos);
#if defined(TEST_MOD_FAULT_REPORT)
        Require(output.find("0x80001004") != std::string::npos);
        if (unknownStore) {
            Require(output.find("width/value: unavailable in fault record") != std::string::npos);
            Require(output.find("width: ") == std::string::npos);
            Require(output.find("value: 0x") == std::string::npos);
        } else {
            Require(output.find("0x123456789ABCDEF0") != std::string::npos);
            Require(output.find("width: 4") != std::string::npos);
        }
        Require(output.find("0x80001000-0x80002000 sora_scene") != std::string::npos);
#else
        Require(output.find("0xCC000000") != std::string::npos);
        Require(output.find("access: read; size unknown") != std::string::npos);
        Require(output.find("hardware has no backing memory") != std::string::npos);
#endif
    }
}
