#pragma once

#include "settings_overlay.h"
#include "runtime_config.h"
#include "display_settings.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/gx/GXAurora.h>

#include <cstdio>
#include <cstdlib>
#include <chrono>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace WindowPlacementPersistence {
inline bool sizeDirty = false;
inline bool positionDirty = false;
inline uint32_t width = 0;
inline uint32_t height = 0;
inline int32_t x = 0;
inline int32_t y = 0;
inline std::chrono::steady_clock::time_point changedAt{};

inline void Flush(bool force = false) {
    if (!sizeDirty && !positionDirty) {
        return;
    }
    constexpr auto kSaveDelay = std::chrono::milliseconds(300);
    if (!force && std::chrono::steady_clock::now() - changedAt < kSaveDelay) {
        return;
    }
    if (sizeDirty && width != 0 && height != 0) {
        RuntimeConfigFile::SetWindowSize(width, height);
    }
    if (positionDirty) {
        RuntimeConfigFile::SetWindowPosition(x, y);
    }
    sizeDirty = false;
    positionDirty = false;
}
} // namespace WindowPlacementPersistence

// The close event can be consumed while execution is inside a guest fiber
// (for example, OSSleepThread).  Running normal C++/CRT shutdown from that
// fiber re-enters runtime teardown and can fault while the fiber machinery is
// still active.  A window close is an intentional successful exit, so end the
// process directly and do not run the crash/atexit paths.
namespace SdCard { void Shutdown(); }  // hle/storage/sd_card.h

[[noreturn]] inline void ExitForAuroraWindowClose() noexcept {
    settings_overlay::ReleaseControllers();
    WindowPlacementPersistence::Flush(true);
    // What the game saved on the SD card (a mod's settings, replays) goes back into UserData/SD: the atexit handler
    // that does it never runs on this path.
    SdCard::Shutdown();
#if defined(_WIN32)
    // Not ExitProcess: the engine is a DLL, and ExitProcess runs its static destructors after
    // the other threads are gone; a still-joinable std::thread then calls std::terminate and the fatal popup hangs
    // the close (issue #38).
    std::fflush(nullptr);
    ::TerminateProcess(::GetCurrentProcess(), 0);
    std::_Exit(EXIT_SUCCESS);
#else
    std::_Exit(EXIT_SUCCESS);
#endif
}

// Update cached Aurora window/framebuffer dimensions based on pending events.
inline void ProcessAuroraEvents(const AuroraEvent* events) {
    if (!events) {
        return;
    }

    for (const AuroraEvent* event = events; event->type != AURORA_NONE; ++event) {
        switch (event->type) {
        case AURORA_WINDOW_MOVED:
            if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_WINDOWED) {
                WindowPlacementPersistence::x = event->windowPos.x;
                WindowPlacementPersistence::y = event->windowPos.y;
                WindowPlacementPersistence::positionDirty = true;
                WindowPlacementPersistence::changedAt = std::chrono::steady_clock::now();
            }
            break;
        case AURORA_WINDOW_RESIZED:
            if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_WINDOWED &&
                event->windowSize.width != 0 && event->windowSize.height != 0) {
                WindowPlacementPersistence::width = event->windowSize.width;
                WindowPlacementPersistence::height = event->windowSize.height;
                WindowPlacementPersistence::sizeDirty = true;
                WindowPlacementPersistence::changedAt = std::chrono::steady_clock::now();
            }
            break;
        case AURORA_EXIT:
            ExitForAuroraWindowClose();
        default:
            break;
        }
    }

    WindowPlacementPersistence::Flush();

    settings_overlay::HandleEvents(events);
}

inline bool BeginAuroraFrame() {
    if (!aurora_begin_frame()) {
        return false;
    }
    // Apply only after begin_frame: changing viewport policy during event dispatch
    // can deadlock the asynchronous frame worker. Re-read every frame because the
    // OS can resize a window without delivering a resize event.
    uint32_t surfaceWidth = 0;
    uint32_t surfaceHeight = 0;
    AuroraGetSurfaceSize(&surfaceWidth, &surfaceHeight);
    DisplaySettings::OnFrameBoundary(surfaceWidth, surfaceHeight);
    return true;
}

// Poll Aurora events and update cached window/framebuffer dimensions.
inline void UpdateAuroraAndProcessEvents() {
    ProcessAuroraEvents(aurora_update());
}
