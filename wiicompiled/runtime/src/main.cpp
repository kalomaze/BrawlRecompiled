#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <array>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <crtdbg.h>
#include <windows.h>
#include <mmsystem.h>
#include <dbghelp.h>
#else
#include <signal.h>
#include "host_fault_context.h"
#include <unistd.h>
#endif
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "hle/gecko.h"
#include "save_states.h"
#include "guest_code_patch.h"
#include "hle/storage/sd_card.h"
#include "netplay_ui.h"
#include "netplay_start.h"
#include "netplay_session.h"
#include "frame_input.h"
#include "det_clock.h"
#include "game_folder.h"
#include "abi_bridge.h"
#include "guest_flat_memory.h"
#include "fault_report.h"
#include "gx_guest_write.h"
#include "memory.h"
#include "system_bridge.h"
#include "ppc_runtime.h"
#include "aurora_events.h"
#include "display_settings.h"
#include "graphics_info.h"
#include "wii_remote_input.h"
#include "discord_presence.h"
#include "fiber_manager.h"
#include "hle_stubs.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "recomp_mod_loader.h"
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>

// Defined in `runtime/src/hle/vi.cpp` (used by GX/VI HLE).
extern std::atomic_bool g_auroraFrameActive;
extern "C" int g_gxFrameCount;
extern "C" const char* DVDResolveHostPathForTest(const char* dvdPath);
bool OS_HLE_InterruptsEnabled() noexcept;

