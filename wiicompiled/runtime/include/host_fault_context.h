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
};

inline bool MayRecoverDataFault(int signal, int code, Access access) noexcept {
    if ((signal != SIGSEGV && signal != SIGBUS) || code <= 0) return false;
    // Changing page permissions cannot repair an alignment fault.
    if (signal == SIGBUS && code == BUS_ADRALN) return false;
    return access == Access::Read || access == Access::Write;
}

inline Access ArmAccess(uint64_t esr) noexcept {
    // ESR_ELx: EC [31:26], WnR [6]. See Linux arch/arm64/include/asm/esr.h.
    const auto exception = (esr >> 26) & 0x3f;
    if (exception == 0x20 || exception == 0x21) return Access::Execute;
    if (exception != 0x24 && exception != 0x25) return Access::Unknown;
    // An invalid fault address (FnV) or cache-maintenance operation (CM) is
    // not a recoverable guest load/store, regardless of the reported WnR bit.
    if (esr & ((1u << 10) | (1u << 8))) return Access::Unknown;
    return (esr & (1u << 6)) ? Access::Write : Access::Read;
}

inline Access LinuxArmAccess(const void* reserved, size_t length) noexcept {
    // Linux UAPI asm/sigcontext.h requires ESR records inside __reserved[],
    // never in extra_context. Do not follow extension pointers from a handler.
    const auto* bytes = static_cast<const unsigned char*>(reserved);
    if (!bytes) return Access::Unknown;
    for (size_t offset = 0; length - offset >= 8;) {
        uint32_t magic, size;
        std::memcpy(&magic, bytes + offset, 4);
        std::memcpy(&size, bytes + offset + 4, 4);
        if (magic == 0 || size < 16 || size % 16 != 0 || size > length - offset)
            return Access::Unknown;
        if (magic == 0x45535201) { // ESR_MAGIC
            uint64_t esr;
            std::memcpy(&esr, bytes + offset + 8, 8);
            return ArmAccess(esr);
        }
        offset += size;
    }
    return Access::Unknown;
}

inline Context Decode(const void* raw) noexcept {
    Context result;
    if (!raw) return result;
    const auto* context = static_cast<const ucontext_t*>(raw);
#if defined(__APPLE__)
    if (!context->uc_mcontext) return result;
#if defined(__aarch64__)
    result.instruction = reinterpret_cast<const void*>(arm_thread_state64_get_pc(context->uc_mcontext->__ss));
    result.access = ArmAccess(context->uc_mcontext->__es.__esr);
#elif defined(__x86_64__)
    result.instruction = reinterpret_cast<const void*>(context->uc_mcontext->__ss.__rip);
    const auto error = context->uc_mcontext->__es.__err;
    result.access = (error & 0x10) ? Access::Execute : ((error & 2) ? Access::Write : Access::Read);
#endif
#elif defined(__linux__) && defined(__aarch64__)
    result.instruction = reinterpret_cast<const void*>(context->uc_mcontext.pc);
    result.access = LinuxArmAccess(context->uc_mcontext.__reserved, sizeof(context->uc_mcontext.__reserved));
#elif defined(__linux__) && defined(__x86_64__)
    result.instruction = reinterpret_cast<const void*>(context->uc_mcontext.gregs[REG_RIP]);
    const auto error = context->uc_mcontext.gregs[REG_ERR];
    result.access = (error & 0x10) ? Access::Execute : ((error & 2) ? Access::Write : Access::Read);
#endif
    return result;
}

} // namespace HostFault
