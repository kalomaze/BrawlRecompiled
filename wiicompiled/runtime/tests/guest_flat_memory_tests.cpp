#include "../src/guest_flat_memory.cpp"
#include <csignal>
#include <thread>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "system_bridge.h"
#include "recomp_mod_loader.h"

namespace {
std::atomic<unsigned> patchNotifications{0};
bool rearmDuringPolicy = false;
}
namespace GuestCodePatch {
bool NoteWrite(uint32_t) { patchNotifications.fetch_add(1, std::memory_order_relaxed); return true; }
}

// No guest execution is needed to exercise the real mappings and protection recovery.
namespace RecompMod {
uint32_t CurrentTranslatedExecutionAddress() noexcept { return 0; }
bool HandleExecutableWrite(uint32_t address, size_t, uint64_t) {
    GuestCodePatch::NoteWrite(address);
    if (rearmDuringPolicy) {
        // Model Sync consuming the early notification and rearming before the
        // handler's permission change. The post-open notification must survive.
        patchNotifications.store(0);
        GuestFlat::ReprotectExecutablePage(address);
    }
    return false;
}
}
void SystemBridge::DumpCpuState(const CpuContext*) { std::abort(); }
void ShowRuntimeFatalPopup(std::string_view, std::string_view) noexcept { std::abort(); }

namespace {
volatile std::sig_atomic_t recovered = 0;
volatile std::sig_atomic_t faultIsWrite = 1;
std::atomic<uint32_t> recoveryAddress{0x32000000u};
void RecoverActualFault(int, siginfo_t* info, void*) {
    if (!GuestFlat::HandleAccessViolation(info->si_addr, faultIsWrite != 0,
                                         reinterpret_cast<const void*>(&RecoverActualFault))) std::_Exit(2);
}
void RecoverWhileInterrupted(int) {
    auto* base = reinterpret_cast<uint8_t*>(GuestFlat::kFixedFlatGuestBase);
    recovered = GuestFlat::HandleAccessViolation(base + recoveryAddress.load(), true,
                                                 reinterpret_cast<const void*>(&RecoverWhileInterrupted));
}
}

