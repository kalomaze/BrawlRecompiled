#include "abi_bridge.h"
#include "ppc_interpreter.h"
#include <cassert>
#include <iostream>

// P+'s jump patch holds scalar f3 across a posture-scale call, then uses it
// as the multiplier in ps_muls0. Exercise the actual interpreter/registry
// boundary, including a paired value and the scalar f1 return.
static void ReturnScale(CpuContext* ctx) { ctx->fpr[1].d = 0.5; }
int main() {
    Memory::Init(0x10000);
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
