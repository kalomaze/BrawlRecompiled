#include "abi_bridge.h"
#include "ppc_interpreter.h"
#include "memory_access.h"
#include "isa/ppc_isa_fpenv.h"
#include <cassert>
#include <iostream>

// Single loads preserve subnormal values and NaN payloads even in NI mode.
// Olimar's script getter uses lfsx to load 0x0040000b; flushing it changes
// script conditions and consumes an extra RNG value.
static void TestSingleLoads() {
    const uint32_t savedControl = MkwGetHostFpControl();
    struct Case { uint32_t single; uint64_t widened; };
    const Case cases[] = {
        {0x00000001, 0x36a0000000000000ULL},
        {0x0040000b, 0x38000002c0000000ULL},
        {0x8040000b, 0xb8000002c0000000ULL},
        {0x00800000, 0x3810000000000000ULL},
        {0x80000000, 0x8000000000000000ULL},
        {0x7f800000, 0x7ff0000000000000ULL},
        {0x7f800001, 0x7ff0000020000000ULL},
    };
    Memory::Write32(0x80004000, (48u << 26) | (2u << 21) | (3u << 16)); // lfs f2,0(r3)
    Memory::Write32(0x80004004, (31u << 26) | (4u << 21) | (3u << 16) | (535u << 1)); // lfsx f4,r3,r0
    Memory::Write32(0x80004008, 0x4e800020); // blr
    for (uint32_t fpscr : {0u, 4u}) {
        for (const auto& c : cases) {
            Memory::Write32(0x80005000, c.single);
            MkwApplyHostNiMode(fpscr);
            assert(PpcBitCastToU64Inline(MemoryInline::FlatReadFloat32(0x80005000)) == c.widened);
            auto* host = MemoryInline::ResolveRangeHost(0x80005000, 0, 4, true, false);
            assert(PpcBitCastToU64Inline(MemoryInline::ReadResolvedFloat32(host, 0, 0x80005000)) == c.widened);
            CpuContext ctx{};
            ctx.fpscr = fpscr;
            ctx.gpr[3] = 0x80005000;
            ctx.lr = 0x80006000;
            PpcInterpreter::Run(&ctx, 0x80004000);
            assert(PpcBitCastToU64Inline(ctx.fpr[2].d) == c.widened);
            assert(PpcBitCastToU64Inline(ctx.fpr[4].d) == c.widened);
        }
    }
    MkwRestoreHostMxcsr(savedControl);
}

// P+'s jump patch holds scalar f3 across a posture-scale call, then uses it
// as the multiplier in ps_muls0. Exercise the actual interpreter/registry
// boundary, including a paired value and the scalar f1 return.
static void ReturnScale(CpuContext* ctx) { ctx->fpr[1].d = 0.5; }
int main() {
    Memory::Init(0x10000);
    TestSingleLoads();
    TranslatedFunctionInfo callee{};
    callee.address = 0x80002000;
    callee.name = "ReturnScale";
    callee.rawCpuInvoker = ReturnScale;
    callee.entryPoint = reinterpret_cast<void*>(ReturnScale);
    callee.kind = FunctionKind::Native;
    TranslatedFunctionRegistry::Register(std::move(callee));
    const uint32_t words[] = {
        (63u<<26)|(3u<<21)|(1u<<11)|(72u<<1), // fmr f3,f1
        (4u<<26)|(6u<<21)|(2u<<16)|(2u<<11)|(21u<<1), // ps_add f6,f2,f2
        0x48000ff9, // bl 0x80002000 from 0x80001008
        (4u<<26)|(4u<<21)|(2u<<16)|(3u<<6)|(12u<<1), // ps_muls0 f4,f2,f3
        (4u<<26)|(5u<<21)|(2u<<16)|(1u<<6)|(12u<<1), // ps_muls0 f5,f2,f1
        (4u<<26)|(7u<<21)|(6u<<16)|(3u<<6)|(12u<<1), // ps_muls0 f7,f6,f3
        0x7fc803a6, // mtlr r30
        0x4e800020, // blr
    };
    for (size_t i=0;i<sizeof(words)/sizeof(*words);++i) Memory::Write32(0x80001000+4*i,words[i]);
    CpuContext ctx{};
    ctx.lr=ctx.gpr[30]=0x80003000;
    ctx.fpr[1].d=1.0;
    ctx.fpr[2].d=2.455000877380371;
    PpcInterpreter::Run(&ctx,0x80001000);
    // These destinations were written by paired instructions.
    assert(PPC_PsToScalarInline(ctx.fpr[4].d)==2.455000877380371);
    assert(PPC_PsToScalarInline(ctx.fpr[5].d)==1.2275004386901855);
    assert(PPC_PsToScalarInline(ctx.fpr[7].d)==4.910001754760742);
    assert(ctx.fpr[4].paired.ps1 == 2.455000877380371f);
    assert(ctx.fpr[5].paired.ps1 == 1.2275004386901855f);
    assert(ctx.fpr[7].paired.ps1 == 4.910001754760742f);
    std::cout << "interpreter call forms: ok\n";
}
