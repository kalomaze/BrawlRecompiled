#include "memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <optional>
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "system_bridge.h"
#include <mutex>
#include <sstream>
#include <unordered_map>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

MemoryInline::PageEntry MemoryInline::g_pageTable[MemoryInline::kPageCount]{};
uintptr_t MemoryInline::g_fullPageBias[MemoryInline::kPageCount]{};
uintptr_t MemoryInline::g_fullWritablePageBias[MemoryInline::kPageCount]{};
const MemoryInline::SparseWritablePageTable*
    MemoryInline::g_sparseWritablePageTables[MemoryInline::kPageCount]{};
template <typename T>
T MemoryInline::ReadResolvedFallback(uint32_t addr) {
    if constexpr (sizeof(T) == 1) return Memory::Read8(addr);
    if constexpr (sizeof(T) == 2) return Memory::Read16(addr);
    if constexpr (sizeof(T) == 4) return Memory::Read32(addr);
    return Memory::Read64(addr);
}
template uint8_t MemoryInline::ReadResolvedFallback<uint8_t>(uint32_t);
template uint16_t MemoryInline::ReadResolvedFallback<uint16_t>(uint32_t);
template uint32_t MemoryInline::ReadResolvedFallback<uint32_t>(uint32_t);
template uint64_t MemoryInline::ReadResolvedFallback<uint64_t>(uint32_t);
float MemoryInline::ReadResolvedFallbackFloat32(uint32_t addr) { return Memory::ReadFloat32(addr); }
double MemoryInline::ReadResolvedFallbackFloat64(uint32_t addr) { return Memory::ReadFloat64(addr); }
template <typename T>
void MemoryInline::WriteResolvedFallback(uint32_t addr, T value) {
    if constexpr (sizeof(T) == 1) Memory::Write8(addr, value);
    else if constexpr (sizeof(T) == 2) Memory::Write16(addr, value);
    else if constexpr (sizeof(T) == 4) Memory::Write32(addr, value);
    else Memory::Write64(addr, value);
}
template void MemoryInline::WriteResolvedFallback<uint8_t>(uint32_t, uint8_t);
template void MemoryInline::WriteResolvedFallback<uint16_t>(uint32_t, uint16_t);
template void MemoryInline::WriteResolvedFallback<uint32_t>(uint32_t, uint32_t);
template void MemoryInline::WriteResolvedFallback<uint64_t>(uint32_t, uint64_t);
void MemoryInline::WriteResolvedFallbackFloat32(uint32_t addr, double val) { Memory::WriteFloat32(addr, val); }
void MemoryInline::WriteResolvedFallbackFloat64(uint32_t addr, double val) { Memory::WriteFloat64(addr, val); }
namespace {
struct Region {
    Memory::RegionConfig config;
    std::shared_ptr<std::vector<uint8_t>> storage;
    uint8_t* storagePtr = nullptr;
    size_t storageSize = 0;
};

std::vector<Region>& Regions() {
    static std::vector<Region> regions;
    return regions;
}

std::mutex& RegionMutex() {
    static std::mutex mutex;
    return mutex;
}

std::array<std::unique_ptr<MemoryInline::SparseWritablePageTable>,
           MemoryInline::kPageCount>& SparseWritablePageTableStorage() {
    static std::array<std::unique_ptr<MemoryInline::SparseWritablePageTable>,
                      MemoryInline::kPageCount> storage;
    return storage;
}

void ClearPageTable() {
    auto* table = MemoryInline::g_pageTable;
    for (uint32_t i = 0; i < MemoryInline::kPageCount; ++i) {
        auto& entry = table[i];
        entry.base = nullptr;
        entry.limit = 0;
        MemoryInline::g_fullPageBias[i] = 0;
        MemoryInline::g_fullWritablePageBias[i] = 0;
        MemoryInline::g_sparseWritablePageTables[i] = nullptr;
        SparseWritablePageTableStorage()[i].reset();
    }
}

} // namespace

