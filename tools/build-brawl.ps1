# Windows release build of Super Smash Bros. Brawl: build.cmake (the same build as on Linux) with the release's
# pinned llvm-mingw toolchain, the engine as a DLL started by a small loader exe, and the C++/WinRT headers for the
# media-session monitor. The result is copied to Play\, which tools\package-brawl.ps1 packages.
#   powershell -ExecutionPolicy Bypass -File tools\build-brawl.ps1 [-Game <extracted disc>] [-Jobs N] [-OldCpu]
# Extract the disc first (extract.cmake, see README.md); -Game defaults to Disc\DATA, like build.cmake.
[CmdletBinding()]
param(
    [string]$Game,
    [int]$Jobs = 0,
    # The engine for processors without AVX2/FMA (x86-64-v2): engine_usa_v2.dll, built in build\release-v2\; the
    # loader picks it on such a CPU.
    [switch]$OldCpu
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$root = Split-Path -Parent $PSScriptRoot            # <project>\tools -> <project>
$launcherScripts = Join-Path $root 'wiicompiled\Launcher'
$toolchain = Join-Path $root 'toolchain'
$build = Join-Path $root $(if ($OldCpu) { 'build\release-v2' } else { 'build\release' })
$module = if ($OldCpu) { 'engine_usa_v2' } else { 'engine_usa' }
if ($Jobs -le 0) {
    # A translated shard compile can take ~2.5 GB; one job per core exhausted a 16 GB machine.
    $ramGb = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB
    $Jobs = [Math]::Max(1, [Math]::Min([Environment]::ProcessorCount, [int][Math]::Floor($ramGb / 2.5)))
}

if (-not (Test-Path -LiteralPath (Join-Path $toolchain 'llvm-mingw\bin\clang++.exe'))) {
    & (Join-Path $launcherScripts 'Prepare-PortableTools.ps1') -Destination $toolchain
}
# The pinned dependency trees: their license files go into the release (package-brawl.ps1, package-linux.sh).
$dependencies = Join-Path $root 'wiicompiled\Dependencies'
$cppWinRt = Join-Path $dependencies 'cppwinrt'
if (-not (Test-Path -LiteralPath (Join-Path $cppWinRt 'winrt\base.h'))) {
    & (Join-Path $launcherScripts 'Prepare-Dependencies.ps1') -Destination $dependencies
}
$env:PATH = "$toolchain\llvm-mingw\bin;$toolchain\CMake\bin;$toolchain\Ninja;$env:PATH"

$release = "-DMKW_ENGINE_MODULE=ON;-DMKW_ENGINE_MODULE_NAME=$module;-DMKW_CPPWINRT_INCLUDE_DIR=$($cppWinRt.Replace('\', '/'))"
$extra = @(if ($OldCpu) { '-DMKW_HOST_ISA=v2' }) + @(if ($Game) { "-DGAME=$Game" })
& cmake "-DBUILD_DIR=$build" "-DJOBS=$Jobs" "-DNATIVE_ARGS=$release" @extra -P (Join-Path $root 'build.cmake')
if ($LASTEXITCODE -ne 0) { throw "build.cmake failed with exit code $LASTEXITCODE." }
$native = Join-Path $build 'native'

# Only the copy running from Play\ locks the files we overwrite; sweeps run their own copy under autotest\ and must survive.
$playExe = [IO.Path]::GetFullPath((Join-Path $root 'Play\SuperSmashBrosBrawl.exe'))
Get-Process SuperSmashBrosBrawl -ErrorAction SilentlyContinue | Where-Object { $_.Path -and [IO.Path]::GetFullPath($_.Path) -ieq $playExe } | Stop-Process -Force; Start-Sleep -Seconds 1
# A killed game can leave a zombie process (one thread stuck in a driver) holding the old files open for a long
# time; Windows still lets them be renamed, so the copy moves such a file aside and the leftovers go once freed.
Get-ChildItem (Join-Path $root 'Play') -Filter '*.locked-*' | ForEach-Object { Remove-Item -LiteralPath $_.FullName -ErrorAction SilentlyContinue }
function Copy-ReplacingLocked($from, $to) {
    try { Copy-Item -LiteralPath $from -Destination $to -Force -ErrorAction Stop }
    catch {
        Move-Item -LiteralPath $to -Destination "$to.locked-$([DateTime]::Now.ToString('HHmmss'))" -Force
        Copy-Item -LiteralPath $from -Destination $to -Force
    }
}
# Everything CMake staged beside the program (runtime/cmake/PublicProducts.cmake): the engine, the libraries it loads
# and its data files.
Get-ChildItem (Join-Path $native '*') -File -Include '*.dll', 'dsp_coef.bin', 'initial_pipeline_cache.db' |
    ForEach-Object { Copy-ReplacingLocked $_.FullName (Join-Path $root "Play\$($_.Name)") }
Copy-Item -LiteralPath (Join-Path $native 'wii_bootstrap') -Destination (Join-Path $root 'Play') -Recurse -Force
if (-not $OldCpu) { Copy-ReplacingLocked (Join-Path $native 'WiiCompiled.exe') $playExe }
Write-Host "Copied $native to Play\"