int main() {
    GuestFlat::Initialize({
        {0x00000000u, 0x4000u, GuestFlat::Backing::Mem1},
        {0x80000000u, 0x4000u, GuestFlat::Backing::Mem1},
        {0x10000000u, 0x4000u, GuestFlat::Backing::Mem2},
        {0x90000000u, 0x4000u, GuestFlat::Backing::Mem2},
    });
    if (!GuestFlat::IsActive()) {
        std::cerr << "guest address space did not become active\n";
        return 1;
    }
    if (GuestFlat::RequiresCheckedAccess() !=
        (static_cast<size_t>(getpagesize()) > GuestFlat::kGuestPageSize)) {
        std::cerr << "guest access mode does not reflect the host page size\n";
        return 1;
    }
    auto* mem1Physical = GuestFlat::HostPointer(0x00000000u);
    auto* mem1Cached = GuestFlat::HostPointer(0x80000000u);
    auto* mem2Physical = GuestFlat::HostPointer(0x10000000u);
    auto* mem2Cached = GuestFlat::HostPointer(0x90000000u);
    if (!mem1Physical || !mem1Cached || !mem2Physical || !mem2Cached) {
        std::cerr << "missing host alias\n";
        return 1;
    }
    if (GuestFlat::HostPointer(0x4000u) != nullptr ||
        GuestFlat::HostPointer(0xa0000000u) != nullptr) {
        std::cerr << "unmapped guest address resolved to host memory\n";
        return 1;
    }
    mem1Physical[7] = 0x5a;
    mem2Cached[9] = 0xa5;
    const auto* guest = reinterpret_cast<const uint8_t*>(GuestFlat::kFixedFlatGuestBase);
    if (mem1Cached[7] != 0x5a || guest[0x80000007u] != 0x5a ||
        mem2Physical[9] != 0xa5 || guest[0x10000009u] != 0xa5) {
        std::cerr << "guest aliases are not coherent\n";
        return 1;
    }
    auto* guestWritable = reinterpret_cast<uint8_t*>(GuestFlat::kFixedFlatGuestBase);
    guestWritable[0x80000008u] = 0x3c;
    guestWritable[0x1000000au] = 0xc3;
    if (mem1Physical[8] != 0x3c || mem2Cached[10] != 0xc3) {
        std::cerr << "guest writes were not visible through host aliases\n";
        return 1;
    }

    GuestFlat::Initialize({
        {0x00000000u, 0x4000u, GuestFlat::Backing::Mem1},
        {0x80000000u, 0x4000u, GuestFlat::Backing::Mem1},
        {0x10000000u, 0x4000u, GuestFlat::Backing::Mem2},
        {0x90000000u, 0x4000u, GuestFlat::Backing::Mem2},
    });
    try {
        GuestFlat::Initialize({{0x00000000u, 0x8000u, GuestFlat::Backing::Mem1}});
        std::cerr << "guest address space accepted a different layout\n";
        return 1;
    } catch (const std::runtime_error&) {
    }
    if (!GuestFlat::RequiresCheckedAccess()) {
        constexpr uint32_t address = 0x80001000u;
        auto* page = guestWritable + address;
        GuestFlat::RegisterExecutableRange(address, address + 0x1000u);
        if (GuestFlat::HandleAccessViolation(page, false, reinterpret_cast<const void*>(&main)) ||
            patchNotifications.load() != 0) {
            std::cerr << "read fault entered executable-write recovery\n";
            return 1;
        }
        if (GuestFlat::HandleAccessViolation(page, true, page) ||
            GuestFlat::HandleAccessViolation(page, true, nullptr)) {
            std::cerr << "invalid host instruction pointer passed write-guard recovery\n";
            return 1;
        }
        if (!GuestFlat::HandleAccessViolation(page, true, reinterpret_cast<const void*>(&main))) {
            std::cerr << "permitted executable write was not recovered\n";
            return 1;
        }
        *page = 0x6b;
        if (mem1Physical[0x1000] != 0x6b) return 1;
        // A second thread's delayed fault must still be classified as an
        // executable-page fault after the first handler reopened the page.
        const auto unhandledFaults = GuestFlat::Counters().unhandled;
        if (!GuestFlat::HandleAccessViolation(page, true, reinterpret_cast<const void*>(&main)) ||
            GuestFlat::Counters().unhandled != unhandledFaults) return 1;
        // Recovery must work even if the interrupted operation owns StateMutex.
        recoveryAddress.store(address);
        std::signal(SIGUSR1, RecoverWhileInterrupted);
        {
            std::lock_guard<std::mutex> interrupted(GuestFlat::StateMutex());
            std::raise(SIGUSR1);
        }
        std::signal(SIGUSR1, SIG_DFL);
        if (!recovered) return 1;
        recoveryAddress.store(0x32000000u);

        rearmDuringPolicy = true;
        if (!GuestFlat::HandleAccessViolation(page, true, reinterpret_cast<const void*>(&main)) ||
            patchNotifications.load() != 1) return 1;
        rearmDuringPolicy = false;
        *page = 0x7c;
        if (mem1Physical[0x1000] != 0x7c) return 1;

        // If Sync consumes the post-open notification before the store retries,
        // its rearm must cause a fresh real write fault and a fresh notification.
        struct sigaction action{}, previous{};
        action.sa_sigaction = RecoverActualFault;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGSEGV, &action, &previous) != 0) return 1;
        patchNotifications.store(0);
        GuestFlat::ReprotectExecutablePage(address);
        *page = 0x7d;
        if (patchNotifications.load() == 0 || mem1Physical[0x1000] != 0x7d) return 1;
        std::thread rearm([&] {
            for (unsigned i = 0; i < 1000; ++i) GuestFlat::ReprotectExecutablePage(address);
        });
        auto* volatilePage = static_cast<volatile uint8_t*>(page);
        for (unsigned i = 0; i < 1000; ++i) *volatilePage = static_cast<uint8_t>(i);
        rearm.join();
        if (mem1Physical[0x1000] != static_cast<uint8_t>(999)) return 1;
        if (sigaction(SIGSEGV, &previous, nullptr) != 0) return 1;

        GuestFlat::ReprotectExecutablePage(address);
        // Make the OS protection change fail, rather than mocking its return value.
        if (munmap(page, static_cast<size_t>(getpagesize())) != 0) return 1;
        try {
            GuestFlat::ReprotectExecutablePage(address);
            std::cerr << "failed executable rearm silently ignored\n";
            return 1;
        } catch (const std::runtime_error&) {
        }
        if (GuestFlat::HandleAccessViolation(page, true, reinterpret_cast<const void*>(&main))) {
            std::cerr << "failed protection recovery was reported as handled\n";
            return 1;
        }
    }
    // Reject unknown addresses even when interrupted code owns the mapper lock.
    std::signal(SIGUSR1, RecoverWhileInterrupted);
    {
        std::lock_guard<std::mutex> interrupted(GuestFlat::StateMutex());
        std::raise(SIGUSR1);
    }
    std::signal(SIGUSR1, SIG_DFL);
    if (recovered) return 1;

    const auto before = GuestFlat::Counters().unhandled;
    std::atomic<bool> rejected{true};
    auto reject = [&] {
        if (GuestFlat::HandleAccessViolation(guestWritable + 0x33000000u, true,
                                             reinterpret_cast<const void*>(&main))) rejected = false;
    };
    std::thread first(reject), second(reject);
    first.join();
    second.join();
    if (!rejected || GuestFlat::Counters().unhandled != before + 2) return 1;

    // Guest RAM is data, never native host instructions. Retrying an execution
    // fault after granting read/write permission would fault forever.
    auto* executionFault = guestWritable + 0x40000000u;
    if (GuestFlat::HandleAccessViolation(executionFault, false, executionFault) ||
        GuestFlat::HandleAccessViolation(executionFault, true, executionFault)) {
        std::cerr << "native execution in guest RAM was reported as recovered\n";
        return 1;
    }
    // Check actual OS access after attempted recovery, not just its return value.
    // A child exits through the signal handler only if the access still faults.
    auto expectFault = [&](uint32_t address, bool write, int expectedExit) {
        const pid_t child = fork();
        if (child < 0) return false;
        if (child == 0) {
            faultIsWrite = write;
            struct sigaction action{};
            action.sa_sigaction = RecoverActualFault;
            action.sa_flags = SA_SIGINFO;
            sigemptyset(&action.sa_mask);
            if (sigaction(SIGSEGV, &action, nullptr) != 0) std::_Exit(3);
            auto* ptr = reinterpret_cast<volatile uint8_t*>(guestWritable + address);
            if (write) *ptr = 0x5c;
            else { const auto value = *ptr; (void)value; }
            std::_Exit(4);
        }
        int status = 0;
        return waitpid(child, &status, 0) == child && WIFEXITED(status) &&
               WEXITSTATUS(status) == expectedExit;
    };
    for (const bool write : {false, true}) {
        if (GuestFlat::HandleAccessViolation(guestWritable + 0x30000000u, write,
                                             reinterpret_cast<const void*>(&main)) ||
            !expectFault(0x30000000u, write, 2)) {
            std::cerr << "unsupported guest access acquired backing memory\n";
            return 1;
        }
    }
    constexpr uint32_t scratch = 0xCCCC0000u;
    if (!GuestFlat::HandleAccessViolation(guestWritable + scratch, false,
                                          reinterpret_cast<const void*>(&main)) ||
        guest[scratch] != 0 || !expectFault(scratch, true, 1)) {
        std::cerr << "MMIO compatibility page is not read-only zero storage\n";
        return 1;
    }
    return 0;
}