namespace {

void BuildPageTable() {
    auto* table = MemoryInline::g_pageTable;
    for (const auto& region : Regions()) {
        const uint64_t base = region.config.baseAddress;
        const uint64_t size = region.storageSize;
        const uint64_t end = base + size;
        if (size == 0) {
            continue;
        }

        const uint32_t startPage = static_cast<uint32_t>(base >> MemoryInline::kPageShift);
        const uint32_t endPage = static_cast<uint32_t>((end - 1) >> MemoryInline::kPageShift);
        for (uint32_t page = startPage; page <= endPage; ++page) {
            const uint64_t pageBase = static_cast<uint64_t>(page) << MemoryInline::kPageShift;
            const uint64_t offset = pageBase - base;
            if (offset >= size) {
                continue;
            }
            const uint32_t limit = static_cast<uint32_t>(std::min<uint64_t>(MemoryInline::kPageSize, size - offset));
            table[page].base = region.storagePtr + static_cast<size_t>(offset);
            table[page].limit = limit;
        }
    }

    // The last page of each contiguous mapping remains on the checked path so
    // an access straddling its end cannot escape the mapped region. Every
    // preceding full page can safely service native accesses up to 8 bytes,
    // including a cross-page access into its contiguous successor.
    for (uint32_t page = 0; page + 1 < MemoryInline::kPageCount; ++page) {
        const auto& current = table[page];
        const auto& next = table[page + 1];
        if (!current.base || current.limit != MemoryInline::kPageSize ||
            !next.base || next.limit < MemoryInline::kMaxFastScalarSize - 1u ||
            next.base != current.base + MemoryInline::kPageSize)
            continue;
        const uintptr_t guestPageBase = static_cast<uintptr_t>(page) << MemoryInline::kPageShift;
        const uintptr_t bias = reinterpret_cast<uintptr_t>(current.base) - guestPageBase;
        MemoryInline::g_fullPageBias[page] = bias + 1u;
    }
}

void RefreshFastPathTables() {
    auto& sparseStorage = SparseWritablePageTableStorage();
    for (uint32_t page = 0; page < MemoryInline::kPageCount; ++page) {
        const uintptr_t bias = MemoryInline::g_fullPageBias[page];
        MemoryInline::g_sparseWritablePageTables[page] = nullptr;
        sparseStorage[page].reset();

        const bool coarseExecutable =
            RecompMod::g_executableWriteGuardCoarsePages[page].load(
                std::memory_order_relaxed) != 0;
        MemoryInline::g_fullWritablePageBias[page] = coarseExecutable ? 0 : bias;
        if (!coarseExecutable || bias == 0)
            continue;

        auto sparse = std::make_unique<MemoryInline::SparseWritablePageTable>();
        bool hasExecutableSubPage = false;
        bool hasWritableSubPage = false;
        const uint32_t firstExactPage =
            page * MemoryInline::kWritableSubPagesPerPage;
        for (uint32_t subPage = 0;
             subPage < MemoryInline::kWritableSubPagesPerPage; ++subPage) {
            const bool executable =
                RecompMod::g_executableWriteGuardPages[firstExactPage + subPage].load(
                    std::memory_order_relaxed) != 0;
            hasExecutableSubPage |= executable;
            hasWritableSubPage |= !executable;
            sparse->encodedBias[subPage] = executable ? 0 : bias;
        }

        // A homogeneous executable page has no writable fast path; a
        // homogeneous data page already uses g_fullWritablePageBias. Retain an
        // allocation only for the intended mixed case.
        if (hasExecutableSubPage && hasWritableSubPage) {
            MemoryInline::g_sparseWritablePageTables[page] = sparse.get();
            sparseStorage[page] = std::move(sparse);
        }
    }
}

Region* TryResolveRegion(uint32_t address, size_t length) {
    auto& regions = Regions();
    for (auto& region : regions) {
        const uint64_t base = region.config.baseAddress;
        const uint64_t limit = base + region.storageSize;
        const uint64_t addr = address;
        if (addr >= base && addr <= limit && length <= limit - addr) {
            return &region;
        }
    }
    return nullptr;
}

Region& ResolveRegion(uint32_t address, size_t length) {
    if (Region* region = TryResolveRegion(address, length)) {
        return *region;
    }
    // Emit extra context to help diagnose early-boot accesses that miss the map.
    if (auto* cpu = TryGetCpuContext()) {
        const uint32_t active = RecompMod::CurrentTranslatedExecutionAddress();
        RT_LOG(RT_TAG_MEMORY) << "AccessViolation ctx=" << cpu
                  << " pc=0x" << std::hex << cpu->pc
                  << " active=0x" << active
                  << " lr=0x" << cpu->lr
                  << " r1=0x" << cpu->gpr[1]
                  << " r8=0x" << cpu->gpr[8]
                  << " r9=0x" << cpu->gpr[9]
                  << " r12=0x" << cpu->gpr[12]
                  << " r30=0x" << cpu->gpr[30]
                  << " r31=0x" << cpu->gpr[31]
                  << std::dec
                  << " addr=0x" << std::hex << address
                  << " len=" << length << std::dec
                  << " reason=no mapped region" << std::endl;
    } else {
        RT_LOG(RT_TAG_MEMORY) << "AccessViolation ctx=(null) addr=0x" << std::hex << address
                  << " len=" << length << std::dec << " reason=no mapped region" << std::endl;
    }
    RT_LOG(RT_TAG_MEMORY) << "===== DUMPING CPU STATE =====" << std::endl;
    SystemBridge::DumpCpuState(TryGetCpuContext());
    // Hexdump guest memory around pointer-carrying registers so a corrupted
    // structure's surroundings (e.g. ASCII sprayed over a link pointer) are
    // visible in the report without a debugger attached.
    if (auto* cpu = TryGetCpuContext()) {
        for (const int reg : {4, 5, 6, 7, 8, 26, 27, 28, 29, 30, 31}) {
            const uint32_t base = cpu->gpr[reg];
            if (base < 0x80000000u || base >= 0x94000000u)
                continue;
            const uint32_t start = (base - 0x40u) & ~0xFu;
            RT_LOG(RT_TAG_MEMORY) << "hexdump around r" << reg << "=0x" << std::hex << base << ":" << std::endl;
            for (uint32_t row = 0; row < 16; ++row) {
                const uint32_t rowAddr = start + row * 16u;
                RT_LOG(RT_TAG_MEMORY) << "  0x" << std::hex << rowAddr << ":";
                char ascii[17] = {};
                for (uint32_t i = 0; i < 16; ++i) {
                    uint8_t byte = 0;
                    if (!MemoryInline::TryReadGuestScalar(rowAddr + i, byte)) {
                        std::cerr << " ??";
                        ascii[i] = '?';
                        continue;
                    }
                    std::cerr << " " << std::setw(2) << std::setfill('0') << static_cast<uint32_t>(byte);
                    ascii[i] = (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
                }
                std::cerr << "  |" << ascii << "|" << std::dec << std::setfill(' ') << std::endl;
            }
        }
    }
#if defined(_WIN32)
    void* frames[32]{};
    const USHORT captured = CaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)), frames, nullptr);
    const auto imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    HANDLE process = GetCurrentProcess();
    static const bool symbolsReady = [] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        if (SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE)
            return true;
        // The runtime crash reporter may already own the process-wide DbgHelp
        // session. Reuse it rather than treating ERROR_INVALID_PARAMETER as
        // symbol unavailability.
        return GetLastError() == ERROR_INVALID_PARAMETER;
    }();
    RT_LOG(RT_TAG_MEMORY) << "host stack at first invalid guest access:" << std::endl;
    for (USHORT index = 0; index < captured; ++index) {
        const auto addressValue = reinterpret_cast<uintptr_t>(frames[index]);
        RT_LOG(RT_TAG_MEMORY) << "  #" << index << " absolute=0x" << std::hex << addressValue;
        if (imageBase != 0 && addressValue >= imageBase)
            std::cerr << " image+0x" << (addressValue - imageBase);
        std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> symbolBuffer{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        if (symbolsReady && SymFromAddr(process, addressValue, &displacement, symbol))
            std::cerr << " " << symbol->Name << "+0x" << displacement;
        std::cerr << std::dec << std::endl;
    }
#endif
    std::cerr.flush();
    throw Memory::AccessViolation(address, length, "no mapped region");
}