namespace {

// Defined below, beside the fatal-log machinery.
std::string FormatHostStackTrace(unsigned framesToSkip = 0);

void ServiceGuestTimingDuringAuroraFrameWait() {
    // Aurora can block inside FIFO drains before control returns to GX HLE, for as long as a
    // whole display period. Keep VI retraces, alarms and audio moving at wall-clock cadence here
    // while still suppressing guest rescheduling and recursive Aurora work.
    VI_HLE_ProcessRetracesDeferred(8);
    OS_HLE_ProcessAlarmsDeferred(8);
    Audio_HLE_PollDeferred();
}

#if defined(_WIN32)
int __cdecl WindowsCrtReportHook(int reportType, char* message, int* returnValue) {
    if (returnValue) {
        *returnValue = 0;
    }

    RT_LOG(RT_TAG_RUNTIME) << "CRT report type=" << reportType;
    if (message) {
        std::cerr << ": " << message;
    } else {
        std::cerr << '\n';
    }

    RT_LOG(RT_TAG_RUNTIME) << "CRT report stack:\n" << FormatHostStackTrace(1);
    std::cerr.flush();
    return TRUE;
}

void ConfigureWindowsFatalDialogBehavior() {
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportHook(WindowsCrtReportHook);
}

// A busy PC (browsers, launchers, sync and antivirus tools) delays the game's threads when they wake
// for a frame, which shows as dropped frames at 60. Run above normal priority, and opt out of
// Windows' power throttling (EcoQoS), which can slow the process down and coarsen its timers.
void RaiseWindowsProcessPriority() {
    if (!::SetPriorityClass(::GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS)) {
        RT_LOG(RT_TAG_RUNTIME) << "SetPriorityClass failed: " << ::GetLastError() << std::endl;
    }
    struct PowerThrottlingState {
        ULONG Version;
        ULONG ControlMask;
        ULONG StateMask;
    };
    constexpr int kProcessPowerThrottling = 4;  // PROCESS_INFORMATION_CLASS::ProcessPowerThrottling
    constexpr ULONG kExecutionSpeed = 0x1, kIgnoreTimerResolution = 0x4;
    using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    const auto setInfo = reinterpret_cast<SetProcessInformationFn>(
        reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation")));
    if (setInfo != nullptr) {
        PowerThrottlingState state{1, kExecutionSpeed | kIgnoreTimerResolution, 0};
        if (!setInfo(::GetCurrentProcess(), kProcessPowerThrottling, &state, sizeof(state))) {
            state.ControlMask = kExecutionSpeed;  // before Windows 11 the timer flag is unknown
            setInfo(::GetCurrentProcess(), kProcessPowerThrottling, &state, sizeof(state));
        }
    }
}

class WindowsTimerResolutionGuard {
public:
    WindowsTimerResolutionGuard() {
        RaiseWindowsProcessPriority();
        const MMRESULT result = ::timeBeginPeriod(1);
        if (result == TIMERR_NOERROR) {
            armed_ = true;
        } else {
            RT_LOG(RT_TAG_RUNTIME) << "timeBeginPeriod(1) failed: " << result << std::endl;
        }
    }

    ~WindowsTimerResolutionGuard() {
        if (armed_) {
            ::timeEndPeriod(1);
        }
    }

    WindowsTimerResolutionGuard(const WindowsTimerResolutionGuard&) = delete;
    WindowsTimerResolutionGuard& operator=(const WindowsTimerResolutionGuard&) = delete;

private:
    bool armed_ = false;
};
#endif
} // namespace

namespace {

std::string g_lastEntryLabel;
std::atomic_bool g_auroraInitialized{false};
std::atomic_flag g_abortSignalHandled = ATOMIC_FLAG_INIT;
std::atomic_bool g_fatalErrorReported{false};
std::atomic_bool g_fatalPopupShown{false};
std::atomic<int> g_lastExitCode{0};
std::atomic_bool g_exitCodeSet{false};

struct ProcessTranscriptState {
    bool enabled = false;
    std::filesystem::path path;
    std::ofstream file;
    std::mutex fileMutex;
    std::atomic_bool initialized{false};
    int savedStdoutFd = -1;
    int savedStderrFd = -1;
    int stdoutPipeReadFd = -1;
    int stdoutPipeWriteFd = -1;
    int stderrPipeReadFd = -1;
    int stderrPipeWriteFd = -1;
    std::thread stdoutThread;
    std::thread stderrThread;
};

std::filesystem::path GetDefaultRuntimeLogDirectory() {
    return RuntimeConfigFile::ApplicationDataDirectory() / "Logs";
}

// Every entry in the Logs root - both the per-run folders written by this
// scheme and any flat .log files left over from the previous one - is removed
// once it is older than the retention window.
void PruneOldRunLogs(const std::filesystem::path& logRoot) {
    constexpr auto kRetention = std::chrono::hours(24 * 4);

    std::error_code ec;
    const auto now = std::filesystem::file_time_type::clock::now();
    for (const auto& entry : std::filesystem::directory_iterator(logRoot, ec)) {
        std::error_code entryEc;
        const auto writeTime = std::filesystem::last_write_time(entry.path(), entryEc);
        if (entryEc) {
            continue;
        }
        if (now - writeTime > kRetention) {
            std::filesystem::remove_all(entry.path(), entryEc);
        }
    }
}

// Logs/<product>_<epochSeconds>_pid<pid>/ - one folder per run. The console
// transcript and every crash artifact for the run land in here, so "zip this
// folder" is a complete diagnostic. Created lazily so even a crash before
// transcript setup still has somewhere to write.
const std::filesystem::path& GetRunLogDirectory() {
    // Never destroyed: a crash log written during exit (std::terminate from a static's destructor) used to read this
    // after its destructor ran and created a file with a garbage name next to the executable.
    static const std::filesystem::path& runDirectory = *new std::filesystem::path([] {
        const std::filesystem::path logRoot = GetDefaultRuntimeLogDirectory();

        std::error_code ec;
        std::filesystem::create_directories(logRoot, ec);
        PruneOldRunLogs(logRoot);

#if defined(_WIN32)
        const unsigned long pid = ::GetCurrentProcessId();
#else
        const auto pid = static_cast<unsigned long>(::getpid());
#endif
        const auto now = std::chrono::system_clock::now();
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

        std::ostringstream name;
        name << "brawl"
             << "_" << secs << "_pid" << pid;
        const std::filesystem::path directory = logRoot / name.str();
        std::filesystem::create_directories(directory, ec);
        return directory;
    }());
    return runDirectory;
}

ProcessTranscriptState& GetProcessTranscriptState() {
    static ProcessTranscriptState state;
    return state;
}

void WriteProcessTranscriptChunk(ProcessTranscriptState& state, const char* data, size_t size) {
    if (!state.enabled || !state.file || size == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(state.fileMutex);
    state.file.write(data, static_cast<std::streamsize>(size));
    state.file.flush();
}

void PumpTranscriptPipe(ProcessTranscriptState& state, int readFd, int mirrorFd) {
    std::array<char, 4096> buffer{};
    for (;;) {
#if defined(_WIN32)
        const int bytesRead = _read(readFd, buffer.data(), static_cast<unsigned int>(buffer.size()));
#else
        const ssize_t bytesRead = ::read(readFd, buffer.data(), buffer.size());
#endif
        if (bytesRead <= 0) {
            break;
        }

        if (mirrorFd >= 0) {
            size_t offset = 0;
            while (offset < static_cast<size_t>(bytesRead)) {
#if defined(_WIN32)
                const int written = _write(mirrorFd,
                                           buffer.data() + offset,
                                           static_cast<unsigned int>(static_cast<size_t>(bytesRead) - offset));
#else
                const ssize_t written = ::write(mirrorFd,
                                                buffer.data() + offset,
                                                static_cast<size_t>(bytesRead) - offset);
#endif
                if (written <= 0) {
                    break;
                }
                offset += static_cast<size_t>(written);
            }
        }

        WriteProcessTranscriptChunk(state, buffer.data(), static_cast<size_t>(bytesRead));
    }
}

#if defined(_WIN32)
int GetFileDescriptor(FILE* file) {
    return _fileno(file);
}

int DuplicateFileDescriptor(int fd) {
    return _dup(fd);
}

int DuplicateFileDescriptorTo(int sourceFd, int targetFd) {
    return _dup2(sourceFd, targetFd);
}

void CloseFileDescriptor(int fd) {
    _close(fd);
}
#else
int GetFileDescriptor(FILE* file) {
    return fileno(file);
}

int DuplicateFileDescriptor(int fd) {
    return ::dup(fd);
}

int DuplicateFileDescriptorTo(int sourceFd, int targetFd) {
    return ::dup2(sourceFd, targetFd);
}

void CloseFileDescriptor(int fd) {
    ::close(fd);
}
#endif

bool InstallTranscriptPipe(int& outReadFd, int& outWriteFd, int targetFd) {
#if defined(_WIN32)
    int pipeFds[2]{-1, -1};
    if (_pipe(pipeFds, 8192, _O_BINARY) != 0) {
        return false;
    }
#else
    int pipeFds[2]{-1, -1};
    if (::pipe(pipeFds) != 0) {
        return false;
    }
#endif

    outReadFd = pipeFds[0];
    outWriteFd = pipeFds[1];
    if (targetFd == GetFileDescriptor(stdout)) {
        std::fflush(stdout);
    } else if (targetFd == GetFileDescriptor(stderr)) {
        std::fflush(stderr);
    }

    if (DuplicateFileDescriptorTo(outWriteFd, targetFd) < 0) {
        CloseFileDescriptor(outReadFd);
        CloseFileDescriptor(outWriteFd);
        outReadFd = -1;
        outWriteFd = -1;
        return false;
    }
    return true;
}

#if defined(_WIN32)
void AttachParentConsoleForDiagnostics() {
    // GUI-subsystem products get no console and no bound stdout/stderr unless a parent already
    // redirected them (pipe/file: leave alone) or has a console to attach to (bind only the
    // streams still unbound). With no parent console, fall back to NUL rather than leaving
    // _fileno(stdout) == -2, which would stop InitializeProcessTranscript from redirecting into
    // the log file at all.
    const HANDLE outHandle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE errHandle = ::GetStdHandle(STD_ERROR_HANDLE);
    const bool haveOut = outHandle != nullptr && outHandle != INVALID_HANDLE_VALUE;
    const bool haveErr = errHandle != nullptr && errHandle != INVALID_HANDLE_VALUE;
    if (haveOut && haveErr) {
        return;
    }

    const bool attached =
        ::GetConsoleWindow() != nullptr || ::AttachConsole(ATTACH_PARENT_PROCESS) != 0;
    const char* const sink = attached ? "CONOUT$" : "NUL";
    if (!haveOut) {
        (void)std::freopen(sink, "w", stdout);
    }
    if (!haveErr) {
        (void)std::freopen(sink, "w", stderr);
    }
    std::cout.clear();
    std::cerr.clear();
}
#else
void AttachParentConsoleForDiagnostics() {}
#endif

// The setup writes build-fingerprint.json beside every product executable; its SetupVersion is
// the only version identity the runtime has (products are compiled locally, so nothing is baked
// into the binary). Surface it at the top of the transcript so every attached log self-identifies.
std::string ReadInstalledSetupVersion() {
    const auto directory = RuntimeConfigFile::ExecutableDirectory();
    if (!directory) {
        return {};
    }
    std::ifstream file(*directory / "build-fingerprint.json", std::ios::binary);
    if (!file) {
        return {};
    }
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    constexpr std::string_view kKey = "\"SetupVersion\"";
    const auto keyPos = text.find(kKey);
    if (keyPos == std::string::npos) {
        return {};
    }
    const auto colon = text.find(':', keyPos + kKey.size());
    const auto open = colon == std::string::npos ? std::string::npos : text.find('"', colon + 1);
    const auto close = open == std::string::npos ? std::string::npos : text.find('"', open + 1);
    if (close == std::string::npos) {
        return {};
    }
    return text.substr(open + 1, close - open - 1);
}

void InitializeProcessTranscript(int argc, char** argv) {
    auto& state = GetProcessTranscriptState();
    if (state.initialized.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    const std::filesystem::path path = GetRunLogDirectory() / "console.log";
    state.file.open(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!state.file) {
        return;
    }

    state.enabled = true;
    state.path = path;

#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const auto pid = static_cast<unsigned long>(::getpid());
#endif

    const std::string setupVersion = ReadInstalledSetupVersion();

    {
        std::lock_guard<std::mutex> lock(state.fileMutex);
        state.file << "[runtime] WiiCompiled "
                   << (setupVersion.empty() ? "version unknown" : setupVersion) << "\n";
        state.file << "[runtime] process transcript started\n";
        state.file << "[runtime] pid=" << pid << "\n";
        state.file << "[runtime] argv=";
        for (int i = 0; i < argc; ++i) {
            if (i != 0) {
                state.file << ' ';
            }
            state.file << argv[i];
        }
        state.file << "\n";
        state.file.flush();
    }

    state.savedStdoutFd = DuplicateFileDescriptor(GetFileDescriptor(stdout));
    state.savedStderrFd = DuplicateFileDescriptor(GetFileDescriptor(stderr));

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    auto restoreFailedSetup = [&state]() {
        if (state.savedStdoutFd >= 0) {
            DuplicateFileDescriptorTo(state.savedStdoutFd, GetFileDescriptor(stdout));
        }
        if (state.savedStderrFd >= 0) {
            DuplicateFileDescriptorTo(state.savedStderrFd, GetFileDescriptor(stderr));
        }
        if (state.stdoutPipeReadFd >= 0) {
            CloseFileDescriptor(state.stdoutPipeReadFd);
            state.stdoutPipeReadFd = -1;
        }
        if (state.stdoutPipeWriteFd >= 0) {
            CloseFileDescriptor(state.stdoutPipeWriteFd);
            state.stdoutPipeWriteFd = -1;
        }
        if (state.stderrPipeReadFd >= 0) {
            CloseFileDescriptor(state.stderrPipeReadFd);
            state.stderrPipeReadFd = -1;
        }
        if (state.stderrPipeWriteFd >= 0) {
            CloseFileDescriptor(state.stderrPipeWriteFd);
            state.stderrPipeWriteFd = -1;
        }
        if (state.savedStdoutFd >= 0) {
            CloseFileDescriptor(state.savedStdoutFd);
            state.savedStdoutFd = -1;
        }
        if (state.savedStderrFd >= 0) {
            CloseFileDescriptor(state.savedStderrFd);
            state.savedStderrFd = -1;
        }
        state.enabled = false;
        state.file.close();
    };

    if (!InstallTranscriptPipe(state.stdoutPipeReadFd, state.stdoutPipeWriteFd, GetFileDescriptor(stdout)) ||
        !InstallTranscriptPipe(state.stderrPipeReadFd, state.stderrPipeWriteFd, GetFileDescriptor(stderr))) {
        restoreFailedSetup();
        return;
    }

    state.stdoutThread = std::thread([&state]() {
        PumpTranscriptPipe(state, state.stdoutPipeReadFd, state.savedStdoutFd);
    });
    state.stderrThread = std::thread([&state]() {
        PumpTranscriptPipe(state, state.stderrPipeReadFd, state.savedStderrFd);
    });
}

void ShutdownProcessTranscript() {
    auto& state = GetProcessTranscriptState();
    if (!state.enabled) {
        return;
    }

    std::fflush(stdout);
    std::fflush(stderr);
    std::cout.flush();
    std::cerr.flush();

    if (state.savedStdoutFd >= 0) {
        DuplicateFileDescriptorTo(state.savedStdoutFd, GetFileDescriptor(stdout));
    }
    if (state.savedStderrFd >= 0) {
        DuplicateFileDescriptorTo(state.savedStderrFd, GetFileDescriptor(stderr));
    }

    if (state.stdoutPipeWriteFd >= 0) {
        CloseFileDescriptor(state.stdoutPipeWriteFd);
        state.stdoutPipeWriteFd = -1;
    }
    if (state.stderrPipeWriteFd >= 0) {
        CloseFileDescriptor(state.stderrPipeWriteFd);
        state.stderrPipeWriteFd = -1;
    }

    if (state.stdoutThread.joinable()) {
        state.stdoutThread.join();
    }
    if (state.stderrThread.joinable()) {
        state.stderrThread.join();
    }

    if (state.stdoutPipeReadFd >= 0) {
        CloseFileDescriptor(state.stdoutPipeReadFd);
        state.stdoutPipeReadFd = -1;
    }
    if (state.stderrPipeReadFd >= 0) {
        CloseFileDescriptor(state.stderrPipeReadFd);
        state.stderrPipeReadFd = -1;
    }
    if (state.savedStdoutFd >= 0) {
        CloseFileDescriptor(state.savedStdoutFd);
        state.savedStdoutFd = -1;
    }
    if (state.savedStderrFd >= 0) {
        CloseFileDescriptor(state.savedStderrFd);
        state.savedStderrFd = -1;
    }

    {
        std::lock_guard<std::mutex> lock(state.fileMutex);
        state.file << "\n[runtime] process transcript ended\n";
        state.file.flush();
    }
    state.file.close();
    state.enabled = false;
}

// The one host stack walker. Every caller - the CRT report hook, the fatal log
// and the stderr crash dump - goes through this, so the log and the console see
// exactly the same frames, including the TranslatedFunctionRegistry fallback for
// addresses DbgHelp cannot name.
std::string FormatHostStackTrace(unsigned framesToSkip) {
#if defined(_WIN32)
    static std::atomic_bool s_symbolsReady{false};
    HANDLE process = GetCurrentProcess();
    if (!s_symbolsReady.load(std::memory_order_acquire)) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        if (SymInitialize(process, nullptr, TRUE)) {
            s_symbolsReady.store(true, std::memory_order_release);
        }
    }
    const bool symbolsReady = s_symbolsReady.load(std::memory_order_acquire);

    void* frames[64]{};
    const USHORT captured = CaptureStackBackTrace(static_cast<DWORD>(framesToSkip),
                                                  static_cast<DWORD>(std::size(frames)), frames, nullptr);
    std::ostringstream out;
    if (captured == 0) {
        out << "[runtime] host stack trace unavailable (CaptureStackBackTrace returned 0)\n";
        return out.str();
    }
    out << "[runtime] host stack trace (most recent call first):\n";
    for (USHORT i = 0; i < captured; ++i) {
        const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        HMODULE module = nullptr;
        std::string modulePath = "?";
        DWORD64 moduleBase = 0;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(frames[i]),
                               &module) != 0 &&
            module != nullptr) {
            moduleBase = reinterpret_cast<DWORD64>(module);
            std::wstring modulePathBuffer(MAX_PATH, L'\0');
            for (;;) {
                const DWORD length =
                    GetModuleFileNameW(module, modulePathBuffer.data(), static_cast<DWORD>(modulePathBuffer.size()));
                if (length == 0) {
                    break;
                }
                if (length < modulePathBuffer.size()) {
                    modulePathBuffer.resize(length);
                    modulePath = RuntimeConfigFile::PathToUtf8(std::filesystem::path(modulePathBuffer));
                    break;
                }
                // Truncated; retry with a larger buffer up to the extended path limit.
                if (modulePathBuffer.size() >= 32768) {
                    break;
                }
                modulePathBuffer.resize(modulePathBuffer.size() * 2);
            }
        }

        const char* symbolName = "?";
        std::string translatedFuncName;
        uint32_t ppcAddress = 0;
        DWORD64 symbolDisp = 0;
        std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> symbolBuffer{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        if (symbolsReady && SymFromAddr(process, addr, &symbolDisp, symbol)) {
            symbolName = symbol->Name;
        } else if (auto info = TranslatedFunctionRegistry::FindByHostAddress(static_cast<uintptr_t>(addr))) {
            // DbgHelp could not name it; the translated-function registry can.
            translatedFuncName = info->name;
            ppcAddress = info->address;
            if (!translatedFuncName.empty()) {
                symbolName = translatedFuncName.c_str();
                symbolDisp = addr - reinterpret_cast<DWORD64>(info->entryPoint);
            }
        }

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        const char* fileName = nullptr;
        DWORD lineNumber = 0;
        if (symbolsReady && SymGetLineFromAddr64(process, addr, &lineDisp, &line)) {
            fileName = line.FileName;
            lineNumber = line.LineNumber;
        }

        out << "  ["
            << std::setw(2) << std::setfill('0') << static_cast<unsigned>(i)
            << std::setfill(' ') << "] "
            << modulePath << "!" << symbolName
            << " + 0x" << std::hex << std::uppercase << symbolDisp
            << " (0x" << addr;
        if (moduleBase != 0) {
            out << ", module+0x" << (addr - moduleBase);
        }
        if (ppcAddress != 0) {
            out << ", PPC:0x" << std::setw(8) << std::setfill('0') << ppcAddress << std::setfill(' ');
        }
        out << std::dec << std::nouppercase;
        if (fileName) {
            out << ", " << fileName << ":" << lineNumber;
        }
        out << ")\n";
    }
    return out.str();
#else
    (void)framesToSkip;
    return {};
#endif
}

void WriteFatalLogImpl(std::string_view reason, std::string_view extraDetails = {},
                       const uint32_t* missingGuestTarget = nullptr) {
    const std::filesystem::path runDirectory = GetRunLogDirectory();
    std::string fileName = "crash_";
    fileName.append(reason);
    fileName.append(".txt");
    std::ofstream out(runDirectory / fileName, std::ios::out | std::ios::trunc);
    if (!out) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const auto nowSecs = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    out << "[runtime] fatal log" << std::endl;
    out << "[runtime] reason: " << reason << std::endl;
    out << "[runtime] timestamp(seconds): " << nowSecs << std::endl;
    out << "[runtime] entry: " << (g_lastEntryLabel.empty() ? "<unknown>" : g_lastEntryLabel) << std::endl;
    if (!extraDetails.empty()) {
        out << "[runtime] details: " << extraDetails << std::endl;
    }
    if (missingGuestTarget) {
        out << "[runtime] guest jump target: 0x" << std::hex << std::uppercase
            << *missingGuestTarget << std::dec << std::endl;
    }

    const CpuContext* cpu = TryGetCpuContext();
    if (!cpu) {
        // Exceptions can unwind CpuContextScope before we get here.
        // Fall back to the persistent CPU context snapshot so fatal logs still include registers.
        cpu = &GetPersistentCpuContext();
    }
    if (cpu) {
        SystemBridge::DumpCrashHeuristics(out, cpu, missingGuestTarget);
        SystemBridge::DumpCpuState(out, cpu);
    } else {
        out << "[runtime] CPU context unavailable." << std::endl;
    }

    // Guest memory snapshots so the heap/object state can be walked offline.
    // Written once per process: several fatal paths can fire in sequence
    // (e.g. an exception followed by the exit-code report) and the snapshots
    // are large.
    static std::atomic_bool s_memorySnapshotWritten{false};
    if (!s_memorySnapshotWritten.exchange(true, std::memory_order_acq_rel)) {
        SystemBridge::WriteGuestMemorySnapshot(out, runDirectory / "mem1.bin");
    }

    out.flush();
    RT_LOG(RT_TAG_RUNTIME) << "crash artifacts written to "
                           << RuntimeConfigFile::PathToUtf8(runDirectory) << std::endl;
}

void SetRuntimeExitCodeImpl(int code) {
    g_lastExitCode.store(code, std::memory_order_relaxed);
    g_exitCodeSet.store(true, std::memory_order_relaxed);
}

} // namespace

// Before the process replaces itself (a Linux restart into a network session, netplay_start.cpp): the console gets
// its own stdout and stderr back, since the new image keeps those descriptors but not the thread that drained the
// transcript pipe behind them, and would block on its first full pipe.
void RuntimeShutdownProcessTranscript() {
    ShutdownProcessTranscript();
}

namespace {

void DumpHostStackTrace() {
#if defined(_WIN32)
    static std::atomic_flag s_inProgress = ATOMIC_FLAG_INIT;
    if (s_inProgress.test_and_set()) {
        return;
    }
    const std::string trace = FormatHostStackTrace(1);
    std::fputs(trace.c_str(), stderr);
    std::fflush(stderr);
    s_inProgress.clear();
#else
    RT_LOGF(RT_TAG_RUNTIME, "Host stack trace unavailable on this platform\n");
    std::fflush(stderr);
#endif
}

} // namespace

extern "C" void DumpHostStackTraceForRuntimeHelper() {
    DumpHostStackTrace();
}

void MarkFatalErrorReported() {
    g_fatalErrorReported.store(true, std::memory_order_release);
}

void ShowRuntimeFatalPopup(std::string_view category, std::string_view details) noexcept {
    if (g_fatalPopupShown.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // Unattended runs: the log has it; a dialog would only block the exit.
    if (std::getenv("WIICOMPILED_NO_POPUP")) {
        RT_LOGF(RT_TAG_RUNTIME, "fatal %.*s: %.*s\n", static_cast<int>(category.size()), category.data(),
                static_cast<int>(details.size()), details.data());
        return;
    }

    try {
        std::string message;
        message.reserve(category.size() + details.size() + 220);
        message.append("The game stopped because ");
        message.append(category.empty() ? "a fatal error occurred." : category);
        message.append(".\n\n");
        if (details.empty()) {
            message.append("No additional details were available.");
        } else {
            constexpr size_t kMaxPopupDetails = 4096;
            message.append(details.data(), std::min(details.size(), kMaxPopupDetails));
            if (details.size() > kMaxPopupDetails) {
                message.append("\n\n[Additional details were written to the crash log.]");
            }
        }
        message.append("\n\nThe full log of this run is console.log in:\n");
        message.append(RuntimeConfigFile::PathToUtf8(GetRunLogDirectory()));
#if defined(_WIN32)
        ::MessageBoxA(nullptr, message.c_str(), "WiiCompiled - Fatal Error",
                      MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TASKMODAL);
#else
        // Retain the console diagnostic when no native dialog is available.
        RT_LOGF(RT_TAG_RUNTIME, "fatal dialog: %s\n", message.c_str());
#endif
    } catch (...) {
        // Reporting a crash must never throw or mask the original failure.
    }
}

namespace RuntimeCrash {

void WriteCrashArtifacts(std::string_view reason, std::string_view extraDetails,
                         const uint32_t* missingGuestTarget) noexcept {
    try {
        WriteFatalLogImpl(reason, extraDetails, missingGuestTarget);
    } catch (...) {
        // Crash reporting must never mask the original failure.
    }
}

[[noreturn]] void FatalMissingGuestTarget(uint32_t target, CpuContext* cpu) noexcept {
    RT_LOG(RT_TAG_RUNTIME) << "InvokeIndirectCpu: target 0x" << std::hex << target
              << " not translated (missing function)" << std::dec << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "Caller LR = 0x" << std::hex << (cpu ? cpu->lr : 0u)
              << std::dec << std::endl;
    PpcInterpreter::DumpRecentBranches();  // a mod's interpreted code is the usual way here
    try {
        SystemBridge::DumpCrashHeuristics(std::cerr, cpu, &target);
        SystemBridge::DumpCpuState(cpu);
    } catch (...) {
    }
    std::fflush(stderr);

    std::ostringstream message;
    message << "The game stopped because it tried to execute guest address 0x"
            << std::hex << target
            << ", but that function was not translated or registered.\n\n"
            << "Caller LR: 0x" << (cpu ? cpu->lr : 0u);
    if (target == 0) {
        message << "\n\nA jump to address 0 usually means a virtual call through a bad "
                   "object pointer; the crash log heuristics have details.";
    }
    WriteCrashArtifacts("missing_target", message.str(), &target);
    ShowRuntimeFatalPopup("Missing translated function", message.str());
    MarkFatalErrorReported();
    std::exit(EXIT_FAILURE);
}

} // namespace RuntimeCrash


namespace {

void RuntimeAuroraLogCallback(AuroraLogLevel level, const char* module,
                              const char* message, unsigned int len) {
    const std::string_view moduleView = module != nullptr ? std::string_view(module) : std::string_view{};
    const std::string_view messageView = message != nullptr ? std::string_view(message, len) : std::string_view{};
    std::cerr << "[aurora] [" << static_cast<int>(level) << "] [" << moduleView << "] "
              << messageView << std::endl;
    if (level == LOG_FATAL) {
        std::string details(messageView);
        details += "\n\nOn older or integrated graphics, updating the graphics driver from the GPU maker's site and "
                   "lowering Resolution (F10, or resolution_multiplier = 1.0 in UserData/Config.toml) usually helps.";
        ShowRuntimeFatalPopup("Aurora reported a fatal renderer error", details);
    }
}

const TranslatedFunctionInfo* ResolveEntry() {
    const auto* entry = TranslatedFunctionRegistry::FindByAddressPtr(kDefaultEntryAddress);
    if (!entry) {
        std::ostringstream oss;
        oss << "No translated function registered at address 0x" << std::hex << kDefaultEntryAddress;
        throw std::runtime_error(oss.str());
    }
    return entry;
}

void SeedCpuContext(CpuContext& cpu) {
    cpu.gpr[1] = 0x81700000u;
}

void DumpAccessViolationReport(const Memory::AccessViolation& ex,
                               std::string_view entryLabel) {
    const uint32_t address = ex.address();
    const size_t length = ex.length();
    const CpuContext* cpu = TryGetCpuContext();

    RT_LOG(RT_TAG_RUNTIME) << "===== Memory Access Violation =====" << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "Reason : " << ex.reason() << std::endl;
    std::cerr << std::hex << std::uppercase;
    RT_LOG(RT_TAG_RUNTIME) << "Address: 0x" << std::setw(8) << std::setfill('0') << address
              << " (+0x" << length << ")" << std::dec << std::setfill(' ') << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "Entry  : " << (entryLabel.empty() ? "(unknown)" : std::string(entryLabel)) << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "Mode   : strict (trap on unmapped)" << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "r1 seed: 0x81700000" << std::endl;
    if (cpu) {
        RT_LOG(RT_TAG_RUNTIME) << "CurrentCpuContext: " << cpu << "  r1=0x"
                  << std::hex << std::uppercase << cpu->gpr[1] << std::dec << std::nouppercase << std::endl;
        RT_LOG(RT_TAG_RUNTIME) << "Last recorded PC : 0x" << std::hex << std::uppercase << cpu->pc
                  << std::dec << std::nouppercase << std::endl;
    } else {
        RT_LOG(RT_TAG_RUNTIME) << "CurrentCpuContext: (null)" << std::endl;
    }

    SystemBridge::DumpCpuState(cpu);

    const auto regions = Memory::DescribeRegions();
    if (regions.empty()) {
        RT_LOG(RT_TAG_RUNTIME) << "Memory not initialized; no regions mapped." << std::endl;
        return;
    }

    RT_LOG(RT_TAG_RUNTIME) << "Mapped regions:" << std::endl;
    uint64_t bestDistance = std::numeric_limits<uint64_t>::max();
    std::string bestRegion;
    bool insideRegion = false;

    for (const auto& region : regions) {
        const uint64_t base = region.baseAddress;
        const uint64_t end = base + region.sizeBytes;
        const bool contains = address >= base && address < end;
        if (contains) {
            insideRegion = true;
            bestDistance = 0;
            bestRegion = region.name;
        } else {
            const uint64_t distance = address < base ? base - address : address - end + 1;
            if (distance < bestDistance) {
                bestDistance = distance;
                bestRegion = region.name;
            }
        }

        std::cerr << "  - " << region.name
                  << " 0x" << std::hex << std::setw(8) << std::setfill('0') << region.baseAddress
                  << " .. 0x" << std::setw(8) << (end - 1)
                  << std::dec << std::setfill(' ')
                  << " (" << region.sizeBytes / 1024 << " KiB";
        if (contains) {
            std::cerr << ", <-- access landed here";
        }
        std::cerr << ")" << std::endl;
    }

    if (!bestRegion.empty() && !insideRegion) {
        RT_LOG(RT_TAG_RUNTIME) << "Nearest region: " << bestRegion << " (" << bestDistance << " bytes away)" << std::endl;
    }

    RT_LOG(RT_TAG_RUNTIME) << "Verify that the installed game data and runtime build match." << std::endl;
}

#if defined(_WIN32)
constexpr DWORD kCppExceptionCodeGcc = 0x20474343; // "GCC" exception code
constexpr DWORD kCppExceptionCodeMsvc = 0xE06D7363;
// AddressSanitizer uses STATUS_FATAL_APP_EXIT when it detects an error and wants to report it.
// We must let ASan's handler run so it can print file/line information.
constexpr DWORD kAsanFatalAppExit = 0x40000015; // STATUS_FATAL_APP_EXIT
LONG ReportFatalSehAndExit(EXCEPTION_POINTERS* info);

LONG WINAPI UnhandledSehFilter(EXCEPTION_POINTERS* info) {
    if (info == nullptr || info->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == kCppExceptionCodeGcc || code == kCppExceptionCodeMsvc || code == kAsanFatalAppExit) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return ReportFatalSehAndExit(info);
}

LONG ReportFatalSehAndExit(EXCEPTION_POINTERS* info) {
    static std::atomic_flag reporting = ATOMIC_FLAG_INIT;
    if (reporting.test_and_set()) std::_Exit(EXIT_FAILURE);

    // Native faults can interrupt the allocator, streams, or window system.
    // Inspect only the OS exception record; leave stack/guest-state traversal
    // to a debugger rather than risking another failure while reporting.
    FaultReport::Message message;
    message.Text("[runtime] FATAL native Windows exception");
    const auto* record = info ? info->ExceptionRecord : nullptr;
    if (record) {
        message.Text("\n  code: ").Hex(record->ExceptionCode)
            .Text("\n  instruction: ").Hex(reinterpret_cast<uintptr_t>(record->ExceptionAddress), sizeof(uintptr_t) * 2);
        if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
             record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && record->NumberParameters >= 2) {
            const auto access = record->ExceptionInformation[0];
            message.Text("\n  access: ").Text(access == 0 ? "read" : access == 1 ? "write" : access == 8 ? "execute" : "unknown")
                .Text(" at ").Hex(record->ExceptionInformation[1], sizeof(uintptr_t) * 2);
            if (record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR && record->NumberParameters >= 3)
                message.Text("\n  paging error: ").Hex(record->ExceptionInformation[2]);
        }
    } else {
        message.Text("\n  exception record unavailable");
    }
    message.Text("\n  active guest function: ").Hex(RecompMod::CurrentTranslatedExecutionAddress());
    message.Fatal();
}

#else
// POSIX has one process-wide signal handler. It attempts guest mapping recovery
// before reporting an unhandled fault; Windows installs that recovery in GuestFlat.
// Only async-signal-safe operations belong here: the interrupted thread may hold
// allocator, logger, or window-system locks. Leave detailed inspection to the
// debugger/core dump instead of risking another fault or deadlock while reporting.
void ReportUnhandledSignalFault(int sig, void* faultAddress) {
    constexpr char prefix[] = "Fatal memory signal at host address 0x";
    char message[sizeof(prefix) - 1 + sizeof(uintptr_t) * 2 + 1];
    size_t position = 0;
    for (size_t i = 0; i < sizeof(prefix) - 1; ++i) message[position++] = prefix[i];
    const auto address = reinterpret_cast<uintptr_t>(faultAddress);
    for (size_t digit = sizeof(uintptr_t) * 2; digit > 0; --digit)
        message[position++] = "0123456789abcdef"[(address >> ((digit - 1) * 4)) & 0xf];
    message[position++] = '\n';
    const auto ignored = write(STDERR_FILENO, message, position);
    (void)ignored;
    // The signal remains blocked until this handler returns; its pending default
    // delivery terminates with the original signal rather than a generic exit(1).
    if (signal(sig, SIG_DFL) == SIG_ERR || raise(sig) != 0) _exit(128 + sig);
}

void PosixMemoryFaultHandler(int sig, siginfo_t* info, void* ucontextVoid) {
    void* faultAddress = info != nullptr ? info->si_addr : nullptr;
    const auto fault = HostFault::Decode(ucontextVoid);

    // Guest-space faults are the flat memory interception mechanism (MMIO,
    // the executable-write guard, unmapped pages). Resolving one here means resuming the
    // faulting instruction, which just returning from the handler does.
    // Unknown metadata and instruction fetches must never be treated as reads.
    if (faultAddress != nullptr && HostFault::MayRecoverDataFault(sig, info->si_code, fault) &&
        GuestFlat::HandleAccessViolation(faultAddress, fault.access == HostFault::Access::Write, fault.instruction)) {
        return;
    }

    ReportUnhandledSignalFault(sig, faultAddress);
}

void InstallPosixMemoryFaultHandler() {
    struct sigaction action {};
    action.sa_sigaction = PosixMemoryFaultHandler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, nullptr) != 0) {
        FaultReport::Message message;
        message.Text("[runtime] FATAL installing SIGSEGV handler; errno=").Decimal(errno).Fatal();
    }
    // Linux pager failures use SIGBUS; Darwin protection faults can use it too.
    // Only faults admitted by platform-specific classification enter memory recovery.
    if (sigaction(SIGBUS, &action, nullptr) != 0) {
        FaultReport::Message message;
        message.Text("[runtime] FATAL installing SIGBUS handler; errno=").Decimal(errno).Fatal();
    }
}
#endif

