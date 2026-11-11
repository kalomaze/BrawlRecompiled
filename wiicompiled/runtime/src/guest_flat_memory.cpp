#include "guest_flat_memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "memory.h"
#include "ppc_runtime.h"
#include "recomp_mod_loader.h"
#include "runtime_log.h"
#include "system_bridge.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace GuestFlat {
#if !defined(MKW_GUEST_FLAT_FIXED_PAGE_SIZE)
bool g_requiresCheckedAccess = false;
#endif
namespace {

#if defined(_WIN32)
// Placeholder / view constants. Declared here so the build does not depend on
// the exact Windows SDK version that first shipped them.
constexpr DWORD kMemReplacePlaceholder = 0x00004000;
constexpr DWORD kMemReservePlaceholder = 0x00040000;
constexpr DWORD kMemPreservePlaceholder = 0x00000002;
#endif
constexpr size_t kAllocationGranularity = 0x10000;  // 64 KiB
constexpr size_t kHostPageSize = 0x1000;

// Only hosts that can expose a page larger than 4 KiB need to discover their
// size at runtime; see RequiresCheckedAccess() in guest_flat_memory.h.
#if !defined(MKW_GUEST_FLAT_FIXED_PAGE_SIZE)
size_t HostPageSize()
{
    const long size = sysconf(_SC_PAGESIZE);
    return size > 0 ? static_cast<size_t>(size) : kGuestPageSize;
}
#endif

// Named, platform-neutral protection modes so every fault-interception call site below (the
// MMIO window, the executable-write guard, the on-demand
// unmapped-block commit) can stay identical text on both platforms; only ProtectRange() and
// CommitPlaceholder() below branch on VirtualProtect vs. mprotect.
#if defined(_WIN32)
using ProtectionFlags = DWORD;
constexpr ProtectionFlags kProtNone = PAGE_NOACCESS;
constexpr ProtectionFlags kProtRead = PAGE_READONLY;
constexpr ProtectionFlags kProtReadWrite = PAGE_READWRITE;
#else
using ProtectionFlags = int;
constexpr ProtectionFlags kProtNone = PROT_NONE;
constexpr ProtectionFlags kProtRead = PROT_READ;
constexpr ProtectionFlags kProtReadWrite = PROT_READ | PROT_WRITE;
#endif

#if defined(_WIN32)
using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);

VirtualAlloc2Fn g_virtualAlloc2 = nullptr;
MapViewOfFile3Fn g_mapViewOfFile3 = nullptr;
#endif

uint8_t* g_base = nullptr;
bool g_initialized = false;
std::vector<RegionRequest> g_activeRegions;
#if defined(_WIN32)
PVOID g_vectoredHandle = nullptr;
#endif

std::mutex& StateMutex() {
    static std::mutex mutex;
    return mutex;
}

struct SectionKey {
    Backing backing = Backing::Owned;
    uint32_t ownedBase = 0;

    bool operator==(const SectionKey& other) const {
        return backing == other.backing && ownedBase == other.ownedBase;
    }
};

struct SectionKeyHash {
    size_t operator()(const SectionKey& key) const {
        return (static_cast<size_t>(key.ownedBase) << 3) ^ static_cast<size_t>(key.backing);
    }
};

struct Section {
#if defined(_WIN32)
    HANDLE handle = nullptr;
#else
    int fd = -1;
#endif
    uint64_t size = 0;
    uint8_t* hostView = nullptr;
};

std::unordered_map<SectionKey, Section, SectionKeyHash>& Sections() {
    static std::unordered_map<SectionKey, Section, SectionKeyHash> sections;
    return sections;
}

struct MappedRegion {
    uint32_t guestBase = 0;
    uint64_t guestSize = 0;      // requested size (page-table authority)
    uint64_t mappedSize = 0;     // rounded to allocation granularity
    uint64_t sectionOffset = 0;
    uint8_t* hostView = nullptr; // section host view base
};

std::vector<MappedRegion>& MappedRegions() {
    static std::vector<MappedRegion> regions;
    return regions;
}

struct GuardedRange {
    uint32_t start = 0;
    uint32_t end = 0;
};

std::vector<GuardedRange>& ExecutableRanges() {
    static std::vector<GuardedRange> ranges;
    return ranges;
}

// Fault handlers must read these without a mutex or first-use allocation.
static_assert(std::atomic<uint8_t>::is_always_lock_free);
// Registration is published before OS protection and never cleared. Otherwise
// a fault racing registration could mistake executable memory for sparse data.
constexpr uint8_t kExecutableRegistered = 1;
constexpr uint8_t kExecutableProtected = 2;
std::atomic<uint8_t> g_executablePages[1u << 20]{};
#if !defined(_WIN32)
// mprotect is idempotent for simultaneous commits. Only the first successful
// commit increments the distinct-region counter and emits a diagnostic.
std::atomic<uint8_t> g_unmappedCommittedBlocks[1u << 16]{};
#endif

