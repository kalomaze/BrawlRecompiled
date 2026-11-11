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
x86-64-v3 baseline on x86-64 and require a compatible CPU. They use synthetic inputs, do not
need a disc or generated game code, and remove their temporary files even on failure.
Without the environment variable, these tests are reported as skipped rather than excluded
from compilation. Native execution has been verified on Ubuntu; other hosts are unverified.

An external generic DOL can be included without adding it to the repository:

```powershell
$env:RECOMP_GENERIC_DOL = 'D:\path\to\main.dol'
dotnet test translator/Translator.sln -c Release
```

The test treats the file as a generic DOL: it reads the DOL entry point, builds a DOL-only image,
and translates that entry point without game-specific addresses or bindings.