void AbortSignalHandler(int signum) {
    if (g_abortSignalHandled.test_and_set()) std::_Exit(EXIT_FAILURE);
    FaultReport::Message message;
    message.Text("[runtime] FATAL abort signal ").Decimal(signum);
    message.Fatal();
}

} // namespace

// Retained as the thin public wrapper over WriteFatalLogImpl (declared in
// system_bridge.h); it has no in-tree callers because every fatal path now goes
// through RuntimeCrash::WriteCrashArtifacts.
void WriteFatalLog(std::string_view reason) {
    WriteFatalLogImpl(reason);
}

void SetRuntimeExitCode(int code) {
    SetRuntimeExitCodeImpl(code);
}

// Global handler called via atexit() to flush buffers before any exit
static void AtExitHandler() {
    // End-of-run guest memory report. This runs before the fatal-report check
    // below because the counters describe the whole session and are just as
    // interesting after a crash as after a clean exit.
    GuestFlat::LogFaultSummary();
    // Files the game saved to the SD card go back into UserData/SD.
    SdCard::Shutdown();

    // Skip if already reported by another handler
    if (g_fatalErrorReported.load(std::memory_order_acquire)) {
        return;
    }
    if (g_exitCodeSet.load(std::memory_order_relaxed) &&
        g_lastExitCode.load(std::memory_order_relaxed) != 0) {
        ShowRuntimeFatalPopup("the runtime exited with an error",
                              "The game stopped after reporting a fatal error. Check the process transcript and crash log for details.");
        WriteFatalLogImpl("exitcode");
    }

    std::cerr.flush();
    std::cout.flush();
    std::fflush(stdout);
    std::fflush(stderr);
}