std::atomic<uint32_t> g_countMmio{0};
std::atomic<uint32_t> g_countXGuard{0};
std::atomic<uint32_t> g_countUnmapped{0};
std::atomic<uint32_t> g_countUnmappedRegions{0};

uint64_t RoundUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

std::string LastErrorText(const char* what) {
    std::ostringstream oss;
#if defined(_WIN32)
    oss << what << " failed (GetLastError=" << GetLastError() << ")";
#else
    oss << what << " failed (" << std::strerror(errno) << ")";
#endif
    return oss.str();
}

// Protects [address, address+size) with `protection`, bridging VirtualProtect (Windows) and
// mprotect (POSIX) so every fault-interception call site below can stay platform-neutral.
bool ProtectRange(uint8_t* address, uint64_t size, ProtectionFlags protection) {
#if defined(_WIN32)
    DWORD previous = 0;
    return VirtualProtect(address, static_cast<SIZE_T>(size), protection, &previous) != FALSE;
#else
    return mprotect(address, static_cast<size_t>(size), protection) == 0;
#endif
}

#if defined(_WIN32)
void ResolvePlacementApi() {
    if (g_virtualAlloc2 != nullptr && g_mapViewOfFile3 != nullptr) return;
    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    if (kernelBase == nullptr) kernelBase = LoadLibraryW(L"kernelbase.dll");
    if (kernelBase != nullptr) {
        g_virtualAlloc2 =
            reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kernelBase, "VirtualAlloc2"));
        g_mapViewOfFile3 =
            reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kernelBase, "MapViewOfFile3"));
    }
    if (g_virtualAlloc2 == nullptr || g_mapViewOfFile3 == nullptr) {
        throw std::runtime_error(
            "Flat guest memory requires Windows 10 1803 or newer (VirtualAlloc2/MapViewOfFile3 "
            "are unavailable on this system).");
    }
}
#endif

void EnsureReservation() {
    if (g_base != nullptr) return;
#if defined(_WIN32)
    ResolvePlacementApi();

    void* requested = reinterpret_cast<void*>(kFixedFlatGuestBase);

    // One extra granule stays an uncommitted placeholder so an access that
    // straddles 0xFFFFFFFF faults instead of corrupting whatever the allocator
    // happened to place directly after the reservation.
    void* reserved = g_virtualAlloc2(GetCurrentProcess(), requested,
                                     static_cast<SIZE_T>(kGuestSpaceSize + kAllocationGranularity),
                                     MEM_RESERVE | kMemReservePlaceholder, PAGE_NOACCESS,
                                     nullptr, 0);
    if (reserved == nullptr) {
        std::ostringstream oss;
        oss << "Unable to reserve the 4 GiB flat guest address space at 0x" << std::hex
            << reinterpret_cast<uintptr_t>(requested) << std::dec
            << " (GetLastError=" << GetLastError()
            << "). The translated code addresses guest memory through this fixed base, so it "
               "cannot fall back to another one. Something else in this process reserved the "
               "16 TiB region first - an injected DLL, an overlay or a debugging tool is the "
               "usual cause.";
        throw std::runtime_error(oss.str());
    }
    if (reserved != requested) {
        throw std::runtime_error(
            "The flat guest reservation did not land on the fixed base the translated code was "
            "compiled against.");
    }
#else
    void* requested = reinterpret_cast<void*>(kFixedFlatGuestBase);

    // No MAP_FIXED here (and deliberately no MAP_FIXED_NOREPLACE, which needs Linux 4.17+ -
    // this must work on kernels as old as 4.9): `requested` is only a hint. The kernel's
    // get_unmapped_area honors a page-aligned hint when the whole range is free, so this lands
    // on the fixed base in the normal case; if anything already occupies part of the range, the
    // kernel silently picks a different address instead of clobbering it, which the check below
    // catches - same "something got there first" contract as the Windows path, without needing
    // a specific kernel version.
    void* reserved = mmap(requested, kGuestSpaceSize + kAllocationGranularity, kProtNone,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reserved == MAP_FAILED) {
        std::ostringstream oss;
        oss << "Unable to reserve the 4 GiB flat guest address space at 0x" << std::hex
            << reinterpret_cast<uintptr_t>(requested) << std::dec
            << " (" << std::strerror(errno)
            << "). The translated code addresses guest memory through this fixed base, so it "
               "cannot fall back to another one.";
        throw std::runtime_error(oss.str());
    }
    if (reserved != requested) {
        munmap(reserved, kGuestSpaceSize + kAllocationGranularity);
        std::ostringstream oss;
        oss << "Unable to reserve the 4 GiB flat guest address space at 0x" << std::hex
            << reinterpret_cast<uintptr_t>(requested) << std::dec
            << ". Either something else in this process already occupies that address (an "
               "injected library, an overlay or a debugging tool is the usual cause), or this "
               "kernel's virtual address space does not reach that high (common on some 32-bit-"
               "userspace-compatible or older AArch64 configurations, e.g. a kernel built for "
               "39-bit virtual addresses) - in the latter case mmap() silently substitutes an "
               "address near the top of the space it does have instead of honoring the request.";
        throw std::runtime_error(oss.str());
    }
#endif

    g_base = static_cast<uint8_t*>(reserved);
}

