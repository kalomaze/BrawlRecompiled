#include "host_fault_context.h"

#include <array>
#include <csignal>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>

namespace {
using HostFault::Access;
void Require(bool value) { if (!value) std::abort(); }
void Record(unsigned char* bytes, uint32_t magic, uint32_t size, uint64_t esr = 0) {
    std::memcpy(bytes, &magic, 4);
    std::memcpy(bytes + 4, &size, 4);
    std::memcpy(bytes + 8, &esr, 8);
}

sigjmp_buf recovery;
volatile std::sig_atomic_t accessType;
volatile std::sig_atomic_t instructionPresent;
volatile std::sig_atomic_t esrPresent;
volatile std::sig_atomic_t capturedSignal, capturedCode;
void CaptureFault(int signal, siginfo_t* info, void* raw) {
    capturedSignal = signal;
    capturedCode = info->si_code;
    const auto fault = HostFault::Decode(raw);
    accessType = static_cast<std::sig_atomic_t>(fault.access);
    instructionPresent = fault.instruction != nullptr;
#if defined(__linux__) && defined(__aarch64__)
    // QEMU user-mode currently omits ESR records. Distinguish that limitation
    // from a decoder returning Unknown despite receiving a valid hardware ESR.
    const auto* context = static_cast<const ucontext_t*>(raw);
    const auto* bytes = context->uc_mcontext.__reserved;
    for (size_t offset = 0; offset + 8 <= sizeof(context->uc_mcontext.__reserved);) {
        uint32_t magic, size;
        std::memcpy(&magic, bytes + offset, 4);
        std::memcpy(&size, bytes + offset + 4, 4);
        if (magic == 0x45535201) esrPresent = 1;
        if (size < 16 || size > sizeof(context->uc_mcontext.__reserved) - offset) break;
        offset += size;
    }
#endif
    siglongjmp(recovery, 1);
}
}

int main() {
    Require(HostFault::MayRecoverDataFault(SIGSEGV, SEGV_ACCERR, Access::Write));
    Require(HostFault::MayRecoverDataFault(SIGSEGV, SEGV_MAPERR, Access::Read));
    Require(HostFault::MayRecoverDataFault(SIGBUS, BUS_ADRERR, Access::Write));
    Require(!HostFault::MayRecoverDataFault(SIGBUS, BUS_ADRALN, Access::Read));
    Require(!HostFault::MayRecoverDataFault(SIGBUS, BUS_ADRALN, Access::Write));
    Require(!HostFault::MayRecoverDataFault(SIGSEGV, SEGV_ACCERR, Access::Execute));
    Require(!HostFault::MayRecoverDataFault(SIGSEGV, SEGV_ACCERR, Access::Unknown));
    Require(!HostFault::MayRecoverDataFault(SIGSEGV, SI_USER, Access::Read));
    constexpr uint64_t data = uint64_t{0x24} << 26;
    Require(HostFault::ArmAccess(data | 0xf) == Access::Read);
    Require(HostFault::ArmAccess(data | 0x4f) == Access::Write);
    Require(HostFault::ArmAccess((uint64_t{0x25} << 26) | 0x40) == Access::Write);
    Require(HostFault::ArmAccess(uint64_t{0x20} << 26) == Access::Execute);
    Require(HostFault::ArmAccess(uint64_t{0x21} << 26) == Access::Execute);
    Require(HostFault::ArmAccess(data | (1 << 10)) == Access::Unknown);
    Require(HostFault::ArmAccess(data | (1 << 8)) == Access::Unknown);
    Require(HostFault::ArmAccess(0x40) == Access::Unknown);
    Require(HostFault::Decode(nullptr).access == Access::Unknown);

    std::array<unsigned char, 128> bytes{};
    Record(bytes.data(), 0x46508001, 32);
    Record(bytes.data() + 32, 0x45535201, 16, data | 0x40);
    Require(HostFault::LinuxArmAccess(bytes.data(), bytes.size()) == Access::Write);
    Require(HostFault::LinuxArmAccess(bytes.data(), 47) == Access::Unknown);
    Require(HostFault::LinuxArmAccess(nullptr, 128) == Access::Unknown);
    for (const uint32_t invalid : {0u, 8u, 17u, UINT32_MAX}) {
        Record(bytes.data(), 0x46508001, invalid);
        Require(HostFault::LinuxArmAccess(bytes.data(), bytes.size()) == Access::Unknown);
    }
    bytes.fill(0);
    Require(HostFault::LinuxArmAccess(bytes.data(), bytes.size()) == Access::Unknown);
    Record(bytes.data() + 1, 0x45535201, 16, data);
    Require(HostFault::LinuxArmAccess(bytes.data() + 1, 16) == Access::Read);

    const size_t pageSize = static_cast<size_t>(getpagesize());
    void* page = mmap(nullptr, pageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Require(page != MAP_FAILED);
    struct sigaction action{}, oldSegv{}, oldBus{};
    action.sa_sigaction = CaptureFault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    Require(sigaction(SIGSEGV, &action, &oldSegv) == 0);
    Require(sigaction(SIGBUS, &action, &oldBus) == 0);
    int missingEsr = 0;
    for (const Access expected : {Access::Read, Access::Write, Access::Execute}) {
        accessType = static_cast<std::sig_atomic_t>(Access::Unknown);
        instructionPresent = 0;
        esrPresent = 0;
        if (sigsetjmp(recovery, 1) == 0) {
            if (expected == Access::Read) (void)*static_cast<volatile unsigned char*>(page);
            else if (expected == Access::Write) *static_cast<volatile unsigned char*>(page) = 1;
            else reinterpret_cast<void (*)()>(page)();
            std::abort();
        }
        Require(instructionPresent);
#if defined(__linux__) && defined(__aarch64__)
        if (!esrPresent) {
            Require(accessType == static_cast<std::sig_atomic_t>(Access::Unknown));
            ++missingEsr;
            continue;
        }
#endif
        Require(accessType == static_cast<std::sig_atomic_t>(expected));
    }
#if defined(__linux__) && defined(__aarch64__)
    // Exclusive loads require natural alignment even when ordinary unaligned
    // loads are supported. The page is accessible: mprotect cannot fix this.
    alignas(16) unsigned char storage[16]{};
    if (sigsetjmp(recovery, 1) == 0) {
        uint64_t value;
        asm volatile("ldxr %0, [%1]" : "=r"(value) : "r"(storage + 1) : "memory");
        std::abort();
    }
    Require(capturedSignal == SIGBUS && capturedCode == BUS_ADRALN);
    Require(!HostFault::MayRecoverDataFault(capturedSignal, capturedCode, Access::Read));
#endif
    Require(sigaction(SIGSEGV, &oldSegv, nullptr) == 0);
    Require(sigaction(SIGBUS, &oldBus, nullptr) == 0);
    Require(munmap(page, pageSize) == 0);
    if (missingEsr) std::printf("Synthetic ESR tests passed; %d real faults lacked ESR metadata and remained Unknown.\n", missingEsr);
    else std::puts("Synthetic ESR and real read/write/execute fault tests passed.");
}
