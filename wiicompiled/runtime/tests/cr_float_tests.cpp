#include "isa/ppc_isa_cr.h"
#include <cassert>
#include <cstring>

// Volatile bit patterns prevent compile-time evaluation from hiding the
// finite-math-only miscompile of a runtime guest comparison.
volatile uint64_t values[] = {
    0, 0x8000000000000000ULL, 0x3ff0000000000000ULL,
    0x7ff0000000000000ULL, 0xfff0000000000000ULL,
    0x7ff8000000000000ULL, 0xfff8000000000000ULL, 0x7ff0000000000001ULL
};
double value(unsigned i) {
    uint64_t bits=values[i];double result;std::memcpy(&result,&bits,8);return result;
}
int main() {
    for(unsigned field=0;field<8;++field) {
        const auto check=[&](double a,double b,unsigned expected) {
            uint32_t cr=0x5a5aa5a5;const unsigned shift=4*(7-field);
            SetCRFloatResident(cr,field,a,b);
            assert(((cr>>shift)&15)==expected);
            assert((cr&~(15u<<shift))==(0x5a5aa5a5u&~(15u<<shift)));
        };
        check(value(0),value(1),2); // signed zero is equal
        check(value(0),value(2),8);
        check(value(2),value(0),4);
        check(value(3),value(2),4);
        check(value(4),value(2),8);
        check(value(3),value(3),2);
        for(unsigned n=5;n<8;++n) {
            check(value(0),value(n),1);
            check(value(n),value(0),1);
            check(value(n),value(n),1);
        }
        // Dreamland's disabled-hazard timer: -inf - -inf -> unordered.
        double timer=(value(4)-value(4))*value(2)+value(4);
        check(value(0),timer,1);
    }
}