#if defined(_WIN32)
// Carves `size` bytes out of the enclosing placeholder so a view or a private
// commit can replace it. Splitting an exact-size placeholder is a no-op that
// reports ERROR_INVALID_PARAMETER; the caller validates the replacement.
void SplitPlaceholder(uint8_t* address, uint64_t size) {
    VirtualFree(address, static_cast<SIZE_T>(size), MEM_RELEASE | kMemPreservePlaceholder);
}
#endif

void MapGuestView(const Section& section, uint64_t sectionOffset, uint32_t guestBase,
                  uint64_t mappedSize) {
    uint8_t* target = g_base + guestBase;
#if defined(_WIN32)
    SplitPlaceholder(target, mappedSize);
    void* view = g_mapViewOfFile3(section.handle, GetCurrentProcess(), target, sectionOffset,
                                  static_cast<SIZE_T>(mappedSize), kMemReplacePlaceholder,
                                  PAGE_READWRITE, nullptr, 0);
    if (view == nullptr) {
        std::ostringstream oss;
        oss << "Unable to map guest region 0x" << std::hex << guestBase << " (+0x" << mappedSize
            << ") into the flat reservation" << std::dec << " (GetLastError=" << GetLastError()
            << ")";
        throw std::runtime_error(oss.str());
    }
#else
    // MAP_FIXED is safe (and needs no particular kernel version) here specifically because we're
    // deliberately overwriting a sub-range of the PROT_NONE reservation this module already owns
    // exclusively (see EnsureReservation) - unlike the initial reservation itself, there's no
    // "something else might already be there" concern to guard against.
    void* view = mmap(target, static_cast<size_t>(mappedSize), kProtReadWrite,
                      MAP_SHARED | MAP_FIXED, section.fd, static_cast<off_t>(sectionOffset));
    if (view == MAP_FAILED) {
        std::ostringstream oss;
        oss << "Unable to map guest region 0x" << std::hex << guestBase << " (+0x" << mappedSize
            << ") into the flat reservation" << std::dec << " (" << std::strerror(errno) << ")";
        throw std::runtime_error(oss.str());
    }
#endif
}

// Replaces a placeholder with private committed memory. Used for the MMIO
// window (read-only zeros) and for on-demand commits of stray guest pages.
bool CommitPlaceholder(uint8_t* address, uint64_t size, ProtectionFlags protection) {
#if defined(_WIN32)
    SplitPlaceholder(address, size);
    void* result = g_virtualAlloc2(GetCurrentProcess(), address, static_cast<SIZE_T>(size),
                                   MEM_RESERVE | MEM_COMMIT | kMemReplacePlaceholder, protection,
                                   nullptr, 0);
    return result != nullptr;
#else
    // No separate reserve-vs-commit step is needed: the anonymous PROT_NONE reservation this
    // range came from is already demand-zero backed, so mprotect() alone both "commits" and
    // protects it.
    return ProtectRange(address, size, protection);
#endif
}

// One definition of the two windows lives in memory_access.h; these are the
// names the fault handler below reads.
bool IsMmio(uint32_t address) { return MemoryInline::IsMmioAddress(address); }
bool IsGpuFifo(uint32_t address) { return MemoryInline::IsGpuFifoAddress(address); }

void ApplyExecutableProtectionLocked() {
    if (g_base == nullptr || RequiresCheckedAccess()) return;
    auto& protectedPages = g_executablePages;
    for (const auto& range : ExecutableRanges()) {
        // Edge pages too (code and data): GuestCodePatch keeps one open after a data store until the next frame, so
        // its data costs a fault a frame at most, and a mod's hook written there by a plain store is still seen.
        const uint64_t first = static_cast<uint64_t>(range.start) & ~(kHostPageSize - 1u);
        const uint64_t last = RoundUp(range.end, kHostPageSize);
        if (last <= first) continue;
        for (uint64_t page = first; page < last; page += kHostPageSize) {
            const uint32_t pageIndex = static_cast<uint32_t>(page >> 12);
            const auto flags = protectedPages[pageIndex].fetch_or(kExecutableRegistered, std::memory_order_release);
            if (flags & kExecutableProtected) continue;
            if (ProtectRange(g_base + page, kHostPageSize, kProtRead)) {
                protectedPages[pageIndex].fetch_or(kExecutableProtected, std::memory_order_relaxed);
            }
        }
    }
}

