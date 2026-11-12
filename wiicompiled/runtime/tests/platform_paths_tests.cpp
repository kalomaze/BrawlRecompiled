#include "platform/host_platform.h"

#include <iostream>

int main() {
#if !defined(__ANDROID__)
    const auto executable = RuntimePlatform::ExecutablePath();
    if (!executable || !executable->is_absolute() || !std::filesystem::is_regular_file(*executable) ||
        RuntimePlatform::ExecutableDirectory() != executable->parent_path()) {
        std::cerr << "executable path is missing or inconsistent with its directory\n";
        return 1;
    }
#endif
    if (!RuntimePlatform::ExecutableDirectory()) {
        std::cerr << "unable to resolve the current executable directory\n";
        return 1;
    }

    const auto userData = RuntimePlatform::ApplicationDataDirectory("WiiCompiledPlatformPathsTest");
    if (userData.filename() != "WiiCompiledPlatformPathsTest") {
        std::cerr << "application-data directory lost its application name: " << userData << '\n';
        return 1;
    }
    if (RuntimePlatform::LogDirectory("WiiCompiledPlatformPathsTest") != userData / "Logs") {
        std::cerr << "log directory is not derived from application data\n";
        return 1;
    }
#if defined(__APPLE__)
    if (userData.parent_path().filename() != "Application Support" ||
        userData.parent_path().parent_path().filename() != "Library") {
        std::cerr << "macOS application-data directory is not under ~/Library/Application Support: "
                  << userData << '\n';
        return 1;
    }
#endif

    return 0;
}
