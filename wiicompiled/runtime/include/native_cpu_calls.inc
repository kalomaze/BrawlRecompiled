// Native call catalog used by InvokeDirectCpu. Keep project-specific HLE
// address mappings here; abi_bridge.h contains only the generic ABI machinery.
//
// This is a .inc, not a header: it has no includes of its own and references
// CpuContext, KnownTypedNativeCpuCall<> and KnownNativeCpuCall<>, all of which
// are declared above its single mid-file include site in abi_bridge.h. It
// cannot compile standalone and must never be included anywhere else.

extern "C" void nw4r__lyt__detail__DrawQuad_800847c0(CpuContext* ctx);
extern "C" void nw4r__lyt__detail__DrawQuad_80084d20(CpuContext* ctx);

extern "C" void GX__SetVtxDesc_HLE(uint32_t a, uint32_t t);
extern "C" void GX__ClearVtxDesc_HLE();
extern "C" void GX__SetVtxAttrFmt_HLE(uint32_t vf, uint32_t a, uint32_t c, uint32_t t, uint32_t fr);
extern "C" void GX__SetTexCoordGen2_HLE(uint32_t dc, uint32_t f, uint32_t sp, uint32_t m, uint32_t n, uint32_t pm);
extern "C" void GX__SetNumTexGens_HLE(uint32_t n);
extern "C" void GX__DrawDone_HLE();
extern "C" void GX__Begin_HLE(uint32_t t, uint32_t vf, uint32_t nv);
extern "C" void GX__SetLineWidth_HLE(uint32_t width, uint32_t texOffsets);
extern "C" void GX__SetPointSize_HLE(uint32_t pointSize, uint32_t texOffsets);
extern "C" void GX__SetCullMode_HLE(uint32_t m);
extern "C" void GX__SetNumChans_HLE(uint32_t n);
extern "C" void GX__SetChanCtrl_HLE(uint32_t ch, uint32_t en, uint32_t as, uint32_t ms, uint32_t lm, uint32_t df, uint32_t af);
extern "C" void GX__InitTexObj_HLE(uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m);
extern "C" void GX__InitTexObjLOD_HLE(uint32_t oa, uint32_t mif, uint32_t maf, float mil, float mal, float lb, uint32_t bc, uint32_t el, uint32_t ma);
extern "C" void GX__InvalidateTexAll_HLE();
extern "C" void GX__LoadTexObj_HLE(uint32_t oa, uint32_t tid);
extern "C" void GX__SetNumIndStages_HLE(uint32_t n);
extern "C" void GX__SetTevDirect_HLE(uint32_t s);
extern "C" void GX__SetTevColorIn_HLE(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
extern "C" void GX__SetTevAlphaIn_HLE(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
extern "C" void GX__SetTevColorOp_HLE(uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_);
extern "C" void GX__SetTevAlphaOp_HLE(uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_);
extern "C" void GX__SetTevColorS10_HLE(uint32_t id, uint32_t cp);
extern "C" void GX__SetTevKColor_HLE(uint32_t id, uint32_t cp);
extern "C" void GX__SetTevKColorSel_HLE(uint32_t s, uint32_t sel);
extern "C" void GX__SetTevKAlphaSel_HLE(uint32_t s, uint32_t sel);
extern "C" void GX__SetTevSwapMode_HLE(uint32_t s, uint32_t rs, uint32_t ts);
extern "C" void GX__SetTevSwapModeTable_HLE(uint32_t id, uint32_t r, uint32_t g, uint32_t b, uint32_t a);
extern "C" void GX__SetTevOrder_HLE(uint32_t s, uint32_t c, uint32_t m, uint32_t col);
extern "C" void GX__SetNumTevStages_HLE(uint32_t n);
extern "C" void GX__SetBlendMode_HLE(uint32_t t, uint32_t s, uint32_t d, uint32_t op);
extern "C" void GX__LoadPosMtxImm_HLE(uint32_t ma, uint32_t id);
extern "C" void GX__SetCurrentMtx_HLE(uint32_t id);

#define MKW_KNOWN_TYPED_NATIVE(addr, fn, ...) \
template <> struct KnownTypedNativeCpuCall<addr> { \
    static constexpr bool kAvailable = true; \
    static void Invoke(CpuContext* cpu) { (void)cpu; fn(__VA_ARGS__); } \
}

MKW_KNOWN_TYPED_NATIVE(0x801EF280u, GX__SetVtxDesc_HLE, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x801EFB10u, GX__ClearVtxDesc_HLE);
MKW_KNOWN_TYPED_NATIVE(0x801EFB44u, GX__SetVtxAttrFmt_HLE, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(0x801F0258u, GX__SetTexCoordGen2_HLE, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_TYPED_NATIVE(0x801F0480u, GX__SetNumTexGens_HLE, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x801F0A30u, GX__DrawDone_HLE);
MKW_KNOWN_TYPED_NATIVE(0x801F1088u, GX__Begin_HLE, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x801F12ACu, GX__SetLineWidth_HLE, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x801F12FCu, GX__SetPointSize_HLE, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x801F136Cu, GX__SetCullMode_HLE, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x801F2644u, GX__SetNumChans_HLE, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x801F2668u, GX__SetChanCtrl_HLE, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9]);
MKW_KNOWN_TYPED_NATIVE(0x801F28F0u, GX__InitTexObj_HLE, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9], cpu->gpr[10]);
MKW_KNOWN_TYPED_NATIVE(0x801F2B44u, GX__InitTexObjLOD_HLE, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d), cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_TYPED_NATIVE(0x801F3194u, GX__InvalidateTexAll_HLE);
MKW_KNOWN_TYPED_NATIVE(0x801F2FB0u, GX__LoadTexObj_HLE, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x801F3ADCu, GX__SetNumIndStages_HLE, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x801F3AFCu, GX__SetTevDirect_HLE, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x801F3C30u, GX__SetTevColorIn_HLE, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(0x801F3C70u, GX__SetTevAlphaIn_HLE, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(0x801F3CB0u, GX__SetTevColorOp_HLE, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_TYPED_NATIVE(0x801F3D08u, GX__SetTevAlphaOp_HLE, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_TYPED_NATIVE(0x801F3DC0u, GX__SetTevColorS10_HLE, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x801F3E24u, GX__SetTevKColor_HLE, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x801F3E80u, GX__SetTevKColorSel_HLE, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x801F3ED0u, GX__SetTevKAlphaSel_HLE, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x801F3F20u, GX__SetTevSwapMode_HLE, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x801F3F5Cu, GX__SetTevSwapModeTable_HLE, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(0x801F409Cu, GX__SetTevOrder_HLE, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_TYPED_NATIVE(0x801F41F8u, GX__SetNumTevStages_HLE, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x801F46CCu, GX__SetBlendMode_HLE, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_TYPED_NATIVE(0x801F51DCu, GX__LoadPosMtxImm_HLE, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x801F52E4u, GX__SetCurrentMtx_HLE, cpu->gpr[3]);

#undef MKW_KNOWN_TYPED_NATIVE

#define MKW_KNOWN_NATIVE_CPU_CALL(addr, fn) \
template <> struct KnownNativeCpuCall<addr> { \
    static constexpr bool kAvailable = true; \
    static constexpr uint32_t kNonvolatileFprWriteMask = 0; \
    static constexpr void (*Entry)(CpuContext*) = &fn; \
}


#undef MKW_KNOWN_NATIVE_CPU_CALL
