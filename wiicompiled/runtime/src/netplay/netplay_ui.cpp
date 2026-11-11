#include "netplay_ui.h"

#include "frame_input.h"
#include "wii_remote_input.h"
#include "netplay/game_layout.h"
#include "netplay_lobby.h"
#include "netplay_session.h"
#include "netplay_start.h"
#include "runtime_log.h"
#include "aurora_events.h"
#include "hle_stubs.h"
#include "settings_overlay.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdlib>
#include <string>

// Defined in hle/vi.cpp: whether an aurora frame is open for drawing.
extern std::atomic_bool g_auroraFrameActive;
void PAD_HLE_SampleLocalPads(FrameInput::GcPad* pads);

namespace NetplayUi {
namespace {

using Phase = NetplayLobby::Phase;
constexpr uint32_t kTwo = 0x0100, kA = 0x0800;

std::string SlotName(int slot) {
    const auto& names = NetplayStart::SessionNames();
    return slot >= 0 && slot < static_cast<int>(names.size()) ? names[slot] : "player " + std::to_string(slot + 1);
}

// What the session shows: who the game is waiting for, a desync warning, and the way out when a
// player is gone (the game can't go on without them).
void DrawSession(ImDrawList* list) {
    const NetplaySession::Status status = NetplaySession::GetStatus();
    if (!status.active) {
        return;
    }
    const GameLayout::View view = GameLayout::GameView();
    // A status line in a bar near the top, in the overlay's own font (Brawl has none of NSMBW's menu fonts).
    const auto banner = [&](const std::string& text) {
        const float y = 190.0f, size = 26.0f * view.scale;
        list->AddRectFilled(ImVec2(view.centerX - 290 * view.scale, view.centerY - (y + 20) * view.scale),
                            ImVec2(view.centerX + 290 * view.scale, view.centerY - (y - 20) * view.scale),
                            IM_COL32(20, 40, 90, 180), 10 * view.scale);
        const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
        list->AddText(ImGui::GetFont(), size, ImVec2(view.centerX - extent.x * 0.5f, view.centerY - y * view.scale - extent.y * 0.5f),
                      IM_COL32(255, 255, 255, 255), text.c_str());
    };
    if (status.lostSlot >= 0) {
        // The game is frozen: read the controller directly (A on a GameCube pad or a Wii Remote, 2 too).
        static uint32_t prevHold = ~0u;
        WiiRemoteInput::KpadSample sample;
        uint32_t hold = 0;
        if (WiiRemoteInput::ReadKpadSample(0, sample)) {
            hold = sample.hold;
        }
        FrameInput::GcPad pads[4]{};
        PAD_HLE_SampleLocalPads(pads);
        if (pads[0].button & 0x0100u) {
            hold |= kA;
        }
        const bool confirm = prevHold != ~0u && (hold & ~prevHold & (kTwo | kA)) != 0;
        prevHold = hold;
        banner("Lost the connection to " + SlotName(status.lostSlot) + ". Press A to leave.");
        if (confirm) {
            NetplayStart::EndSession();
        }
        return;
    }
    std::string line;
    if (status.waitingForSlot >= 0 && status.waitedMs > 700) {
        line = "Waiting for " + SlotName(status.waitingForSlot) + "...";
    } else if (status.desyncFrame != 0) {
        line = "The game got out of sync between devices.";
    }
    if (line.empty()) {
        return;
    }
    banner(line);
}

// NSMBW_LOBBY_TEST (Android: debug.nsmbw.lobbytest) drives the lobby without the menus, to test
// rooms between devices: "host" opens a room for 2 and starts it once someone joins, "join" joins
// the first room it finds. The session then starts as it does from the menus.
void LobbyTest() {
    static const std::string mode = [] {
        const char* value = std::getenv("NSMBW_LOBBY_TEST");
        return value != nullptr ? std::string(value) : std::string();
    }();
    if (mode != "host" && mode != "join") {
        return;
    }
    static bool begun = false;
    if (!begun) {
        begun = true;
        RT_LOGF(RT_TAG_RUNTIME, "netplay test: %s\n", mode.c_str());
        if (mode == "host") {
            NetplayLobby::Host(2, 0);
        } else {
            NetplayLobby::Browse();
        }
        return;
    }
    const NetplayLobby::Snapshot lobby = NetplayLobby::Get();
    if (mode == "host" && lobby.phase == Phase::Hosting && lobby.members.size() >= 2) {
        NetplayLobby::Start();
    }
    if (mode == "join" && lobby.phase == Phase::Browsing && !lobby.rooms.empty()) {
        const auto& room = lobby.rooms.front();
        RT_LOGF(RT_TAG_RUNTIME, "netplay test: joining '%s' at %s%s\n", room.name.c_str(), room.address.c_str(),
                room.tailscale ? " (Tailscale)" : "");
        NetplayLobby::Join(room.id);
    }
    static std::string lastMessage;
    if (lobby.phase == Phase::Failed && lobby.message != lastMessage) {
        lastMessage = lobby.message;
        RT_LOGF(RT_TAG_RUNTIME, "netplay test: failed: %s\n", lobby.message.c_str());
    }
}

// NSMBW_DUMP_LAYOUT=<arc>;<arc>...: log every pane's position in those layouts once (finding
// where the game's own buttons are).
void DumpLayouts() {
    static bool done = false;
    const char* value = std::getenv("NSMBW_DUMP_LAYOUT");
    if (done || value == nullptr) {
        return;
    }
    done = true;
    std::string all = value;
    for (size_t pos = 0; pos < all.size();) {
        const size_t end = std::min(all.find(';', pos), all.size());
        const std::string arc = all.substr(pos, end - pos);
        pos = end + 1;
        GameLayout::Layout layout;
        if (!layout.Load(arc)) {
            RT_LOGF(RT_TAG_RUNTIME, "layout dump: can't load %s\n", arc.c_str());
            continue;
        }
        for (const auto& name : layout.PaneNames()) {
            float x = 0, y = 0, w = 0, h = 0;
            if (layout.PaneWorldRect(name, x, y, w, h)) {
                RT_LOGF(RT_TAG_RUNTIME, "layout %s %s: x %.0f y %.0f w %.0f h %.0f\n", arc.c_str(), name.c_str(), x, y, w, h);
            }
        }
    }
}

} // namespace

void OnSessionStall() {
    // At most ~30 presents a second: enough for the banner, cheap while nothing moves.
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(33)) {
        return;
    }
    last = now;
    UpdateAuroraAndProcessEvents();
    if (g_auroraFrameActive.load(std::memory_order_acquire)) {
        settings_overlay::Draw();
        VI_HLE_PresentFrame(/*presentedXfb=*/false, /*paceToRetrace=*/false);
    }
}

void Draw() {
    DumpLayouts();
    if (NetplaySession::Active()) {
        DrawSession(ImGui::GetBackgroundDrawList());
        return;
    }
    LobbyTest();
    // Brawl's online menu owns rooms; a ready plan restarts into the session.
    if (const NetplayLobby::Snapshot lobby = NetplayLobby::Get(); lobby.phase == Phase::Restarting) {
        NetplayLobby::Plan plan;
        if (NetplayLobby::TakePlan(plan)) {
            if (!NetplayStart::Restart(plan)) {
                NetplayLobby::Stop();
                RT_LOGF(RT_TAG_RUNTIME, "netplay: couldn't start the session on this device\n");
            }
        }
    }
}

} // namespace NetplayUi
