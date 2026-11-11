// gx_egg.cpp - EGG Library Helper Stubs
#include "gx_internal.h"

// Vertex helper declarations not used by the direct-call catalog.
extern "C" void GX__SetArray_HLE(uint32_t a, uint32_t ba, uint32_t str);
extern "C" void GX_HLE_FIFO_WriteFloat(float val);
extern "C" void GX_HLE_FIFO_Write8(uint8_t val);

// ============================================================================
// EGG::DrawGX Functions
// ============================================================================

// Use translated implementations for the main DrawGX setup routines.
// Keep HLE fallbacks only for known missing alias entry points.

extern "C" void EGG__LightTexture__SetupTevFinish_HLE_8022e2bc(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    const uint32_t stageCount = Memory::Read8(self + 0x75);
    const uint32_t tevCount = Memory::Read16(self + 0x78);

    if (stageCount != 0) {
        uint32_t remainder = tevCount % stageCount;
        if (remainder > 0) {
            const GXColor black{0, 0, 0, 255};
            while (remainder < stageCount) {
                GXSetTevColor(static_cast<GXTevRegID>(remainder + 1), black);
                GXSetTevKColor(static_cast<GXTevKColorID>(remainder), black);
                ++remainder;
            }

            const uint32_t mode = Memory::Read32(self + 0x44);
            const float origin = Memory::ReadFloat32(ctx->gpr[2] + ((mode == 2) ? -25004 : -25040));
            const float span = Memory::ReadFloat32(ctx->gpr[2] + -25024);
            const float end = static_cast<float>(origin + span);
            const float lower = static_cast<float>(origin - span);

            auto writeVertex = [](uint8_t posIdx, float s, float t) {
                GX_HLE_FIFO_Write8(posIdx);
                GX_HLE_FIFO_WriteFloat(s);
                GX_HLE_FIFO_WriteFloat(t);
            };

            GX__Begin_HLE(GX_QUADS, GX_VTXFMT0, 4);
            writeVertex(0, origin, origin);
            writeVertex(1, origin, lower);
            writeVertex(2, end, lower);
            writeVertex(3, end, origin);
        }
    }

    Memory::Write8(self + 0x74, 2);
}

// ============================================================================
// EGG::AsyncDisplay
// ============================================================================

extern "C" void EGG__AsyncDisplay__endRender_HLE_8020ff9c(CpuContext* ctx) {
    uint32_t p = ctx->gpr[3]; ctx->gpr[3] = p; ctx->lr = 0x8024D9E0;
    InvokeIndirectCpu(0x8024D090u, ctx);
    InvokeIndirectCpu(0x801C51B0u, ctx);
}
