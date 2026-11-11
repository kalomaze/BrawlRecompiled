// Standalone: c++ -std=c++17 -Iruntime/include runtime/tests/cr_logical_tests.cpp -o /tmp/cr-logical-test
#include "isa/ppc_isa_cr.h"
#include <cstdlib>
#include <cstdint>

void require(bool condition) { if (!condition) std::abort(); }

int main() {
    // Truth tables ordered (A,B) = 00,01,10,11. PowerPC CR logical operations:
    // NOR, AND-complement, XOR, NAND, AND, equivalence, OR-complement, OR.
    constexpr uint8_t truth[8] = {0b0001,0b0100,0b0110,0b0111,0b1000,0b1001,0b1101,0b1110};
    for (unsigned op=0; op<8; ++op)
        for (unsigned a=0; a<2; ++a)
            for (unsigned b=0; b<2; ++b)
                for (unsigned dst=0; dst<32; ++dst) {
                    uint32_t before=0x5a5aa5a5;
                    before=PpcCrSetBitResident(before,4,a);
                    before=PpcCrSetBitResident(before,1,b);
                    const uint32_t after=PpcCrLogicalResident(before,op,dst,4,1);
                    const uint32_t bit=1u<<(31-dst);
                    require(((after & bit)!=0) == ((truth[op]>>(2*a+b))&1));
                    require((before & ~bit)==(after & ~bit));
                }
    // PPOnline lockChar: resume=false, lastCss=-1 must branch to selectedChar.
    uint32_t cr=0;
    SetCRResident(cr,0,0,int32_t(0),int32_t(0));
    SetCRResident(cr,0,1,int32_t(-1),int32_t(0));
    cr=PpcCrLogicalResident(cr,6,20,4,1);
    require(GetCRBitResident(cr,5,0));
}