bool SameLayout(const std::vector<RegionRequest>& lhs, const std::vector<RegionRequest>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t index = 0; index < lhs.size(); ++index) {
        if (lhs[index].base != rhs[index].base || lhs[index].size != rhs[index].size ||
            lhs[index].backing != rhs[index].backing) {
            return false;
        }
    }
    return true;
}

uint64_t SectionOffsetFor(const RegionRequest& region) {
    switch (region.backing) {
    case Backing::Mem1:
        return region.base & 0x01FFFFFFu;  // 32 MiB MEM1 window
    case Backing::Mem2:
        return region.base & 0x0FFFFFFFu;  // 256 MiB MEM2 window
    case Backing::Owned:
    default:
        return 0;
    }
}

SectionKey KeyFor(const RegionRequest& region) {
    SectionKey key;
    key.backing = region.backing;
    key.ownedBase = region.backing == Backing::Owned ? region.base : 0;
    return key;
}

void ZeroMappedStorage() {
    for (auto& [key, section] : Sections()) {
        (void)key;
        if (section.hostView != nullptr && section.size != 0) {
            std::memset(section.hostView, 0, static_cast<size_t>(section.size));
        }
    }
}

#if defined(_WIN32)
// BRAWL_WATCH_WRITE=<hex guest address>[:<hex value>]: a debugging aid for mods. Each write to that word (that leaves
// it holding <value>, when one is given) is logged with the guest state: its page stays read-only, and each write to
// the page is single-stepped, checked, and the page protected again.
uint32_t g_watchAddress = 0;  // start of the watched pages
uint32_t g_watchWord = 0;     // the word itself
bool g_watchHasValue = false;
uint32_t g_watchValue = 0;
bool g_watchAccess = false;  // BRAWL_WATCH_ACCESS=<start>-<end>: every access to those pages, the first ones logged
uint32_t g_watchEnd = 0;
thread_local uint8_t* t_watchPage = nullptr;
thread_local bool t_watchRearm = false;
thread_local bool t_watchHit = false;
thread_local uint32_t t_watchBefore = 0;

uint32_t WatchedWord() {
    uint32_t value = 0;
    std::memcpy(&value, g_base + g_watchWord, 4);
    return __builtin_bswap32(value);
}

void ArmWriteWatch() {
    const char* text = std::getenv("BRAWL_WATCH_WRITE");
    if (text == nullptr) {
        text = std::getenv("BRAWL_WATCH_ACCESS");
        g_watchAccess = text != nullptr;
    }
    if (text == nullptr) return;
    char* end = nullptr;
    g_watchAddress = static_cast<uint32_t>(std::strtoul(text, &end, 16)) & ~3u;
    if (g_watchAddress == 0) return;
    g_watchWord = g_watchAddress;
    if (*end == ':') {
        g_watchHasValue = true;
        g_watchValue = static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 16));
    }
    if (g_watchAccess && *end == '-') {
        g_watchEnd = static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 16));
    }
    if (g_watchEnd <= g_watchAddress) g_watchEnd = g_watchAddress + 4;
    g_watchAddress &= ~(kHostPageSize - 1);
    g_watchEnd = (g_watchEnd + kHostPageSize - 1) & ~(kHostPageSize - 1);
    ProtectRange(g_base + g_watchAddress, g_watchEnd - g_watchAddress, g_watchAccess ? kProtNone : kProtRead);
    RT_LOGF(RT_TAG_MEMORY, "watching %s 0x%08X\n", g_watchAccess ? "every access to the page of" : "writes to",
            g_watchAddress);
}

bool HandleWriteWatch(EXCEPTION_POINTERS* info) {
    const auto* record = info->ExceptionRecord;
    uint8_t* page = g_base + (g_watchAddress & ~(kHostPageSize - 1));
    if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && t_watchRearm) {
        if (g_watchAccess) page = t_watchPage;
        t_watchRearm = false;
        static int logged = 0;
        if (t_watchHit && (!g_watchHasValue || WatchedWord() == g_watchValue) && logged++ < 40) {
            RT_LOGF(RT_TAG_MEMORY, "[watch] 0x%08X: 0x%08X -> 0x%08X, guest state:\n", g_watchWord, t_watchBefore,
                    WatchedWord());
            SystemBridge::DumpCpuState(TryGetCpuContext());
        }
        t_watchHit = false;
        ProtectRange(page, kHostPageSize, g_watchAccess ? kProtNone : kProtRead);
        return true;
    }
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 ||
        record->ExceptionInformation[0] > 1 ||
        (record->ExceptionInformation[0] == 0 && !g_watchAccess)) {
        return false;
    }
    const uintptr_t fault = static_cast<uintptr_t>(record->ExceptionInformation[1]);
    if (g_watchAccess) {
        if (fault < reinterpret_cast<uintptr_t>(g_base + g_watchAddress) || fault >= reinterpret_cast<uintptr_t>(g_base + g_watchEnd)) {
            return false;
        }
        page = reinterpret_cast<uint8_t*>(fault & ~static_cast<uintptr_t>(kHostPageSize - 1));
        t_watchPage = page;
    } else if (fault < reinterpret_cast<uintptr_t>(page) || fault >= reinterpret_cast<uintptr_t>(page) + kHostPageSize) {
        return false;
    }
    const uint32_t guestAddress = static_cast<uint32_t>(fault - reinterpret_cast<uintptr_t>(g_base));
    t_watchHit = guestAddress + 8 > g_watchWord && guestAddress < g_watchWord + 4;
    if (g_watchAccess) {
        static int logged = 0;
        if (logged == 30) {  // seen enough: let the game run at full speed again
            ProtectRange(g_base + g_watchAddress, g_watchEnd - g_watchAddress, kProtReadWrite);
            g_watchAccess = false;
            g_watchAddress = 0;
            return true;
        }
        if (logged++ < 30) {
            RT_LOGF(RT_TAG_MEMORY, "[watch] %s 0x%08X, guest state:\n",
                    record->ExceptionInformation[0] ? "write" : "read", guestAddress);
            SystemBridge::DumpCpuState(TryGetCpuContext());
        }
        t_watchHit = false;
    }
    if (t_watchHit) t_watchBefore = WatchedWord();
    ProtectRange(page, kHostPageSize, kProtReadWrite);
    t_watchRearm = true;
    info->ContextRecord->EFlags |= 0x100;  // trap after this one instruction
    return true;
}

