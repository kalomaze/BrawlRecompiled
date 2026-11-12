// AArch64 cooperative context frame: x19-x30 and v8-v15 (240 bytes).
// Darwin also preserves its reserved x18. ELF targets leave x18 untouched;
// Android uses it as the host thread's shadow-call-stack pointer.
#if defined(__APPLE__)
#define SYM(name) _##name
#else
#define SYM(name) name
#endif

    .text
    .p2align 2
    .globl SYM(mkw_co_switch)
#if !defined(__APPLE__)
    .type SYM(mkw_co_switch), %function
#endif
SYM(mkw_co_switch):
    sub sp, sp, #240
#if defined(__APPLE__)
    str x18, [sp, #0]
#endif
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp x29, x30, [sp, #96]
    stp q8, q9, [sp, #112]
    stp q10, q11, [sp, #144]
    stp q12, q13, [sp, #176]
    stp q14, q15, [sp, #208]
    mov x2, sp
    str x2, [x1]

    ldr x2, [x0]
    mov sp, x2
#if defined(__APPLE__)
    ldr x18, [sp, #0]
#endif
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp x29, x30, [sp, #96]
    ldp q8, q9, [sp, #112]
    ldp q10, q11, [sp, #144]
    ldp q12, q13, [sp, #176]
    ldp q14, q15, [sp, #208]
    add sp, sp, #240
    ret
#if !defined(__APPLE__)
    .size SYM(mkw_co_switch), . - SYM(mkw_co_switch)
#endif

// Creates a frame compatible with SYM(mkw_co_switch) and returns its saved SP.
// x0 = one-past-end stack pointer, x1 = entry(void*), x2 = entry argument.
    .globl SYM(mkw_co_init)
#if !defined(__APPLE__)
    .type SYM(mkw_co_init), %function
#endif
SYM(mkw_co_init):
    bic x0, x0, #0xf
    sub x0, x0, #240
#if defined(__APPLE__)
    str x18, [x0, #0]
#endif
    str x1, [x0, #16]      // x19: entry
    str x2, [x0, #24]      // x20: argument
    str xzr, [x0, #96]     // x29
#if defined(__APPLE__)
    adrp x3, SYM(mkw_co_entry_trampoline)@PAGE
    add x3, x3, SYM(mkw_co_entry_trampoline)@PAGEOFF
#else
    adrp x3, SYM(mkw_co_entry_trampoline)
    add x3, x3, :lo12:SYM(mkw_co_entry_trampoline)
#endif
    str x3, [x0, #104]     // x30: first switch "returns" into the trampoline
    ret
#if !defined(__APPLE__)
    .size SYM(mkw_co_init), . - SYM(mkw_co_init)
#endif

#if !defined(__APPLE__)
    .type SYM(mkw_co_entry_trampoline), %function
#endif
SYM(mkw_co_entry_trampoline):
    mov x0, x20
    blr x19
    brk #0
#if !defined(__APPLE__)
    .size SYM(mkw_co_entry_trampoline), . - SYM(mkw_co_entry_trampoline)
#endif

#if !defined(__APPLE__)
    .section .note.GNU-stack, "", %progbits
#endif