// Global terminate handler for uncaught exceptions
static void TerminateHandler() {
    // Skip detailed dump if already reported
    if (g_fatalErrorReported.load(std::memory_order_acquire)) {
        std::fflush(stderr);
        std::_Exit(EXIT_FAILURE);
    }
    std::string terminateDetails;
    if (auto ex = std::current_exception()) {
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            terminateDetails = std::string("Unhandled C++ exception: ") + e.what();
            RT_LOG(RT_TAG_RUNTIME) << "Unhandled C++ exception: " << e.what() << std::endl;
        } catch (...) {
            terminateDetails = "Unhandled non-std C++ exception.";
            RT_LOG(RT_TAG_RUNTIME) << "Unhandled non-std C++ exception." << std::endl;
        }
    } else {
        terminateDetails = "std::terminate() without current exception.";
    }
    ShowRuntimeFatalPopup("an unhandled C++ exception occurred", terminateDetails);
    const std::string hostStackSummary = FormatHostStackTrace(1);
    if (!hostStackSummary.empty()) {
        terminateDetails.append("\n");
        terminateDetails.append(hostStackSummary);
    }
    WriteFatalLogImpl("terminate", terminateDetails);
    RT_LOG(RT_TAG_RUNTIME) << "std::terminate() called - program exiting" << std::endl;
    std::cerr.flush();
    DumpHostStackTrace();
    if (const CpuContext* cpu = TryGetCpuContext()) {
        RT_LOG(RT_TAG_RUNTIME) << "CPU state at terminate:" << std::endl;
        SystemBridge::DumpCpuState(cpu);
    }
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