LONG CALLBACK FlatGuestVectoredHandler(EXCEPTION_POINTERS* info) {
    const auto* record = info->ExceptionRecord;
    if (g_watchAddress != 0 && record != nullptr && HandleWriteWatch(info)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record == nullptr || record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        record->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    void* faultAddress = reinterpret_cast<void*>(record->ExceptionInformation[1]);
    const bool isWrite = record->ExceptionInformation[0] == 1;
    if (HandleAccessViolation(faultAddress, isWrite, record->ExceptionAddress)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallVectoredHandler() {
    if (g_vectoredHandle != nullptr) return;
    g_vectoredHandle = AddVectoredExceptionHandler(1, FlatGuestVectoredHandler);
    if (g_vectoredHandle == nullptr) {
        throw std::runtime_error(LastErrorText("AddVectoredExceptionHandler"));
    }
}
#endif

void ReportFatalGuestFault(const char* category, uint32_t guestAddress, bool isWrite,
                           const char* detail) {
    RT_LOG(RT_TAG_MEMORY) << "FATAL " << category << std::endl;
    std::cerr << "  guest address: 0x" << std::hex << std::uppercase << std::setw(8)
              << std::setfill('0') << guestAddress << std::dec << std::setfill(' ') << std::endl;
    std::cerr << "  access:        " << (isWrite ? "write" : "read") << std::endl;
    // The fault record carries the faulting address but not the width of the
    // access, so the size is genuinely unavailable on this path. The checked
    // path (runtime/src/memory.cpp) reports the exact length instead.
    std::cerr << "  access size:   unknown (not recoverable from the fault record)" << std::endl;
    std::cerr << "  active func:   0x" << std::hex << std::uppercase
              << RecompMod::CurrentTranslatedExecutionAddress() << std::dec << std::nouppercase
              << std::endl;
    std::cerr << "  detail:        " << detail << std::endl;
    if (auto* cpu = TryGetCpuContext()) {
        std::cerr << "  guest pc:      0x" << std::hex << cpu->pc << " lr=0x" << cpu->lr << std::dec
                  << std::endl;
        SystemBridge::DumpCpuState(cpu);
    }
    std::cerr.flush();
    std::ostringstream message;
    message << "The game stopped because translated code performed a forbidden guest memory "
               "access at 0x"
            << std::hex << std::uppercase << guestAddress << ".\n\n"
            << detail;
    ShowRuntimeFatalPopup(category, message.str());
    std::abort();
}

// Recovery can interrupt the allocator or an iostream operation. Format into
// stack storage and write directly, without locks, allocation or guest reads.
void ReportRecoveredFault(const char* reason, uint32_t address, bool isWrite) {
    char message[192];
    size_t length = 0;
    auto append = [&](const char* text) {
        while (*text && length < sizeof(message) - 9) message[length++] = *text++;
    };
    append("[memory] WARNING ");
    append(reason);
    append(isWrite ? " write at 0x" : " read at 0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        message[length++] = "0123456789ABCDEF"[(address >> shift) & 15];
    }
    message[length++] = '\n';
#if defined(_WIN32)
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, static_cast<DWORD>(length), &written, nullptr);
#else
    const int savedErrno = errno;
    const auto ignored = write(STDERR_FILENO, message, length);
    (void)ignored;
    errno = savedErrno;
#endif
}

} // namespace

bool IsActive() {
    return g_initialized;
}

void Initialize(const std::vector<RegionRequest>& regions) {
    std::lock_guard<std::mutex> lock(StateMutex());

#if !defined(MKW_GUEST_FLAT_FIXED_PAGE_SIZE)
    g_requiresCheckedAccess = HostPageSize() > kGuestPageSize;
#endif

    if (g_initialized) {
        if (!SameLayout(g_activeRegions, regions)) {
            throw std::runtime_error(
                "The flat guest address space is mapped once per process; a second Memory::Init "
                "requested a different region layout. Restart the process instead of remapping.");
        }
        ZeroMappedStorage();
        ApplyExecutableProtectionLocked();
        return;
    }

    EnsureReservation();

    // Size every section from the highest byte any of its regions reaches.
    std::unordered_map<SectionKey, uint64_t, SectionKeyHash> sizes;
    for (const auto& region : regions) {
        if (region.size == 0) continue;
        if ((region.base % kAllocationGranularity) != 0) {
            std::ostringstream oss;
            oss << "Guest region base 0x" << std::hex << region.base
                << " is not 64 KiB aligned; the flat mapping cannot place it.";
            throw std::runtime_error(oss.str());
        }
        const uint64_t end = SectionOffsetFor(region) + region.size;
        auto& current = sizes[KeyFor(region)];
        current = std::max(current, end);
    }

    for (auto& [key, size] : sizes) {
        const uint64_t rounded = RoundUp(size, kAllocationGranularity);
        Section section;
        section.size = rounded;
#if defined(_WIN32)
        section.handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                            static_cast<DWORD>(rounded >> 32),
                                            static_cast<DWORD>(rounded & 0xFFFFFFFFu), nullptr);
        if (section.handle == nullptr) {
            throw std::runtime_error(LastErrorText("CreateFileMapping for guest RAM"));
        }
        section.hostView = static_cast<uint8_t*>(
            MapViewOfFile(section.handle, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(rounded)));
        if (section.hostView == nullptr) {
            throw std::runtime_error(LastErrorText("MapViewOfFile for the host guest-RAM alias"));
        }
#else
        // The section is an anonymous shared-memory object: the SAME physical pages get mapped
        // twice below (once here as the always-accessible host view, once per-region as the
        // guest view whose protection the fault handler controls), the same "one backing store,
        // two VA aliases" trick CreateFileMapping/MapViewOfFile(3) gives Windows.
#if defined(__linux__)
        section.fd = memfd_create("wiicompiled-guest-ram", MFD_CLOEXEC);
#else
        // POSIX hosts without memfd_create use an unlinked backing file.
        char path[] = "/tmp/wiicompiled-guest-XXXXXX";
        section.fd = mkstemp(path);
        if (section.fd >= 0) {
            if (unlink(path) != 0 || fcntl(section.fd, F_SETFD, FD_CLOEXEC) != 0) {
                const int error = errno;
                close(section.fd);
                errno = error;
                throw std::runtime_error(LastErrorText("preparing guest RAM backing file"));
            }
        }
#endif
        if (section.fd < 0) {
            throw std::runtime_error(LastErrorText("creating guest RAM backing store"));
        }
        if (ftruncate(section.fd, static_cast<off_t>(rounded)) != 0) {
            throw std::runtime_error(LastErrorText("ftruncate for guest RAM"));
        }
        section.hostView = static_cast<uint8_t*>(
            mmap(nullptr, static_cast<size_t>(rounded), kProtReadWrite, MAP_SHARED, section.fd, 0));
        if (section.hostView == MAP_FAILED) {
            section.hostView = nullptr;
            throw std::runtime_error(LastErrorText("mmap for the host guest-RAM alias"));
        }
#endif
        Sections()[key] = section;
    }

    for (const auto& region : regions) {
        if (region.size == 0) continue;
        const auto& section = Sections()[KeyFor(region)];
        const uint64_t offset = SectionOffsetFor(region);
        const uint64_t mappedSize =
            std::min<uint64_t>(RoundUp(region.size, kAllocationGranularity), section.size - offset);
        MapGuestView(section, offset, region.base, mappedSize);
        MappedRegions().push_back(
            MappedRegion{region.base, region.size, mappedSize, offset, section.hostView});
    }

    // MMIO stays inaccessible in both directions so the vectored handler can report missing HLE; the old
    // PAGE_READONLY read window that returned zero turned missing devices into silent hangs instead.
    if (!CommitPlaceholder(g_base + 0xCC000000u, 0x02000000u, kProtNone)) {
        throw std::runtime_error(LastErrorText("committing the no-access MMIO window"));
    }

    ApplyExecutableProtectionLocked();
#if defined(_WIN32)
    InstallVectoredHandler();
    ArmWriteWatch();
#endif

    // Freshly created section objects are demand-zero, so no explicit clear is
    // needed on the first mapping (that would fault in all 152 MiB at startup).
    g_activeRegions = regions;
    g_initialized = true;

    RT_LOG(RT_TAG_MEMORY) << "guest address space reserved at 0x" << std::hex
              << reinterpret_cast<uintptr_t>(g_base) << std::dec << " (" << MappedRegions().size()
              << " regions, " << Sections().size() << " backing stores)" << std::endl;
}

