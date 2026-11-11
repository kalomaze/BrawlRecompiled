// Ported from Brawl Online gp-v29.json (GPL-2.0-or-later).
static const char* kHeaps[] = {"System","Effect","Fighter1Instance","Fighter2Instance","Fighter3Instance","Fighter4Instance","FighterTechqniq","StageInstance","ItemInstance","WeaponInstance","EnemyInstance","Physics","InfoInstance","GameGlobal","GlobalMode","FighterEffect","Fighter1Resoruce","Fighter2Resoruce","Fighter3Resoruce","Fighter4Resoruce","Fighter1Resoruce2","Fighter2Resoruce2","Fighter3Resoruce2","Fighter4Resoruce2","StageResoruce","IteamResource","PokemonResource","AssistFigureResource","ItemExtraResource","WiiPad","Replay","Tmp","MenuInstance","OverlayFighter1","OverlayFighter2","OverlayFighter3","OverlayFighter4","FighterKirbyResource1","FighterKirbyResource2","FighterKirbyResource3","CommonResource"};
static const Range kAdded[] = {
    {0x805A00B8u, 0x8u}, // g_mtRand
    {0x805A0420u, 0x8u}, // g_mtRandOther
    {0x8059FF58u, 0x4u}, // libc rand next
    {0x804977B4u, 0xCu}, // g_mtPrngLogManager
    {0x807824d4u, 0x4u}, // P+ code-embedded variable in sora_melee .text
    {0x80494f20u, 0x40u}, // dolw-g1
    {0x80494fa0u, 0x20u}, // dolw-g1
    {0x8049e020u, 0x40u}, // dolw-g1
    {0x8049ede0u, 0x60u}, // dolw-g1
    {0x804a3de0u, 0x60u}, // dolw-g1
    {0x804a3e60u, 0x20u}, // dolw-g1
    {0x804a3ec0u, 0x40u}, // dolw-g1
    {0x804a3f20u, 0x20u}, // dolw-g1
    {0x804a3f60u, 0x20u}, // dolw-g1
    {0x804a3fe0u, 0x20u}, // dolw-g1
    {0x804a4580u, 0x40u}, // dolw-g1
    {0x804a4680u, 0x20u}, // dolw-g1
    {0x804a56c0u, 0x20u}, // dolw-g1
    {0x804a5700u, 0x20u}, // dolw-g1
    {0x804a5740u, 0x20u}, // dolw-g1
    {0x804a7f40u, 0x40u}, // dolw-g1
    {0x804a9a80u, 0xb20u}, // dolw-g1
    {0x804aa640u, 0x40u}, // dolw-g1
    {0x804aa6e0u, 0x40u}, // dolw-g1
    {0x804aa740u, 0x2e0u}, // dolw-g1
    {0x804adb20u, 0x140u}, // dolw-g1
    {0x804adc80u, 0x100u}, // dolw-g1
    {0x804afa80u, 0x20u}, // dolw-g1
    {0x805BAF04u, 0x700u}, // gfPadSystem debug/game/menu/merged pads (input history, main thread)
    {0x805BB608u, 0x238u}, // gfPadSystem updateSystem bookkeeping (main thread)
    {0x805BBFC0u, 0x100u}, // controller config/ipSwitch pad struct (0x805BC060, main thread)
    {0x805A0000u, 0x40u}, // .sbss main-thread globals
    {0x805A0100u, 0x80u}, // .sbss main-thread globals
    {0x805A0280u, 0x80u}, // .sbss main-thread globals
    {0x805A0340u, 0x40u}, // .sbss main-thread globals
    {0x805A03C0u, 0x40u}, // .sbss main-thread globals
    {0x8059C480u, 0x40u}, // .sdata main-thread globals (gameProc)
    {0x8059E900u, 0x40u}, // .sdata main-thread globals
    {0x805B8A00u, 0x140u}, // gfTaskScheduler (list heads, pending list)
    {0x805B6200u, 0x200u}, // AreaManager gfTask and the pad manager object owning WiiPad's lists (System FW)
    {0x805B4E80u, 0x2E0u}, // main thread stack above the game loop (outer frames)
    {0x935F0000u, 0x2000u}, // P+ per-port records above the heaps (input buffers, counters; stride 0x880, 4 ports)
    {0x805A7400u, 0x2000u}, // P+ scratch at the bottom of the boot stack (pad struct 0x805A9338)
    {0x805B6D20u, 0x740u}, // gfCameraManager (System FW): cameras the game logic reads
    {0x804953B0u, 0x20u}, // gfSlowManager: s_gfSlowManager requests (.bss)
    {0x805A0080u, 0x8u}, // gfSlowManager: s_needsUpdate (.sbss)
    {0x8059C690u, 0x8u}, // gfSlowManager: s_maxSlowRate (.sdata)
    {0x8049E570u, 0x30u}, // ground collision object lists (grCollision: lbl_8049E570, g_grCollisionList, lbl_8049E588, lbl_8049E594)
    {0x8049EE40u, 0x20u}, // tail of the 0x14-byte record array at 0x8049EDE0 (lbl_8049EDD8; records point into the Effect heap); dolw-g1 covered only up to 0x8049EE40
    {0x805B69E0u, 0x280u}, // camera quake controller (System FW, *0x805A0280): quake flag +0x270, amplitude +0x268
    {0x8049DEDCu, 0xCu}, // camera subject list head (cmSubjectList: count, first, last)
    {0x805BF860u, 0x5760u}, // file IO manager: request queues and request pool (System FW)
    {0x805CA1C0u, 0xA0u}, // file IO manager (gfFileIOManager, System FW)
    {0x805BB860u, 0x20u}, // gfArchiveManager (System FW, *0x8059FFA0): loaded archive list
    {0x80494E00u, 0x3C0u}, // scene/model render records (lbl_80494E00, lbl_80494EE8): pointers into fighter resource and instance heaps
    {0x805B7480u, 0x1E0u}, // controller configuration and per-port stick state (System FW, *0x805A00C8)
    {0x8049EDD8u, 0x5068u}, // nw4r::ef EffectSystem (lbl_8049EDD8): effect/emitter/particle lists and pools
    {0x804A3E70u, 0x20u}, // nw4r::ef resource list (lbl_804A3E70)
    {0x80584080u, 0x20u}, // P+ hold-to-pause: START hold timers (+8 * controller) and the pause-off word (+4)
    {0x80588000u, 0x4C0u}, // P+ All-Star Versus rosters (+0xA0 * port), 0x805882F4, their CSS twin (0x80588300 + 0x70 * port)
    {0x800030C8u, 0x8u}, // OS module list head and tail (OSLink / OSUnlink of a fighter module)
    {0x805BC0C0u, 0x3C8u}, // gfModuleManager (System FW): 16 module entries {gfModule*, file handles, count, flags, name}
};
static const Range kExcluded[] = {
    {0x80611F40u, 0x20u}, // GX FIFO ring tail (RenderFifo heap, below the System heap)
    {0x805B7660u, 0x20u}, // bottom of the thread stack after the controller configuration
    {0x804A3E90u, 0x578u}, // nw4r::ef draw strategies (gp-v13): a granule is shared with lbl_804A3E70
    {0x805B5034u, 0x20u}, // gfFrameBuffer's slot bookkeeping (gfApplication +0x5C): written by GX's draw-done and VI's retrace callbacks
};
static const Range kRemoved[] = {
    {0x804A3E90u, 0x578u}, // nw4r::ef draw strategies (function-local statics, guards in .sbss outside the set)
};
