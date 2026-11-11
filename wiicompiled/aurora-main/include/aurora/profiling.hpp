#pragma once

// Disabled profiling must not require the Tracy source tree or evaluate its arguments.
#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#else
#define ZoneScoped
#define ZoneScopedN(name)
#define ZoneScopedS(depth)
#define TracyPlot(name, value)
#define TracyPlotConfig(name, format, step, fill, color)
#define FrameMarkNamed(name)
#endif
