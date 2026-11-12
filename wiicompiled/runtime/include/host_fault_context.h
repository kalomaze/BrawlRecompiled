#pragma once

#include <cstddef>
#include <csignal>
#include <cstdint>
#include <cstring>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#if defined(__aarch64__)
#include <mach/arm/thread_status.h>
#endif
#else
#include <ucontext.h>
#endif

namespace HostFault {

// Match Windows' access-type values in the shared crash record. Unknown must
// not silently become Read when an OS/emulator supplies no access metadata.
enum class Access : uint32_t { Read = 0, Write = 1, Execute = 8, Unknown = UINT32_MAX };
struct Context {
    Access access = Access::Unknown;
    const void* instruction = nullptr;
    bool mappingFault = false;
};

enum class Platform { Linux, Darwin, Unknown };
#if defined(__APPLE__)
inline constexpr Platform kPlatform = Platform::Darwin;
#elif defined(__linux__)
inline constexpr Platform kPlatform = Platform::Linux;
#else
inline constexpr Platform kPlatform = Platform::Unknown;
#endif

inline bool MayRecoverDataFault(int signal, int code, const Context& fault,
                                Platform platform = kPlatform) noexcept {
    if (!fault.mappingFault || (fault.access != Access::Read && fault.access != Access::Write)) return false;
    if (platform == Platform::Unknown) return false;
    if (signal == SIGSEGV) return code == SEGV_MAPERR || code == SEGV_ACCERR;
    // Linux uses SIGBUS for pager/alignment/hardware errors, not mprotect
    // faults. XNU sends protection failures as SIGBUS too, labelled BUS_ADRALN
    // on ARM and BUS_ADRERR on x86 regardless of their actual hardware cause.
    // Darwin therefore needs the syndrome/trap check above, not a signal-code
    // alignment blacklist. Its POSIX metadata still cannot identify every
    // pager error: an EOF mapping can carry an ordinary translation fault.
    return platform == Platform::Darwin && signal == SIGBUS &&
           (code == BUS_ADRALN || code == BUS_ADRERR);
}

inline Context ArmContext(uint64_t esr) noexcept {
    Context result;
    // ESR_ELx: EC [31:26], WnR [6]. See Linux arch/arm64/include/asm/esr.h.
    const auto exception = (esr >> 26) & 0x3f;
    if (exception == 0x20 || exception == 0x21) {
        result.access = Access::Execute;
        return result;
    }
    if (exception != 0x24 && exception != 0x25) return result;
    // An invalid fault address (FnV) or cache-maintenance operation (CM) is
    // not a recoverable guest load/store, regardless of the reported WnR bit.
    if (esr & ((1u << 10) | (1u << 8))) return result;
    result.access = (esr & (1u << 6)) ? Access::Write : Access::Read;
    const auto status = esr & 0x3f;
    // Translation, access-flag, or permission faults at levels 0..3. Reject
    // address-size/alignment faults, external aborts, tag checks and faults
    // during a stage-1 translation-table walk (S1PTW), not ordinary mappings.
    result.mappingFault = status >= 4 && status <= 15 && (esr & (1u << 7)) == 0;
    return result;
}

inline Context X86Context(uint64_t trap, uint64_t error) noexcept {
    Context result;
    if (trap != 14) return result; // #PF; error bits mean something else for #GP, etc.
    result.access = (error & 0x10) ? Access::Execute : ((error & 2) ? Access::Write : Access::Read);
    // Only P/W/U are ordinary data-page faults. Reserved PTE bits, protection
    // keys, shadow stacks, SGX/RMP violations cannot be fixed with mprotect.
    result.mappingFault = (error & ~uint64_t{7}) == 0;
    return result;
}

inline Context LinuxArmContext(const void* reserved, size_t length) noexcept {
    // Linux UAPI asm/sigcontext.h requires ESR records inside __reserved[],
    // never in extra_context. Do not follow extension pointers from a handler.
    const auto* bytes = static_cast<const unsigned char*>(reserved);
    if (!bytes) return {};
    for (size_t offset = 0; length - offset >= 8;) {
        uint32_t magic, size;
        std::memcpy(&magic, bytes + offset, 4);
        std::memcpy(&size, bytes + offset + 4, 4);
        if (magic == 0 || size < 16 || size % 16 != 0 || size > length - offset)
            return {};
        if (magic == 0x45535201) { // ESR_MAGIC
            uint64_t esr;
            std::memcpy(&esr, bytes + offset + 8, 8);
            return ArmContext(esr);
        }
        offset += size;
    }
    return {};
}

inline Context Decode(const void* raw) noexcept {
    Context result;
    if (!raw) return result;
    const auto* context = static_cast<const ucontext_t*>(raw);
#if defined(__APPLE__)
    if (!context->uc_mcontext) return result;
#if defined(__aarch64__)
    result = ArmContext(context->uc_mcontext->__es.__esr);
    result.instruction = reinterpret_cast<const void*>(arm_thread_state64_get_pc(context->uc_mcontext->__ss));
#elif defined(__x86_64__)
    result = X86Context(context->uc_mcontext->__es.__trapno, context->uc_mcontext->__es.__err);
    result.instruction = reinterpret_cast<const void*>(context->uc_mcontext->__ss.__rip);
#endif
#elif defined(__linux__) && defined(__aarch64__)
    result = LinuxArmContext(context->uc_mcontext.__reserved, sizeof(context->uc_mcontext.__reserved));
    result.instruction = reinterpret_cast<const void*>(context->uc_mcontext.pc);
#elif defined(__linux__) && defined(__x86_64__)
    result = X86Context(context->uc_mcontext.gregs[REG_TRAPNO], context->uc_mcontext.gregs[REG_ERR]);
    result.instruction = reinterpret_cast<const void*>(context->uc_mcontext.gregs[REG_RIP]);
#endif
    return result;
}

} // namespace HostFault