GuestFlat::Backing ClassifyBacking(uint32_t baseAddress) {
    // MEM1: physical (0x00000000), cached (0x80000000), uncached (0xC0000000)
    if ((baseAddress >= Memory::kMem1PhysicalBase &&
         baseAddress < Memory::kMem1PhysicalBase + Memory::kMem1Size) ||
        (baseAddress >= Memory::kMem1CachedBase &&
         baseAddress < Memory::kMem1CachedBase + Memory::kMem1Size) ||
        (baseAddress >= Memory::kMem1UncachedBase &&
         baseAddress < Memory::kMem1UncachedBase + Memory::kMem1Size)) {
        return GuestFlat::Backing::Mem1;
    }

    // Expanded MEM2 holds Brawl's prelinked modules above the retail 64 MiB.
    // Physical, cached and uncached views alias the same backing.
    if ((baseAddress >= Memory::kMem2PhysicalBase &&
         baseAddress < Memory::kMem2PhysicalEnd) ||
        (baseAddress >= Memory::kMem2CachedBase &&
         baseAddress < Memory::kMem2CachedEnd) ||
        (baseAddress >= Memory::kMem2UncachedBase &&
         baseAddress < Memory::kMem2UncachedEnd)) {
        return GuestFlat::Backing::Mem2;
    }

    return GuestFlat::Backing::Owned;
}