uint8_t* HostPointer(uint32_t guestAddress) {
    if (!g_initialized) return nullptr;
    for (const auto& region : MappedRegions()) {
        if (guestAddress < region.guestBase) continue;
        const uint64_t offset = static_cast<uint64_t>(guestAddress) - region.guestBase;
        if (offset >= region.guestSize) continue;
        return region.hostView + region.sectionOffset + offset;
    }
    return nullptr;
}

void RegisterExecutableRange(uint32_t start, uint32_t end) {
    if (RequiresCheckedAccess() || end <= start) return;
    std::lock_guard<std::mutex> lock(StateMutex());
    auto& ranges = ExecutableRanges();
    if (std::any_of(ranges.begin(), ranges.end(), [&](const GuardedRange& range) {
            return range.start == start && range.end == end;
        })) {
        return;
    }
    ranges.push_back(GuardedRange{start, end});
    ApplyExecutableProtectionLocked();
}

void ReprotectExecutablePage(uint32_t address) {
    if (RequiresCheckedAccess() || g_base == nullptr) return;
    std::lock_guard<std::mutex> lock(StateMutex());
    const uint64_t page = static_cast<uint64_t>(address) & ~(kHostPageSize - 1u);
    const bool inside = std::any_of(ExecutableRanges().begin(), ExecutableRanges().end(), [&](const GuardedRange& range) {
        return page < range.end && page + kHostPageSize > range.start;
    });
    const uint32_t pageIndex = static_cast<uint32_t>(page >> 12);
    if (inside && (g_executablePages[pageIndex].load(std::memory_order_relaxed) & kExecutableProtected) == 0 && ProtectRange(g_base + page, kHostPageSize, kProtRead)) {
        g_executablePages[pageIndex].fetch_or(kExecutableProtected, std::memory_order_relaxed);
    }
}

