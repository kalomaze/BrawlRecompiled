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
volatile std::sig_atomic_t admitted;
volatile std::sig_atomic_t mappingFault;
volatile std::sig_atomic_t esrPresent;
volatile std::sig_atomic_t capturedSignal, capturedCode;
void CaptureFault(int signal, siginfo_t* info, void* raw) {
    capturedSignal = signal;
    capturedCode = info->si_code;
    const auto fault = HostFault::Decode(raw);
    admitted = HostFault::MayRecoverDataFault(signal, info->si_code, fault);
    mappingFault = fault.mappingFault;
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
    using HostFault::Platform;
    const auto readPage = HostFault::X86Context(14, 4);
    const auto writePage = HostFault::X86Context(14, 7);
    for (const auto platform : {Platform::Linux, Platform::Darwin}) {
        Require(HostFault::MayRecoverDataFault(SIGSEGV, SEGV_ACCERR, writePage, platform));
        Require(HostFault::MayRecoverDataFault(SIGSEGV, SEGV_MAPERR, readPage, platform));
        Require(!HostFault::MayRecoverDataFault(SIGSEGV, SI_USER, readPage, platform));
        Require(!HostFault::MayRecoverDataFault(SIGSEGV, 999, readPage, platform));
        Require(!HostFault::MayRecoverDataFault(SIGBUS, BUS_OBJERR, readPage, platform));
        Require(!HostFault::MayRecoverDataFault(SIGSEGV, SEGV_ACCERR, {}, platform));
        Require(!HostFault::MayRecoverDataFault(SIGSEGV, SEGV_ACCERR, HostFault::X86Context(14, 0x14), platform));
        Require(!HostFault::MayRecoverDataFault(SIGSEGV, SEGV_ACCERR, HostFault::X86Context(13, 0), platform));
    }
    Require(!HostFault::MayRecoverDataFault(SIGBUS, BUS_ADRERR, writePage, Platform::Linux));
    Require(!HostFault::MayRecoverDataFault(SIGBUS, BUS_ADRALN, readPage, Platform::Linux));
    Require(HostFault::MayRecoverDataFault(SIGBUS, BUS_ADRERR, writePage, Platform::Darwin));
    Require(!HostFault::MayRecoverDataFault(SIGSEGV, SEGV_ACCERR, writePage, Platform::Unknown));
    for (const uint64_t error : {8u, 0x20u, 0x40u, 0x8000u, 0x80000000u})
        Require(!HostFault::X86Context(14, error | 7).mappingFault);
    constexpr uint64_t data = uint64_t{0x24} << 26;
    Require(HostFault::ArmContext(data | 0xf).access == Access::Read);
    Require(HostFault::ArmContext(data | 0x4f).access == Access::Write);
    Require(HostFault::ArmContext((uint64_t{0x25} << 26) | 0x40).access == Access::Write);
    Require(HostFault::ArmContext(uint64_t{0x20} << 26).access == Access::Execute);
    Require(HostFault::ArmContext(uint64_t{0x21} << 26).access == Access::Execute);
    Require(HostFault::ArmContext(data | (1 << 10)).access == Access::Unknown);
    Require(HostFault::ArmContext(data | (1 << 8)).access == Access::Unknown);
    Require(HostFault::ArmContext(0x40).access == Access::Unknown);
    Require(HostFault::Decode(nullptr).access == Access::Unknown);
    // XNU ARM reports protection faults as BUS_ADRALN too. The ESR's actual
    // fault-status code, rather than that misleading label, decides admission.
    const auto armWritePage = HostFault::ArmContext(data | 0x4f);
    Require(HostFault::MayRecoverDataFault(SIGBUS, BUS_ADRALN, armWritePage, Platform::Darwin));
    for (uint64_t status = 0; status < 64; ++status) {
        const auto fault = HostFault::ArmContext(data | status);
        Require(fault.mappingFault == (status >= 4 && status <= 15));
        Require(HostFault::MayRecoverDataFault(SIGBUS, BUS_ADRALN, fault, Platform::Darwin) == fault.mappingFault);
    }
    Require(!HostFault::ArmContext(data | 0x8f).mappingFault); // S1PTW


    std::array<unsigned char, 128> bytes{};
    Record(bytes.data(), 0x46508001, 32);
    Record(bytes.data() + 32, 0x45535201, 16, data | 0x40);
    Require(HostFault::LinuxArmContext(bytes.data(), bytes.size()).access == Access::Write);
    Require(HostFault::LinuxArmContext(bytes.data(), 47).access == Access::Unknown);
    Require(HostFault::LinuxArmContext(nullptr, 128).access == Access::Unknown);
    for (const uint32_t invalid : {0u, 8u, 17u, UINT32_MAX}) {
        Record(bytes.data(), 0x46508001, invalid);
        Require(HostFault::LinuxArmContext(bytes.data(), bytes.size()).access == Access::Unknown);
    }
    bytes.fill(0);
    Require(HostFault::LinuxArmContext(bytes.data(), bytes.size()).access == Access::Unknown);
    Record(bytes.data() + 1, 0x45535201, 16, data);
    Require(HostFault::LinuxArmContext(bytes.data() + 1, 16).access == Access::Read);

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
            Require(!admitted);
            ++missingEsr;
            continue;
        }
#endif
        Require(accessType == static_cast<std::sig_atomic_t>(expected));
        Require(admitted == (expected != Access::Execute));
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
    Require(!admitted);
#endif
#if defined(__linux__)
    // A truncated backing file still has a writable VM mapping. mprotect
    // succeeds but cannot materialize a page beyond EOF: retrying loops.
    char name[] = "/tmp/brawl-fault-XXXXXX";
    const int fd = mkstemp(name);
    Require(fd >= 0);
    Require(unlink(name) == 0);
    Require(ftruncate(fd, static_cast<off_t>(pageSize)) == 0);
    void* filePage = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    Require(filePage != MAP_FAILED);
    *static_cast<volatile unsigned char*>(filePage) = 7;
    Require(ftruncate(fd, 0) == 0);
    for (int attempt = 0; attempt < 2; ++attempt) {
        Require(mprotect(filePage, pageSize, PROT_READ | PROT_WRITE) == 0);
        if (sigsetjmp(recovery, 1) == 0) {
            if (attempt == 0) (void)*static_cast<volatile unsigned char*>(filePage);
            else *static_cast<volatile unsigned char*>(filePage) = 9;
            std::abort();
        }
        Require(capturedSignal == SIGBUS && capturedCode == BUS_ADRERR);
#if defined(__x86_64__)
        Require(mappingFault); // The CPU sees a page fault; Linux identifies pager failure.
#endif
        Require(!admitted);
    }
    Require(munmap(filePage, pageSize) == 0);
    Require(close(fd) == 0);
    std::puts("Truncated-mapping read/write faults rejected despite successful mprotect.");
#endif
    Require(sigaction(SIGSEGV, &oldSegv, nullptr) == 0);
    Require(sigaction(SIGBUS, &oldBus, nullptr) == 0);
    Require(munmap(page, pageSize) == 0);
    if (missingEsr) std::printf("Synthetic ESR tests passed; %d real faults lacked ESR metadata and remained Unknown.\n", missingEsr);
    else std::puts("Synthetic ESR and real read/write/execute fault tests passed.");
}
