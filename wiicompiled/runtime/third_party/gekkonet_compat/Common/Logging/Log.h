#pragma once
// GekkoNet's Brawl Online fork adds six Dolphin log calls. Preserve diagnostics
// without bringing the emulator's logging framework into the recomp runtime.
#include <iostream>
namespace BrawlGekkoLog {
template<class... Args> void Write(const char* message,const Args&... args) {
    std::cerr << "[gekkonet] " << message;
    ((std::cerr << " " << args),...);
    std::cerr << '\n';
}
}
#define INFO_LOG_FMT(category, ...) BrawlGekkoLog::Write(__VA_ARGS__)
#define WARN_LOG_FMT(category, ...) BrawlGekkoLog::Write(__VA_ARGS__)