template <typename T>
T ReadScalar(uint32_t address) {
    // This is the cold *Slow path; Memory::GetPointer already tries the fast
    // probe first, so repeating it here only guaranteed a second miss.
    auto* ptr = Memory::GetPointer(address, sizeof(T));
    if constexpr (sizeof(T) == 1) {
        return *ptr;
    } else {
        T value = 0;
        std::memcpy(&value, ptr, sizeof(T));
        return MemoryInline::MaybeByteSwap(value);
    }
}

template <typename T>
void WriteScalar(uint32_t address, T value) {
    if (RecompMod::HandleExecutableWrite(address, sizeof(T), static_cast<uint64_t>(value))) {
        return;
    }

    auto* ptr = Memory::GetPointer(address, sizeof(T));
    if constexpr (sizeof(T) == 1) {
        *ptr = static_cast<uint8_t>(value);
    } else {
        const T swapped = MemoryInline::MaybeByteSwap(value);
        std::memcpy(ptr, &swapped, sizeof(T));
    }
}

} // namespace

Memory::AccessViolation::AccessViolation(uint32_t address, size_t length, std::string_view reason)
    : std::runtime_error([&]() {
          std::ostringstream oss;
          oss << "Memory access violation at 0x" << std::hex << std::uppercase << address
              << " (+0x" << length << ") :: " << reason;
          return oss.str();
      }()),
      address_(address),
      length_(length),
      reason_(reason) {
    // The handler that reports it runs after unwinding, without the guest context: record that now.
    static std::atomic<int> reported{0};
    if (const CpuContext* cpu = TryGetCpuContext(); cpu && reported.fetch_add(1, std::memory_order_relaxed) < 3) {
        RT_LOG(RT_TAG_MEMORY) << what() << ", guest state where it happened:" << std::endl;
        SystemBridge::DumpCpuState(cpu);
    }
}

void Memory::RefreshWritableFastPathsForExecutableRanges() {
    std::lock_guard<std::mutex> lock(RegionMutex());
    RefreshFastPathTables();
}

