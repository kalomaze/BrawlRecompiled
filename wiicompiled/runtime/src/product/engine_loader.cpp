// Windows launcher for Brawl's engine_usa.dll and optional engine_usa_v2.dll.
#include <windows.h>
#include <cpuid.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path ExeDirectory() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return fs::path(exe).parent_path();
}

} // namespace

// The usual engine needs x86-64-v3 (AVX2, FMA, BMI2...); older processors get engine_<region>_v2.dll when it is there.
bool HostHasV3() {
    unsigned a = 0, b = 0, c = 0, d = 0;
    __cpuid_count(1, 0, a, b, c, d);
    const bool fma = (c >> 12) & 1, osxsave = (c >> 27) & 1, avx = (c >> 28) & 1, movbe = (c >> 22) & 1, f16c = (c >> 29) & 1;
    if (!(fma && osxsave && avx && movbe && f16c)) return false;
    unsigned lo = 0, hi = 0;
    __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 6u) != 6u) return false;
    __cpuid_count(7, 0, a, b, c, d);
    return ((b >> 5) & 1) && ((b >> 3) & 1) && ((b >> 8) & 1);  // AVX2, BMI1, BMI2
}

int main(int argc, char** argv) {
    const fs::path exeDir = ExeDirectory();
    fs::path module = exeDir / "engine_usa.dll";
    const fs::path oldCpu = exeDir / "engine_usa_v2.dll";
    std::error_code ec;
    if ((std::getenv("BRAWL_ENGINE_V2") != nullptr || !HostHasV3()) && fs::exists(oldCpu, ec)) module = oldCpu;
    HMODULE engine = LoadLibraryExW(module.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    auto entry = engine ? reinterpret_cast<int (*)(int, char**)>(GetProcAddress(engine, "SDL_main")) : nullptr;
    if (!entry) {
        const std::wstring message = L"The game engine could not be loaded:\n" + module.wstring() +
                                     L"\n(error " + std::to_wstring(GetLastError()) + L")\n\nUnzip the whole download again.";
        MessageBoxW(nullptr, message.c_str(), L"Super Smash Bros. Brawl", MB_OK | MB_ICONERROR);
        return 1;
    }
    return entry(argc, argv);
}