// Runtime entry point: loads the configuration, brings up aurora and runs the game.
int RuntimeMain(int argc, char** argv) {
    // Must run before the transcript duplicates stdout/stderr: it decides what
    // those descriptors are mirrored to now that the products are GUI-subsystem.
    AttachParentConsoleForDiagnostics();
#if defined(_WIN32)
    ConfigureWindowsFatalDialogBehavior();
    SetUnhandledExceptionFilter(UnhandledSehFilter);
    WindowsTimerResolutionGuard timerResolutionGuard;
#else
    InstallPosixMemoryFaultHandler();
#endif
    InitializeProcessTranscript(argc, argv);
    if (std::signal(SIGABRT, AbortSignalHandler) == SIG_ERR) {
        FaultReport::Message message;
        message.Text("[runtime] FATAL installing SIGABRT handler; errno=").Decimal(errno).Fatal();
    }
    // Install exit/terminate handlers to ensure we get crash info
    std::atexit(AtExitHandler);
    std::set_terminate(TerminateHandler);
    
    std::string currentEntryLabel;

    try {
        if (argc != 1) {
            throw std::invalid_argument("The game runtime does not accept command-line options; use Config.toml through the installed host.");
        }
        RuntimeConfigFile::LogLoadedConfig();
        // The player's own extracted game (game_folder.h); on Windows it is asked for at first start.
        if (!GameFolder::EnsureConfigured()) {
            SetRuntimeExitCodeImpl(0);
            ShutdownProcessTranscript();
            return 0;
        }
#if defined(__ANDROID__)
        // Determinism testing on a phone: debug.nsmbw.det=1 turns the lockstep timing on,
        // debug.nsmbw.dethash / detdump set NSMBW_DET_HASH / NSMBW_DET_DUMP (det_hash.h) and
        // debug.nsmbw.script[2|3] the scripted remote (NSMBW_INPUT_SCRIPT), 91 characters each.
        {
            const auto property = [](const char* name) {
                char value[PROP_VALUE_MAX] = {};
                return __system_property_get(name, value) > 0 ? std::string(value) : std::string();
            };
            if (property("debug.nsmbw.det") == "1") {
                setenv("NSMBW_DETERMINISTIC", "1", 1);
            }
            for (const auto& [prop, env] : {std::pair{"debug.nsmbw.dethash", "NSMBW_DET_HASH"},
                                            std::pair{"debug.nsmbw.detdump", "NSMBW_DET_DUMP"},
                                            std::pair{"debug.nsmbw.lobbytest", "NSMBW_LOBBY_TEST"},
                                            std::pair{"debug.nsmbw.tsonly", "NSMBW_NET_TAILSCALE_ONLY"},
                                            std::pair{"debug.nsmbw.logfps", "NSMBW_LOG_FPS"},
                                            std::pair{"debug.nsmbw.patchreadback", "NSMBW_PATCH_READBACK"}}) {
                if (const std::string value = property(prop); !value.empty()) {
                    setenv(env, value.c_str(), 1);
                }
            }
            std::string script = property("debug.nsmbw.script") + property("debug.nsmbw.script2") +
                                 property("debug.nsmbw.script3");
            if (!script.empty()) {
                setenv("NSMBW_INPUT_SCRIPT", script.c_str(), 1);
            }
            if (const std::string session = property("debug.nsmbw.sscript"); !session.empty()) {
                setenv("NSMBW_SESSION_SCRIPT", session.c_str(), 1);
            }
        }
#endif
        if (const char* deterministic = std::getenv("NSMBW_DETERMINISTIC");
            deterministic != nullptr && std::string(deterministic) == "1") {
            DetClock::Enable();
        }
        // Network play: the session settings turn deterministic mode on and feed every frame's
        // input from all players (netplay_session.h).
        // A restart out of a LAN room lands here: the session file names the players, and the NAND
        // switches to the session's copy of the host's save before anything reads it.
        {
            NetplaySession::Config netplay;
            if (NetplayStart::TakePendingSession(netplay)) {
                NetplaySession::Start(netplay);
            } else if (NetplaySession::LoadConfigFromEnvironment(netplay)) {
                NetplaySession::Start(netplay);
            }
            NetplaySession::SetStallHandler(&NetplayUi::OnSessionStall);
        }
        // A big mod's SD card takes minutes to build when the game first asks for it: keep the window alive and show
        // how far the build is on the title card, as a network stall does (issue #2: "Not responding").
        SdCard::SetProgressHandler([](uint64_t done, uint64_t total) {
            static std::chrono::steady_clock::time_point last{};
            const auto now = std::chrono::steady_clock::now();
            if (done < total && now - last < std::chrono::milliseconds(33)) return;
            last = now;
            char label[96];
            std::snprintf(label, sizeof(label), "Preparing the mods' SD card... %.1f of %.1f GB",
                          done / 1073741824.0, total / 1073741824.0);
            settings_overlay::SetBusyProgress(done < total ? label : nullptr,
                                              total ? static_cast<float>(static_cast<double>(done) / total) : 1.0f);
            NetplayUi::OnSessionStall();
        });
        if (RuntimeConfigFile::DiscordPresenceEnabled()) {
            DiscordPresence::Initialize(RuntimeConfigFile::DiscordClientId(), "Super Smash Bros. Brawl");
        }
        SystemBridge::Initialize();
        TranslatedFunctionRegistry::Finalize();

        // Initialize Aurora (graphics backend)
        // We use auto backend (or specific if needed) and set a default window size.
        // This is required for GX commands (like texture loading) to work.
        AuroraConfig auroraConfig = {};
        auroraConfig.appName = "Super Smash Bros. Brawl";
        const auto applicationDataDirectory = RuntimeConfigFile::ApplicationDataDirectory();
        const auto rendererCacheDirectory = applicationDataDirectory / "Cache";
        std::error_code rendererPathError;
        std::filesystem::create_directories(rendererCacheDirectory, rendererPathError);
        if (rendererPathError) {
            RT_LOG(RT_TAG_RUNTIME) << "Unable to create renderer cache directory "
                      << RuntimeConfigFile::PathToUtf8(rendererCacheDirectory) << ": "
                      << rendererPathError.message() << std::endl;
        }
        const std::string auroraUserPath = RuntimeConfigFile::PathToUtf8(applicationDataDirectory);
        const std::string auroraCachePath = RuntimeConfigFile::PathToUtf8(rendererCacheDirectory);
        auroraConfig.userPath = auroraUserPath.c_str();
        auroraConfig.cachePath = auroraCachePath.c_str();
        // initial_pipeline_cache.db (NSMBW's pipelines, merged into the user's cache at startup)
        // sits beside the executable, or in the app's unpacked runtime folder on Android.
        const auto runtimeDirectory = RuntimeConfigFile::ExecutableDirectory();
        const std::string auroraResourcesPath =
            runtimeDirectory ? RuntimeConfigFile::PathToUtf8(*runtimeDirectory) : std::string();
        if (!auroraResourcesPath.empty()) {
            auroraConfig.resourcesPath = auroraResourcesPath.c_str();
        }
        auroraConfig.logCallback = &RuntimeAuroraLogCallback;
        auroraConfig.logLevel = LOG_DEBUG;
        // NSMBW display settings: aspect_mode, else the older widescreen/force_16_9 keys.
        DisplaySettings::Settings displaySettings;
        displaySettings.aspect = RuntimeConfigFile::AspectMode(
            RuntimeConfigFile::ForceAspect169Enabled()      ? DisplaySettings::kAspect16x9
            : !RuntimeConfigFile::WidescreenEnabled(true) ? DisplaySettings::kAspect4x3
                                                          : DisplaySettings::kAspectFill);
        displaySettings.renderScale = RuntimeConfigFile::ResolutionMultiplier(2.25f);  // 1080p
        DisplaySettings::Initialize(displaySettings);
        const bool configWidescreen = DisplaySettings::GameWidescreen();
        auroraConfig.windowWidth = configWidescreen ? 854 : 640;
        auroraConfig.windowHeight = 480;
        auroraConfig.windowWidth = RuntimeConfigFile::WindowWidth(auroraConfig.windowWidth);
        auroraConfig.windowHeight = RuntimeConfigFile::WindowHeight(auroraConfig.windowHeight);
        auroraConfig.hasWindowPosition = RuntimeConfigFile::WindowPosition(
            auroraConfig.windowPosX, auroraConfig.windowPosY);
        auroraConfig.allowJoystickBackgroundEvents = true;
        auroraConfig.disableCopyFilter = RuntimeConfigFile::DisableCopyFilter(true);
        // Dolphin-style custom textures. Aurora indexes <userPath>/texture_replacements
        // once during aurora_initialize, so both knobs only take effect on the next launch.
        // Dumps name each unmatched texture the way its replacement would have to be named,
        // which is only useful while the index is live - hence the conjunction.
        auroraConfig.allowTextureReplacements = RuntimeConfigFile::TextureReplacements(true);
        auroraConfig.allowTextureDumps = auroraConfig.allowTextureReplacements &&
                                         RuntimeConfigFile::TextureDumps(false);
        // No vsync knob: aurora always configures a non-blocking present mode.
        auroraConfig.desiredBackend = BACKEND_AUTO;
        // GPU driver workarounds: picked from the graphics adapter unless the player forced them.
        auroraConfig.gpuCompat = static_cast<AuroraGpuCompat>(RuntimeConfigFile::GpuCompat());
        // DisplaySettings applies the viewport policy and render height at the first frame boundary.
        // One table for both directions. RuntimeConfigFile::IsSupportedGraphicsApi
        // whitelists exactly these config names, so an unrecognised value has
        // already been rejected (and reported) at parse time.
        struct GraphicsBackendEntry {
            const char* configName;
            AuroraBackend backend;
        };
#if defined(__APPLE__)
        static constexpr std::array<GraphicsBackendEntry, 2> kGraphicsBackends{{
            {"auto", BACKEND_AUTO}, {"metal", BACKEND_METAL},
        }};
// only vulkan for linux
#elif defined(__linux__)
            static constexpr std::array<GraphicsBackendEntry, 2> kGraphicsBackends{{
            {"auto", BACKEND_AUTO}, {"vulkan", BACKEND_VULKAN},
        }};
#elif defined(_WIN32)
        static constexpr std::array<GraphicsBackendEntry, 3> kGraphicsBackends{{
            {"auto", BACKEND_AUTO}, {"d3d12", BACKEND_D3D12}, {"vulkan", BACKEND_VULKAN},
        }};

#endif
        const auto backendDisplayName = [](AuroraBackend value) -> const char* {
            for (const auto& entry : kGraphicsBackends) {
                if (entry.backend == value) {
                    return entry.configName;
                }
            }
            return "unknown";
        };

        const std::string backend = RuntimeConfigFile::GraphicsApi("auto");
        for (const auto& entry : kGraphicsBackends) {
            if (backend == entry.configName) {
                auroraConfig.desiredBackend = entry.backend;
                break;
            }
        }
        const AuroraBackend requestedBackend = auroraConfig.desiredBackend;

        // SDL only reads its Wii driver hint when the joystick subsystem starts, which
        // aurora_initialize does; a Bluetooth Wii Remote paired before launch must be
        // visible on that first scan.
        WiiRemoteInput::ConfigureSdlHints(RuntimeConfigFile::WiiRemotesEnabled(true));
#if defined(__ANDROID__)
        // Graphics debugging on a phone without a rebuild: `adb shell setprop debug.nsmbw.gfx
        // validate,robust` reaches aurora's device setup as AURORA_GFX_FLAGS (webgpu/gpu.cpp).
        // A property holds 91 characters, so debug.nsmbw.gfx2 and gfx3 continue the list.
        {
            std::string flags;
            for (const char* name : {"debug.nsmbw.gfx", "debug.nsmbw.gfx2", "debug.nsmbw.gfx3"}) {
                char value[PROP_VALUE_MAX] = {};
                if (__system_property_get(name, value) > 0) {
                    flags += flags.empty() ? "" : ",";
                    flags += value;
                }
            }
            if (!flags.empty()) {
                setenv("AURORA_GFX_FLAGS", flags.c_str(), 1);
                RT_LOG(RT_TAG_RUNTIME) << "debug.nsmbw.gfx flags: " << flags << std::endl;
            }
        }
#endif


        const AuroraInfo auroraInfo = aurora_initialize(0, nullptr, &auroraConfig);
        if (auroraInfo.initializationStatus != AURORA_INITIALIZATION_SUCCESS) {
            throw std::runtime_error(auroraInfo.initializationError != nullptr
                ? auroraInfo.initializationError : "No supported graphics backend is available");
        }
        if (requestedBackend != BACKEND_AUTO && auroraInfo.backend != requestedBackend) {
            RT_LOG(RT_TAG_RUNTIME) << "graphics_api=\"" << backend
                      << "\" is not available on this system; aurora fell back to \""
                      << backendDisplayName(auroraInfo.backend)
                      << "\". See the [aurora::gpu] lines above for the reason." << std::endl;
        } else {
            RT_LOG(RT_TAG_RUNTIME) << "graphics backend: " << backendDisplayName(auroraInfo.backend)
                      << std::endl;
        }
        GraphicsInfo::Set(auroraInfo.gpuName != nullptr ? auroraInfo.gpuName : "", auroraInfo.gpuCompatActive);
        // BRAWL_ASSUME_INTEGRATED_GPU=1 tests the integrated-GPU limits on a PC with a graphics card.
        GraphicsInfo::SetIntegrated(auroraInfo.gpuIntegrated || std::getenv("BRAWL_ASSUME_INTEGRATED_GPU") != nullptr);
        RT_LOG(RT_TAG_RUNTIME) << "graphics adapter: " << GraphicsInfo::Name() << ", compatibility fixes "
                  << (auroraInfo.gpuCompatActive ? "on" : "off") << std::endl;
        aurora_set_frame_worker_wait_callback(ServiceGuestTimingDuringAuroraFrameWait);
        GxGuestWrite::InstallAuroraHooks();
        DisplaySettings::OnFrameBoundary(auroraInfo.windowSize.native_fb_width,
                                         auroraInfo.windowSize.native_fb_height);
        settings_overlay::InitializeRuntimeSettings();
        static constexpr const char* kAspectNames[] = {"original 4:3", "16:9", "fill", "cinematic 2.39:1"};
        RT_LOG(RT_TAG_CONFIG) << "display aspect=" << kAspectNames[displaySettings.aspect]
                  << " SCGetAspectRatio=" << (configWidescreen ? 1 : 0)
                  << " render=" << DisplaySettings::LinesForScale(displaySettings.renderScale) << " lines ("
                  << displaySettings.renderScale << "x)"
                  << " window=" << auroraInfo.windowSize.width << "x" << auroraInfo.windowSize.height
                  << " native=" << auroraInfo.windowSize.native_fb_width << "x"
                  << auroraInfo.windowSize.native_fb_height << std::endl;
        g_auroraInitialized.store(true, std::memory_order_release);

        auto entry = ResolveEntry();
        Gecko::Install();  // mods' Gecko codes (UserData/SD/<mod>/RSBE01.GCT), before the game's first instruction
        GuestCodePatch::InterpretFromEnvironment();
        InitializePersistentCpuContext();
        auto& cpu = GetPersistentCpuContext();
        SeedCpuContext(cpu);
        
        // Initialize the fiber-based threading system
        Fiber::GuestFiberManager::Initialize();

        // Deterministic mode: the controllers as boot sees them, until the first retrace latches.
        if (DetClock::Enabled()) {
            FrameInput::Latch(0);
        }
        
        CpuContextScope cpuScope(&cpu);

        std::string label = entry->name;
        if (label.empty()) {
            std::ostringstream oss;
            oss << "0x" << std::hex << entry->address;
            label = oss.str();
        }
        currentEntryLabel = label;
        g_lastEntryLabel = currentEntryLabel;

        char stackTop = 0;  // the game's main thread runs below here (save_states.h)
        SaveStates::MarkStackTop(&stackTop + 1);
        InvokeIndirectCpu(entry->address, &cpu);
        const uint32_t result = cpu.gpr[3];
        RT_LOG(RT_TAG_RUNTIME) << label << " => 0x" << std::hex << result << std::dec << " (" << result << ")" << std::endl;
        
        // Shutdown fiber system
        Fiber::GuestFiberManager::Shutdown();
        WindowPlacementPersistence::Flush(true);
        aurora_shutdown();
        DiscordPresence::Shutdown();
        SetRuntimeExitCodeImpl(0);
        ShutdownProcessTranscript();
        return 0;
    } catch (const Memory::AccessViolation& ex) {
        std::cerr << "Runtime error: " << ex.what() << std::endl;
        DumpAccessViolationReport(ex, currentEntryLabel);
        std::ostringstream details;
        details << "addr=0x" << std::hex << std::uppercase << ex.address()
                << " len=0x" << ex.length()
                << std::dec << std::nouppercase
                << " reason=" << ex.reason();
        ShowRuntimeFatalPopup("a guest memory access was out of bounds", details.str());
        WriteFatalLogImpl("access_violation", details.str());
        SetRuntimeExitCodeImpl(1);
        Fiber::GuestFiberManager::Shutdown();
        WindowPlacementPersistence::Flush(true);
        aurora_shutdown();
        DiscordPresence::Shutdown();
        ShutdownProcessTranscript();
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "Runtime error: " << ex.what() << std::endl;
        SystemBridge::DumpCpuState(TryGetCpuContext());
        ShowRuntimeFatalPopup("a runtime exception occurred", ex.what());
        WriteFatalLogImpl("exception");
        SetRuntimeExitCodeImpl(1);
        Fiber::GuestFiberManager::Shutdown();
        WindowPlacementPersistence::Flush(true);
        aurora_shutdown();
        DiscordPresence::Shutdown();
        ShutdownProcessTranscript();
        return 1;
    }
}

#if defined(__ANDROID__)
// SDL's Android activity loads libmain.so and runs the game through its exported SDL_main.
extern "C" __attribute__((visibility("default"))) int SDL_main(int argc, char** argv) {
    return RuntimeMain(argc, argv);
}
#elif defined(MKW_ENGINE_MODULE)
// Windows engine module: src/product/engine_loader.cpp loads it and calls this.
extern "C" __declspec(dllexport) int SDL_main(int argc, char** argv) {
    return RuntimeMain(argc, argv);
}
#else
int main(int argc, char** argv) {
    return RuntimeMain(argc, argv);
}
#endif