Memory::Config Memory::Config::WiiDefaults() {
    // Physical / cached / uncached views of MEM1 and MEM2, then the locked cache.
    Config config;

    config.regions.push_back(RegionConfig{
        .name = "MEM1_PHYS",
        .baseAddress = Memory::kMem1PhysicalBase,
        .sizeBytes = Memory::kMem1Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM1",
        .baseAddress = Memory::kMem1CachedBase,
        .sizeBytes = Memory::kMem1Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM1_UNCACHED",
        .baseAddress = Memory::kMem1UncachedBase,
        .sizeBytes = Memory::kMem1Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM2_PHYS",
        .baseAddress = Memory::kMem2PhysicalBase,
        .sizeBytes = Memory::kMem2Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM2",
        .baseAddress = Memory::kMem2CachedBase,
        .sizeBytes = Memory::kMem2Size,
    });

    config.regions.push_back(RegionConfig{
        .name = "MEM2_UNCACHED",
        .baseAddress = Memory::kMem2UncachedBase,
        .sizeBytes = Memory::kMem2Size,
    });

    // Locked cache (THP decoder fast RAM) is really 16KB at 0xE0000000, but a sub-page mapping can
    // never enter the coarse 1MiB bias tables, forcing every THP load/store through the checked
    // fallback (the dominant cost of THP-heavy screens). Back a full 1MiB page plus the successor
    // page the bias builder needs, so locked-cache access stays on the two-instruction native path.
    constexpr size_t lcSize = MemoryInline::kPageSize + 4096u;
    config.regions.push_back(RegionConfig{
        .name = "LOCKED_CACHE",
        .baseAddress = 0xE0000000,
        .sizeBytes = lcSize,
    });

    return config;
}

void Memory::Init(size_t mem1Size) {
    Config config;
    config.regions.push_back(RegionConfig{.name = "MEM1", .baseAddress = 0x80000000, .sizeBytes = mem1Size});
    Init(config);
}

void Memory::Init(const Config& config) {
    std::lock_guard<std::mutex> lock(RegionMutex());
    auto& regions = Regions();
    regions.clear();
    ClearPageTable();
    regions.reserve(config.regions.size());

#if MKW_FLAT_MEMORY
    // The flat reservation backs every guest view from one shared section object, so cached/uncached/
    // physical mirrors alias as before. GetPointer still hands out the unprotected host alias, so
    // native code (image loading, DVD reads, HLE) is unaffected by the guest view's protections.
    {
        std::vector<GuestFlat::RegionRequest> flatRegions;
        flatRegions.reserve(config.regions.size());
        for (const auto& regionConfig : config.regions) {
            flatRegions.push_back(GuestFlat::RegionRequest{
                regionConfig.baseAddress, regionConfig.sizeBytes,
                ClassifyBacking(regionConfig.baseAddress)});
        }
        GuestFlat::Initialize(flatRegions);
    }

#else
    // Cached, uncached and physical guest aliases share one allocation per RAM
    // bank. Other configured regions own their storage independently.
    struct Store { size_t size = 0; std::shared_ptr<std::vector<uint8_t>> bytes; };
    std::unordered_map<uint64_t, Store> stores;
    const auto key = [](uint32_t base) {
        const auto backing = ClassifyBacking(base);
        return (static_cast<uint64_t>(backing) << 32) |
            (backing == GuestFlat::Backing::Owned ? base : 0u);
    };
    const auto offset = [](uint32_t base) -> size_t {
        switch (ClassifyBacking(base)) {
        case GuestFlat::Backing::Mem1: return base & 0x01FFFFFFu;
        case GuestFlat::Backing::Mem2: return base & 0x0FFFFFFFu;
        default: return 0;
        }
    };
    for (const auto& region : config.regions) {
        if (region.sizeBytes > GuestFlat::kGuestSpaceSize - region.baseAddress)
            throw std::invalid_argument("Guest memory region exceeds the 32-bit address space");
        auto& store = stores[key(region.baseAddress)];
        store.size = std::max(store.size, offset(region.baseAddress) + region.sizeBytes);
    }
    for (auto& [_, store] : stores)
        store.bytes = std::make_shared<std::vector<uint8_t>>(store.size);
#endif

    // Map virtual regions onto that backing store, with mirroring.
    for (const auto& regionConfig : config.regions) {
        Region instance;
        instance.config = regionConfig;

#if MKW_FLAT_MEMORY
        instance.storagePtr = GuestFlat::HostPointer(regionConfig.baseAddress);
        if (instance.storagePtr == nullptr && regionConfig.sizeBytes != 0) {
            throw std::runtime_error("Flat guest mapping is missing region '" + regionConfig.name + "'");
        }
#else
        instance.storage = stores.at(key(regionConfig.baseAddress)).bytes;
        instance.storagePtr = instance.storage->empty() ? nullptr :
            instance.storage->data() + offset(regionConfig.baseAddress);
#endif
        instance.storageSize = regionConfig.sizeBytes;

        regions.emplace_back(std::move(instance));
    }

    BuildPageTable();
    RefreshFastPathTables();
}