FaultCounters Counters() {
    FaultCounters counters;
    counters.mmio = g_countMmio.load(std::memory_order_relaxed);
    counters.xguard = g_countXGuard.load(std::memory_order_relaxed);
    counters.unmapped = g_countUnmapped.load(std::memory_order_relaxed);
    counters.unmappedRegions = g_countUnmappedRegions.load(std::memory_order_relaxed);
    return counters;
}

void LogFaultSummary() noexcept {
    static std::atomic<bool> reported{false};
    if (reported.exchange(true, std::memory_order_relaxed)) return;
    const FaultCounters counters = Counters();
    if (counters.unmapped == 0) {
        RT_LOG(RT_TAG_MEMORY) << "shutdown summary: no unmapped guest touches (xguard="
                  << counters.xguard << " mmio=" << counters.mmio
                  << ")" << std::endl;
        std::cerr.flush();
        return;
    }
    RT_LOG(RT_TAG_MEMORY) << "WARNING shutdown summary: " << counters.unmapped
              << " unmapped guest touches across " << counters.unmappedRegions
              << " distinct 64 KiB regions were absorbed by on-demand commits. Each one is a "
                 "guest pointer that addressed nothing; search the log for "
                 "'[" RT_TAG_MEMORY "] WARNING unmapped guest touch' for the faulting addresses."
              << std::endl;
    RT_LOG(RT_TAG_MEMORY) << "shutdown summary: xguard=" << counters.xguard << " mmio=" << counters.mmio << std::endl;
    std::cerr.flush();
}

