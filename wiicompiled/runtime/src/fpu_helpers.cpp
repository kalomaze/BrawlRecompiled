#include "ppc_runtime.h"
#include "memory.h"

#include <cmath>
#include <cstdint>

namespace {

constexpr uint32_t kFpscrFx = 1u << 31;
constexpr uint32_t kFpscrFex = 1u << 30;
constexpr uint32_t kFpscrVx = 1u << 29;
constexpr uint32_t kFpscrOx = 1u << 28;
constexpr uint32_t kFpscrUx = 1u << 27;
constexpr uint32_t kFpscrZx = 1u << 26;
constexpr uint32_t kFpscrXx = 1u << 25;
constexpr uint32_t kFpscrVxSnan = 1u << 24;
constexpr uint32_t kFpscrVxIsi = 1u << 23;
constexpr uint32_t kFpscrVxIdi = 1u << 22;
constexpr uint32_t kFpscrVxZdz = 1u << 21;
constexpr uint32_t kFpscrVxImz = 1u << 20;
constexpr uint32_t kFpscrVxVc = 1u << 19;
constexpr uint32_t kFpscrVxSoft = 1u << 10;
constexpr uint32_t kFpscrVxSqrt = 1u << 9;
constexpr uint32_t kFpscrVxCvi = 1u << 8;
constexpr uint32_t kFpscrVe = 1u << 7;
constexpr uint32_t kFpscrOe = 1u << 6;
constexpr uint32_t kFpscrUe = 1u << 5;
constexpr uint32_t kFpscrZe = 1u << 4;
constexpr uint32_t kFpscrXe = 1u << 3;
constexpr uint32_t kFpscrVxAny =
    kFpscrVxSnan | kFpscrVxIsi | kFpscrVxIdi | kFpscrVxZdz | kFpscrVxImz |
    kFpscrVxVc | kFpscrVxSoft | kFpscrVxSqrt | kFpscrVxCvi;
constexpr uint32_t kFpscrAnyX = kFpscrOx | kFpscrUx | kFpscrZx | kFpscrXx | kFpscrVxAny;
constexpr uint32_t kFpscrAnyE = kFpscrVe | kFpscrOe | kFpscrUe | kFpscrZe | kFpscrXe;

// VX and FEX are derived from exception causes and enables, never independent writable bits.
void FpscrUpdated(CpuContext* cpu)
{
    if ((cpu->fpscr & kFpscrVxAny) != 0)
    {
        cpu->fpscr |= kFpscrVx;
    }
    else
    {
        cpu->fpscr &= ~kFpscrVx;
    }

    if (((cpu->fpscr >> 22) & (cpu->fpscr & kFpscrAnyE)) != 0)
    {
        cpu->fpscr |= kFpscrFex;
    }
    else
    {
        cpu->fpscr &= ~kFpscrFex;
    }
    MkwApplyHostNiMode(cpu->fpscr);
}

double RoundNearestEven(double value) {
    const double floor_value = std::floor(value);
    const double frac = value - floor_value;
    if (frac < 0.5) {
        return floor_value;
    }
    if (frac > 0.5) {
        return floor_value + 1.0;
    }

    const double half = floor_value / 2.0;
    return half == std::floor(half) ? floor_value : floor_value + 1.0;
}
}

// CurrentCpuContext() aborts on a null context (isa/ppc_isa_context.h), so it
// never returns nullptr - none of the helpers below guard against one.
extern "C" void PPC_Mtfsf(uint32_t fieldMask, double source) {
    CpuContext* cpu = CurrentCpuContext();
    uint32_t mask = 0;
    for (unsigned field = 0; field < 8; ++field) {
        if (fieldMask & (1u << field))
            mask |= 0xFu << (4 * field);
    }
    cpu->fpscr = (cpu->fpscr & ~mask) | (PPC_FprLowWordInline(source) & mask);
    FpscrUpdated(cpu);
}

extern "C" void PPC_Mtfsb1(uint32_t bit) {
    CpuContext* cpu = CurrentCpuContext();
    const uint32_t mask = 0x80000000u >> (bit & 31u);
    // FX is sticky, but only a newly raised exception sets it.
    if ((mask & kFpscrAnyX) != 0 && (cpu->fpscr & mask) == 0)
        cpu->fpscr |= kFpscrFx;
    cpu->fpscr |= mask;
    FpscrUpdated(cpu);
}

extern "C" void PPC_Mtfsb0(uint32_t bit) {
    CpuContext* cpu = CurrentCpuContext();
    cpu->fpscr &= ~(0x80000000u >> (bit & 31u));
    FpscrUpdated(cpu);
}

extern "C" void PPC_Mtfsfi(uint32_t field, uint32_t value) {
    CpuContext* cpu = CurrentCpuContext();
    const uint32_t shift = (7u - (field & 7u)) * 4u;
    const uint32_t mask = 0xFu << shift;
    cpu->fpscr = (cpu->fpscr & ~mask) | ((value << shift) & mask);
    FpscrUpdated(cpu);
}

extern "C" uint32_t PPC_Mcrfs(uint32_t dstField, uint32_t srcField)
{
    CpuContext* cpu = CurrentCpuContext();

    const uint32_t dst = dstField & 7u;
    const uint32_t src = srcField & 7u;
    const uint32_t value = (cpu->fpscr >> ((7u - src) * 4u)) & 0xFu;
    cpu->cr = (cpu->cr & ~(0xFu << ((7u - dst) * 4u))) | (value << ((7u - dst) * 4u));
    cpu->fpscr &= ~((0xFu << ((7u - src) * 4u)) & (kFpscrFx | kFpscrAnyX));
    FpscrUpdated(cpu);
    return cpu->cr;
}

extern "C" double PPC_Mffs() {
    // Broadway Manual 12.2: the FPSCR is placed into the low-order bits of frD;
    // the high-order bits are architecturally undefined and left zero here.
    return PpcBitCastToDoubleInline(static_cast<uint64_t>(CurrentCpuContext()->fpscr));
}

// PPC_Fctiwz is defined inline in ppc_runtime.h. It is the single most emitted
// float helper in the translation (1,051 sites) and this build has no LTO, so
// an out-of-line definition made every fctiwz a full caller-saved register
// barrier around two compares and a cvttsd2si. PPC_Fctiw below shares the same
// isa pack/clamp inlines so the two cannot drift apart.

extern "C" double PPC_Fctiw(double value) {
    const uint32_t rn = CurrentCpuContext()->fpscr & 0x3u;

    double rounded = value;
    switch (rn) {
    case 0:
        rounded = RoundNearestEven(value);
        break;
    case 1:
        rounded = std::trunc(value);
        break;
    case 2:
        rounded = std::ceil(value);
        break;
    case 3:
        rounded = std::floor(value);
        break;
    }

    return PpcPackIntegerWordInline(PpcClampIntegerWordInline(rounded));
}

// stfiwx stores the low 32 bits of an FPR as raw integer data with no
// floating-point conversion - the store half of the fctiwz -> stfiwx idiom.
extern "C" void PPC_Stfiwx(uint32_t addr, double fprValue) {
    Memory::Write32(addr, PPC_FprLowWordInline(fprValue));
}