void Memory::Reset() {
    std::lock_guard<std::mutex> lock(RegionMutex());
    Regions().clear();
    ClearPageTable();
}

namespace {
// MMIO reads have no backing store or generic HLE. Answering zero (the old sparse fallback) turned
// unimplemented devices into silent hangs, so unmapped reads throw like MMIO writes; every device
// we do implement is hooked at the function level, so a hit here means a missing HLE hook.
[[noreturn]] void ThrowMmioReadBlocked(uint32_t addr, size_t length) {
    std::ostringstream reason;
    reason << (MemoryInline::IsGpuFifoAddress(addr)
                   ? "GPU FIFO read blocked (the gather pipe is write-only)"
                   : "MMIO read blocked (non-GPU)")
           << "; add HLE for this device instead of answering zero"
           << " (active=0x" << std::hex << std::uppercase
           << RecompMod::CurrentTranslatedExecutionAddress() << std::dec << std::nouppercase << ")";
    throw Memory::AccessViolation(addr, length, reason.str());
}
} // namespace

uint8_t MemoryInline::Read8Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        if (!IsDeviceMmioAddress(addr)) return 0;  // nothing on the bus there: reads 0, writes dropped
        ThrowMmioReadBlocked(addr, sizeof(uint8_t));
    }
    return ReadScalar<uint8_t>(addr);
}

uint16_t MemoryInline::Read16Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        if (!IsDeviceMmioAddress(addr)) return 0;  // nothing on the bus there: reads 0, writes dropped
        ThrowMmioReadBlocked(addr, sizeof(uint16_t));
    }
    return ReadScalar<uint16_t>(addr);
}

uint32_t MemoryInline::Read32Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        if (!IsDeviceMmioAddress(addr)) return 0;  // nothing on the bus there: reads 0, writes dropped
        ThrowMmioReadBlocked(addr, sizeof(uint32_t));
    }
    const uint32_t value = ReadScalar<uint32_t>(addr);
    return value;
}

uint64_t MemoryInline::Read64Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        if (!IsDeviceMmioAddress(addr)) return 0;  // nothing on the bus there: reads 0, writes dropped
        ThrowMmioReadBlocked(addr, sizeof(uint64_t));
    }
    return ReadScalar<uint64_t>(addr);
}

float MemoryInline::ReadFloat32Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        if (!IsDeviceMmioAddress(addr)) return 0;  // nothing on the bus there: reads 0, writes dropped
        ThrowMmioReadBlocked(addr, sizeof(float));
    }
    const auto bits = ReadScalar<uint32_t>(addr);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double MemoryInline::ReadFloat64Slow(uint32_t addr) {
    if (IsMmioAddress(addr)) {
        if (!IsDeviceMmioAddress(addr)) return 0;  // nothing on the bus there: reads 0, writes dropped
        ThrowMmioReadBlocked(addr, sizeof(double));
    }
    const auto bits = ReadScalar<uint64_t>(addr);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void MemoryInline::Write8Slow(uint32_t addr, uint8_t val) {
    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_Write8(val);
        return;
    }
    if (IsMmioAddress(addr)) {
        if (!IsDeviceMmioAddress(addr)) return;  // nothing on the bus there: reads 0, writes dropped
        throw Memory::AccessViolation(addr, sizeof(val), "MMIO write blocked (non-GPU)");
    }
    WriteScalar(addr, val);
}

void MemoryInline::Write16Slow(uint32_t addr, uint16_t val) {
    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_Write16(val);
        return;
    }
    if (IsMmioAddress(addr)) {
        if (!IsDeviceMmioAddress(addr)) return;  // nothing on the bus there: reads 0, writes dropped
        throw Memory::AccessViolation(addr, sizeof(val), "MMIO write blocked (non-GPU)");
    }
    WriteScalar(addr, val);
}