bool HandleAccessViolation(void* faultAddress, bool isWrite, const void* instructionAddress) noexcept {
    if (!g_initialized || faultAddress == nullptr || instructionAddress == nullptr) return false;

    const uintptr_t fault = reinterpret_cast<uintptr_t>(faultAddress);
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_base);
    if (fault < base || fault - base >= kGuestSpaceSize) return false;
    // PPC instructions are translated/interpreted, never executed in this mapping.
    // Opening data permissions cannot repair a native instruction-fetch fault.
    const uintptr_t pc = reinterpret_cast<uintptr_t>(instructionAddress);
    if (pc >= base && pc - base < kGuestSpaceSize) return false;

    const uint32_t guestAddress = static_cast<uint32_t>(fault - base);

    // 1) Executable-write guard. Registration remains visible while a page is
    // being protected or reopened, so delayed faults cannot enter sparse recovery.
    {
        const uint32_t pageIndex = guestAddress >> 12;
        const bool guarded = (g_executablePages[pageIndex].load(std::memory_order_acquire) & kExecutableRegistered) != 0;
        if (guarded) {
            g_countXGuard.fetch_add(1, std::memory_order_relaxed);
            // Aborts inside CheckExecutableWrite for an unsupported patch; the
            // width and value are not recoverable from the fault record, so the
            // report carries the exact address instead.
            if (!RecompMod::HandleExecutableWrite(guestAddress, 1, 0)) {
                // A permitted write (the REL loader relocating its own text).
                // Those arrive in bulk, so the page is opened permanently
                // rather than trapping every relocation.
                std::lock_guard<std::mutex> lock(StateMutex());
                if (!ProtectRange(g_base + (static_cast<uint64_t>(pageIndex) << 12), kHostPageSize,
                                  kProtReadWrite)) {
                    return false;  // Retrying an unchanged protection fault would loop forever.
                }
                g_executablePages[pageIndex].fetch_and(static_cast<uint8_t>(~kExecutableProtected), std::memory_order_relaxed);
            }
            return true;
        }
    }

    // 2) MMIO. PAGE_NOACCESS makes both directions faults: a write has no backing device (including a GPU
    //    FIFO store the translator failed to lower), and a read would have to invent a register value;
    //    answering zero would turn a missing device into a silent hang, so both are reported instead.
    if (IsMmio(guestAddress)) {
        g_countMmio.fetch_add(1, std::memory_order_relaxed);
        // Outside the device register blocks (Flipper at 0xCC000000/0xCD000000, Hollywood at 0xCD800000) the bus has
        // nothing; mods read through stale pointers there (Project+: an 0xCC-filled pointer plus an offset). Like
        // the console and Dolphin, such reads give 0: the page becomes zeroed scratch memory.
        const uint32_t block = guestAddress & 0xFFFF0000u;
        if (block != 0xCC000000u && block != 0xCD000000u && block != 0xCD800000u) {
            if (ProtectRange(g_base + (guestAddress & ~(kHostPageSize - 1)), kHostPageSize, kProtReadWrite)) {
                ReportRecoveredFault("no hardware register; opened scratch page for", guestAddress, isWrite);
                return true;
            }
        }
        if (IsGpuFifo(guestAddress)) {
            if (isWrite) {
                ReportFatalGuestFault(
                    "GPU FIFO write reached the flat memory path", guestAddress, isWrite,
                    "Gather-pipe stores must be lowered to GX_HLE_FIFO_Write*; the written value "
                    "cannot be recovered from a fault. Fix the translator lowering for this "
                    "store.");
            }
            ReportFatalGuestFault(
                "GPU FIFO read blocked", guestAddress, isWrite,
                "The gather pipe is write-only; nothing can be read back from it. The guest code "
                "that issued this load needs GX HLE, not a memory access.");
        }
        if (isWrite) {
            ReportFatalGuestFault("MMIO write blocked (non-GPU)", guestAddress, isWrite,
                                  "Hardware registers have no backing store. Add HLE for this "
                                  "device instead of letting the write land.");
        }
        ReportFatalGuestFault("MMIO read blocked (non-GPU)", guestAddress, isWrite,
                              "Hardware registers have no backing store. Answering zero would "
                              "hang the caller in a status poll instead of reporting the gap; "
                              "add HLE for this device.");
        return true;
    }

    // 3) Unmapped address: commit the block on demand (reproducing the old zero-fill sparse-map behavior)
    //    and report it, since a commit here means a wild guest pointer that would otherwise walk over silently.
    const uint64_t blockBase = static_cast<uint64_t>(guestAddress) & ~(kAllocationGranularity - 1u);
    g_countUnmapped.fetch_add(1, std::memory_order_relaxed);
    bool committed = false;
    {
#if defined(_WIN32)
        std::lock_guard<std::mutex> lock(StateMutex());
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(g_base + blockBase, &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State == MEM_COMMIT) {
            // Another thread already committed this block: re-running the
            // access succeeds. Any other committed-but-inaccessible state is
            // not ours to fix - resuming would fault forever, so hand the
            // exception to the crash reporter instead.
            const bool writable = (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                                                  PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
            const bool readable = writable || (mbi.Protect & (PAGE_READONLY | PAGE_EXECUTE_READ |
                                                              PAGE_EXECUTE)) != 0;
            return isWrite ? writable : readable;
        }
        if (!CommitPlaceholder(g_base + blockBase, kAllocationGranularity, kProtReadWrite)) return false;
        committed = true;
#else
        // Re-establish access even after a previous commit: protection or the
        // mapping itself may have changed. Failed OS operations must propagate.
        if (!CommitPlaceholder(g_base + blockBase, kAllocationGranularity, kProtReadWrite)) return false;
        const uint32_t blockIndex = static_cast<uint32_t>(blockBase / kAllocationGranularity);
        committed = g_unmappedCommittedBlocks[blockIndex].exchange(1, std::memory_order_relaxed) == 0;
#endif
    }
    if (committed) {
        g_countUnmappedRegions.fetch_add(1, std::memory_order_relaxed);
        ReportRecoveredFault("unmapped guest touch; committed zero-filled block for", guestAddress, isWrite);
    }
    return true;
}

} // namespace GuestFlat
