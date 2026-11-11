using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Translator.Core.Loading;

namespace Translator.Tests;

internal static class TranslatorCppTestHarness
{
    public static string FindWorkspaceRoot()
    {
        for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory is not null; directory = directory.Parent)
        {
            foreach (var candidate in new[] { directory.FullName, Path.Combine(directory.FullName, "wiicompiled") })
                if (File.Exists(Path.Combine(candidate, "runtime", "include", "ppc_runtime.h")))
                    return candidate;
        }
        throw new DirectoryNotFoundException("Could not find Brawl workspace.");
    }

    public static string CompileAndRun(string harness, string? generated = null)
    {
        var root = FindWorkspaceRoot();
        var temp = Directory.CreateTempSubdirectory("brawl-cpp-test-").FullName;
        try
        {
            Directory.CreateDirectory(Path.Combine(temp, "generated"));
            File.WriteAllText(Path.Combine(temp, "generated", "RuntimeConfig.h"),
                "#pragma once\nnamespace RuntimeConfig { constexpr unsigned SDA1_BASE = 0, SDA2_BASE = 0; }\n");
            var harnessPath = Path.Combine(temp, "harness.cpp");
            File.WriteAllText(harnessPath, harness);
            var output = Path.Combine(temp, OperatingSystem.IsWindows() ? "runner.exe" : "runner");
            var arguments = new List<string> {
                "-std=c++20", "-fno-fast-math", "-ffp-contract=off",
                "-ffunction-sections", "-fdata-sections",
                OperatingSystem.IsMacOS() ? "-Wl,-dead_strip" : "-Wl,--gc-sections",
                "-I", Path.Combine(root, "runtime", "include"), "-I", temp,
                "-o", output, harnessPath, WriteSupportSource(temp)
            };
            if (System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture == System.Runtime.InteropServices.Architecture.X64)
            {
                var isa = Environment.GetEnvironmentVariable("MKW_HOST_ISA") ?? "v3";
                if (isa is not ("v2" or "v3"))
                    throw new ArgumentException("MKW_HOST_ISA must be v2 or v3");
                arguments.Add($"-march=x86-64-{isa}");
            }
            foreach (var source in new[] { "abi_bridge.cpp", "fpu_helpers.cpp", "memory.cpp", "ppc_helpers.cpp", "ppc_quantized.cpp" })
                arguments.Add(Path.Combine(root, "runtime", "src", source));
            if (Environment.GetEnvironmentVariable("MKW_FLAT_MEMORY") == "1")
            {
                arguments.Add("-DMKW_FLAT_MEMORY=1");
                arguments.Add(Path.Combine(root, "runtime", "src",
                    OperatingSystem.IsMacOS() ? "guest_flat_memory_macos.cpp" : "guest_flat_memory.cpp"));
            }
            if (generated is not null)
            {
                var generatedPath = Path.Combine(temp, "generated.cpp");
                File.WriteAllText(generatedPath, generated);
                arguments.Add(generatedPath);
            }
            Run(Environment.GetEnvironmentVariable("CXX") ?? "clang++", arguments, 120);
            var runner = Environment.GetEnvironmentVariable("BRAWL_NATIVE_RUNNER");
            return (string.IsNullOrEmpty(runner)
                ? Run(output, [], 10)
                : Run(runner, [output], 30)).Trim();
        }
        finally
        {
            Directory.Delete(temp, recursive: true);
        }
    }

    private static string Run(string executable, IEnumerable<string> arguments, int timeoutSeconds)
    {
        var start = new System.Diagnostics.ProcessStartInfo(executable) {
            RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false
        };
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        using var process = System.Diagnostics.Process.Start(start)
            ?? throw new InvalidOperationException($"Could not start {executable}");
        var stdout = process.StandardOutput.ReadToEndAsync();
        var stderr = process.StandardError.ReadToEndAsync();
        if (!process.WaitForExit(timeoutSeconds * 1000))
        {
            process.Kill(entireProcessTree: true);
            process.WaitForExit();
            throw new TimeoutException($"{executable} timed out after {timeoutSeconds}s");
        }
        var output = stdout.GetAwaiter().GetResult();
        var errors = stderr.GetAwaiter().GetResult();
        if (process.ExitCode != 0)
            throw new InvalidOperationException($"{executable} exited {process.ExitCode}:\n{output}{errors}");
        return output;
    }

    public static ProgramImage CreateImage(params (uint Address, uint Value)[] words)
    {
        var maxOffset = 0;
        foreach (var (address, _) in words)
        {
            var offset = checked((int)(address - MemoryLayout.RamBase));
            maxOffset = Math.Max(maxOffset, offset + 4);
        }

        var memory = new byte[Math.Max(maxOffset, 4)];
        foreach (var (address, value) in words)
        {
            var offset = checked((int)(address - MemoryLayout.RamBase));
            memory[offset + 0] = (byte)(value >> 24);
            memory[offset + 1] = (byte)(value >> 16);
            memory[offset + 2] = (byte)(value >> 8);
            memory[offset + 3] = (byte)value;
        }

        return new ProgramImage(
            memory,
            new AddressRange(MemoryLayout.RamBase, checked(MemoryLayout.RamBase + (uint)memory.Length)),
            new AddressRange(MemoryLayout.RamBase, checked(MemoryLayout.RamBase + (uint)memory.Length)),
            new AddressRange(0, 0),
            sha256: "test");
    }

    private static string WriteSupportSource(string tempRoot)
    {
        var path = Path.Combine(tempRoot, "translator_test_support.cpp");
        const string contents = """
            #include "system_bridge.h"
            #include "ppc_runtime.h"
            #include "recomp_mod_loader.h"

            #include <csetjmp>
            #include <atomic>
            #include <cstddef>
            #include <cstdint>
            #include <cstdlib>
            #include <ostream>
            #include <string_view>

            bool g_suppressSehReporting = false;
            thread_local jmp_buf* g_sehJumpTarget = nullptr;
            thread_local uint32_t g_sehLastExceptionCode = 0;
            thread_local uintptr_t g_sehLastExceptionAddress = 0;
            thread_local uintptr_t g_sehLastAccessedAddress = 0;
            thread_local uint32_t g_sehLastAccessType = 0;

            void DebugTrackPcImpl(uint32_t) {}
            void DebugPumpAurora() {}
            #if defined(__GNUC__) || defined(__clang__)
            extern "C" void DumpRecentPcTrace(size_t) __attribute__((weak));
            void MarkFatalErrorReported() __attribute__((weak));
            #endif
            extern "C" void DumpRecentPcTrace(size_t) {}
            void MarkFatalErrorReported() {}
            extern "C" void DumpHostStackTraceForRuntimeHelper() {}
            extern "C" void OS_HLE_ProcessAlarms(int) {}
            void ShowRuntimeFatalPopup(std::string_view, std::string_view) noexcept
            {
                std::abort();
            }

            // Unexpected interpreter fallback must fail this isolated dispatch test.
            namespace PpcInterpreter {
            bool Enabled() { return false; }
            bool CanRun(uint32_t) { return false; }
            void Run(CpuContext*, uint32_t) { std::abort(); }
            }
            namespace RuntimeCrash {
            [[noreturn]] void FatalMissingGuestTarget(uint32_t, CpuContext*) noexcept { std::abort(); }
            }

            namespace RecompMod {
            std::atomic<bool> g_executableWriteGuardEnabled{false};
            std::atomic<uint8_t> g_executableWriteGuardPages[kExecutableWriteGuardPageCount]{};
            std::atomic<uint8_t> g_executableWriteGuardCoarsePages[kExecutableWriteGuardCoarsePageCount]{};
            std::atomic<uint8_t> g_executableWriteGuardMidPages[kExecutableWriteGuardMidPageCount]{};
            uint32_t CurrentTranslatedExecutionAddress() noexcept { return 0; }
            bool HandleExecutableWrite(uint32_t, size_t, uint64_t) { return false; }
            void CheckExecutableWrite(uint32_t, size_t, uint64_t) {}
            }

            void SystemBridge::DumpCpuState(const CpuContext*) {}
            void SystemBridge::DumpCpuState(std::ostream&, const CpuContext*) {}
            """;

        File.WriteAllText(path, contents);

        return path;
    }

}

// These tests compile real runtime code; keep them compiled in the normal .NET suite,
// but require an explicit opt-in to execute the external compiler.
public sealed class NativeFactAttribute : Xunit.FactAttribute
{
    public NativeFactAttribute()
    {
        if (Environment.GetEnvironmentVariable("BRAWL_NATIVE_TESTS") != "1")
            Skip = "Set BRAWL_NATIVE_TESTS=1 to run with Clang (or CXX).";
    }
}
