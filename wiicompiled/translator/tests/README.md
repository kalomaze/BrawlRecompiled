# Translator tests

`Translator.Tests` is the default binary-free suite. It covers the decoder, IR/SSA, code
generation helpers, parsers with synthetic inputs, project-manifest loading, DOL-only image
construction, and generic entry-point translation.

All test sources compile in the normal suite. Nine native tests additionally compile and run
small C++ programs against the real runtime helpers. Enable them on Ubuntu with Clang installed:

```sh
BRAWL_NATIVE_TESTS=1 dotnet test translator/Translator.sln -c Release
```

`CXX` can select a Clang-compatible compiler executable. Native tests use the runtime's
x86-64-v3 baseline by default; set `MKW_HOST_ISA=v1`, `v2`, or `v3` to select
the instruction floor. `BRAWL_NATIVE_RUNNER` can name an executable wrapper for CPU
emulation. The selected floor requires a compatible CPU. They use synthetic inputs, do not
need a disc or generated game code, and remove their temporary files even on failure.
Without the environment variable, these tests are reported as skipped rather than excluded
from compilation. Native execution has been verified on Ubuntu; other hosts are unverified.

To run the native helpers under a Core 2 CPU model on Linux with QEMU user-mode
emulation installed (the .NET test runner itself stays native):

```sh
BRAWL_NATIVE_TESTS=1 MKW_HOST_ISA=v1 QEMU_CPU=core2duo BRAWL_NATIVE_RUNNER=qemu-x86_64 \
  dotnet test translator/Translator.sln -c Release
```

This checks the exercised instructions and helper semantics, not game performance
or graphics compatibility on period hardware.

An external generic DOL can be included without adding it to the repository:

```powershell
$env:RECOMP_GENERIC_DOL = 'D:\path\to\main.dol'
dotnet test translator/Translator.sln -c Release
```

The test treats the file as a generic DOL: it reads the DOL entry point, builds a DOL-only image,
and translates that entry point without game-specific addresses or bindings.