void MemoryInline::Write32Slow(uint32_t addr, uint32_t val) {
    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_Write32(val);
        return;
    }
    if (IsMmioAddress(addr)) {
        if (!IsDeviceMmioAddress(addr)) return;  // nothing on the bus there: reads 0, writes dropped
        throw Memory::AccessViolation(addr, sizeof(val), "MMIO write blocked (non-GPU)");
    }
    WriteScalar(addr, val);
}

void MemoryInline::Write64Slow(uint32_t addr, uint64_t val) {
    if (IsGpuFifoAddress(addr)) {
        // The gather pipe is a byte-stream FIFO; a 64-bit store is two big-endian
        // 32-bit pushes, high word first. Without this arm the write fell through
        // to WriteScalar, which found no mapped region behind 0xCC008000 and threw
        // "no mapped region" instead of reaching GX.
        GX_HLE_FIFO_Write32(static_cast<uint32_t>(val >> 32));
        GX_HLE_FIFO_Write32(static_cast<uint32_t>(val));
        return;
    }
    if (IsMmioAddress(addr)) {
        if (!IsDeviceMmioAddress(addr)) return;  // nothing on the bus there: reads 0, writes dropped
        throw Memory::AccessViolation(addr, sizeof(val), "MMIO write blocked (non-GPU)");
    }
    WriteScalar(addr, val);
}

void MemoryInline::WriteFloat32Slow(uint32_t addr, double val) {
    // stfs stores the IEEE-754 bit pattern of the single-precision value, never a
    // truncated integer.
    const uint32_t bits = ConvertPpcDoubleToSingleBits(val);
    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_WriteFloat(PpcSingleBitsToFloat(bits));
        return;
    }
    if (IsMmioAddress(addr)) {
        if (!IsDeviceMmioAddress(addr)) return;  // nothing on the bus there: reads 0, writes dropped
        throw Memory::AccessViolation(addr, sizeof(float), "MMIO write blocked (non-GPU)");
    }

    WriteScalar(addr, bits);
}

void MemoryInline::WriteFloat64Slow(uint32_t addr, double val) {
    // stfd stores the full 64-bit FPR bit pattern - load-bearing for the
    // fctiwz->stfd->lwz idiom, where the integer result lives in the low word.
    uint64_t bits;
    std::memcpy(&bits, &val, sizeof(bits));

    if (IsGpuFifoAddress(addr)) {
        GX_HLE_FIFO_WriteFloat(static_cast<float>(val));
        return;
    }
    if (IsMmioAddress(addr)) {
        if (!IsDeviceMmioAddress(addr)) return;  // nothing on the bus there: reads 0, writes dropped
        throw Memory::AccessViolation(addr, sizeof(double), "MMIO write blocked (non-GPU)");
    }

    WriteScalar(addr, bits);
}

uint8_t* Memory::GetPointer(uint32_t addr) {
    return GetPointer(addr, 1);
}

uint8_t* Memory::GetPointer(uint32_t addr, size_t length) {
    if (auto* ptr = MemoryInline::GetPointerFast(addr, length)) {
        return ptr;
    }
    auto& region = ResolveRegion(addr, length);
    auto offset = static_cast<size_t>(addr - region.config.baseAddress);
    return region.storagePtr + offset;
}

bool Memory::Contains(uint32_t addr, size_t length) {
    if (MemoryInline::GetPointerFast(addr, length)) {
        return true;
    }
    // Contains is a membership predicate: callers probe arbitrary guest values
    // (e.g. gfx-node slots that may hold floats), so a miss must stay silent
    // rather than take ResolveRegion's crash-dump path.
    return TryResolveRegion(addr, length) != nullptr;
}

std::vector<Memory::RegionConfig> Memory::DescribeRegions() {
    std::lock_guard<std::mutex> lock(RegionMutex());
    std::vector<RegionConfig> info;
    info.reserve(Regions().size());
    for (const auto& region : Regions()) {
        info.push_back(RegionConfig{region.config.name, region.config.baseAddress, region.storageSize});
    }
    return info;
}
