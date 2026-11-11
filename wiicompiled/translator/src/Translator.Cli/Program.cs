using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Numerics;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Translator.Cli.Configuration;
using Translator.Core;
using Translator.Core.CodeGen;
using Translator.Core.Build;
using Translator.Core.Analysis;
using Translator.Core.Analysis.BasicBlocks;
using Translator.Core.Disassembly;
using Translator.Core.Loading;
using Translator.Core.Ir;
using Translator.Core.IO;
using Translator.Core.Parsing.Dol;
using Translator.Core.Parsing.Rel;
using Translator.Core.Translation;

// All formats here (YAML, JSON, hex, hashes) are culture-invariant. Under Turkish locale, YamlDotNet's
// UnderscoredNamingConvention lowercased "Id" to dotless "ıd", breaking the "id" key match. Pin globally.
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.DefaultThreadCurrentUICulture = CultureInfo.InvariantCulture;

var command = args.Length > 0 ? args[0] : "info";
var (projectOption, tail) = ExtractOption(args.Skip(1).ToArray(), "--project");
var project = !string.IsNullOrWhiteSpace(projectOption)
    ? TranslationProjectConfig.Load(projectOption)
    : null;
// The function map is the project's function-start oracle. Loading it here
// also publishes the CodeWarrior save/restore thunk ranges, which three static
// passes (guest ABI contracts, inline-thunk emission, inferred ABI) look up by
// symbol name instead of carrying the same four literal addresses each.
var functionMap = project?.Translation.FunctionMapPath is { } functionMapPath
    ? FunctionMap.Load(functionMapPath)
    : null;
if (functionMap is not null)
{
    GuestSaveRestoreThunks.Current = GuestSaveRestoreThunks.FromFunctionMap(functionMap);
}
var root = project?.WorkspaceRoot ?? Directory.GetCurrentDirectory();
var dolFile = new Lazy<DolFile>(LoadDol);
var relFile = new Lazy<RelFile?>(LoadRel);
var image = new Lazy<ProgramImage>(LoadImage);
// Branch targets outside the loaded image, reported once each (target -> first caller).
var outOfImageTargets = new System.Collections.Concurrent.ConcurrentDictionary<uint, uint>();
var canonicalIrStore = new CanonicalIrStore();
// Hoisted out of the translator factory so the base pipeline can prewarm the
// recursive ABI cache in one stable pass.
var inferredGuestAbi = new Lazy<InferredGuestFunctionAbiProvider>(() =>
    new InferredGuestFunctionAbiProvider(image.Value, canonicalIrStore));
var runtimeNativeIndex = new Lazy<RuntimeNativeIndex>(() =>
    RuntimeNativeIndexBuilder.Build(RequireProject().Runtime.NativeRegistrationRoot));
// Void-stub signatures are only trusted as declared guest ABIs when the stub
// lives under runtime.native_abi_directories (the vetted set); everywhere else
// the C++ signature is documentation and the inferred ABI stays authoritative.
var declaredNativeAbi = new Lazy<RuntimeNativeFunctionAbiProvider?>(() =>
{
    var runtime = RequireProject().Runtime;
    if (runtime.NativeAbiDirectories.Count == 0)
        return null;
    var root = Path.GetFullPath(runtime.NativeRegistrationRoot);
    var prefixes = runtime.NativeAbiDirectories
        .Select(directory =>
        {
            var relative = Path.GetRelativePath(root, Path.GetFullPath(directory)).Replace('\\', '/');
            return relative == "." ? "" : relative.TrimEnd('/') + "/";
        })
        .ToArray();
    var scopedAddresses = runtimeNativeIndex.Value.Registrations
        .Where(registration => prefixes.Any(prefix =>
            registration.SourceFile.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)))
        .Select(static registration => registration.Address)
        .ToHashSet();
    return RuntimeNativeFunctionAbiProvider.FromIndex(runtimeNativeIndex.Value, scopedAddresses);
});
var translator = new Lazy<FunctionTranslator>(() =>
{
    var inferredAbi = inferredGuestAbi.Value;
    var providers = new List<IGuestFunctionAbiProvider>();
    if (declaredNativeAbi.Value is { } nativeAbi)
        providers.Add(nativeAbi);
    providers.Add(inferredAbi);
    var abiProvider = new CompositeGuestFunctionAbiProvider(providers.ToArray());
    return new FunctionTranslator(image.Value, abiProvider);
});
// The runtime source index is built lazily once and shared by every consumer,
// so ABI/effect analysis never rescans the C++ within one translator run.
var runtimeNativeGuestEffects = new Lazy<RuntimeNativeGuestEffectSet>(() =>
    runtimeNativeIndex.Value.ToGuestEffectSet());
var baseTranslationExclusions = new Lazy<IReadOnlySet<uint>>(() =>
    runtimeNativeIndex.Value.Registrations
        .Where(static registration => registration.ExcludesBaseTranslation)
        .Select(static registration => registration.Address)
        .ToHashSet());
var leafInliningBlockedTargets = new Lazy<IReadOnlySet<uint>>(() =>
    BuildLeafInliningBlockedTargets(
        runtimeNativeGuestEffects.Value, baseTranslationExclusions.Value));

// Leaf inlining is a front-end decision: the splice happens before SSA, so the
// canonical graph the emission wave lowers is already inlined and every
// discovery entry point has to agree on it. The emission-time counterpart is
// WithProjectCodegenPolicy.
TranslationOptions WithProjectFrontEndPolicy(
    TranslationOptions options, ProjectTranslation translation) =>
    options with
    {
        AllowUnsupportedInstructions = translation.AllowUnsupportedInstructions,
        // Splicing a body whose runtime winner is a native registration, a
        // dropped translation would silently keep executing
        // the original bytes.
        LeafInliningBlockedTargets = leafInliningBlockedTargets.Value
    };
List<AddressRange>? _executableRanges = null;
// Ranges inside text sections that hold data (--data-in-text): never taken for code by pointer-based discovery.
var dataInTextRanges = new List<AddressRange>();
// Callback discovery now runs on the discovery worker pool, so the range cache
// must be built by exactly one thread and published only once it is complete.
var _executableRangesGate = new object();
// Argument registers checked for callback function pointers at every call site
var argumentRegisters = new[] { "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10" };

// Argument validation runs before any command can execute. `--help` used to
// fall straight through to the command's parser, which ignored every argument
// it did not recognise, so `emit-build-shards --help` ran the command with its
// defaults and rewrote generated/build_shards.
if (HandleCommandLineOptions(command, tail) is { } commandLineExitCode)
{
    return commandLineExitCode;
}

return command switch
{
    "help" or "--help" or "-h" or "-?" => ShowGlobalHelp(),
    "info" or "--info" or "--version" => RunInfo(),
    "generate-data-init" => RunGenerateDataInit(),
    "translate-recursive" => RunTranslateRecursive(tail),
    "emit-build-shards" => RunEmitBuildShards(tail),
    _ => ShowHelp(command)
};

/// <summary>
/// May the workspace's completed base translation be reused for the Code.pul about to build?
/// Exit 0 = yes, 3 = no, anything else = error (also treat as no). Answering wrong in the permissive
/// direction bakes vanilla code into a modded product, so every uncertainty exits non-zero.
/// </summary>
int RunEmitBuildShards(string[] argsTail)
{
    var baseMetadata = OptionValue(argsTail, "--base-metadata") ?? Path.Combine(root, "generated", "base_translation_output.json");
    var baseFunctions = OptionValue(argsTail, "--base-functions-dir") ?? Path.Combine(root, "generated", "functions");
    var output = OptionValue(argsTail, "--out") ?? Path.Combine(root, "generated", "build_shards");
    var nativeSources = OptionValue(argsTail, "--native-source-dir") ?? Path.Combine(root, "runtime", "src");

    try
    {
        var nativeSourcePath = Path.GetFullPath(nativeSources);
        var nativeIndex = RuntimeNativeIndexBuilder.Build(nativeSourcePath);
        var result = TranslatedBuildShardEmitter.Emit(new TranslatedBuildShardOptions(
            Path.GetFullPath(baseMetadata),
            Path.GetFullPath(baseFunctions),
            Path.GetFullPath(output),
            nativeSourcePath,
            // Brawl translates main.dol with all 126 .rel modules (~100k functions): far more shards than 72.
            BaseShardCount: int.Parse(OptionValue(argsTail, "--base-shards") ?? "72", CultureInfo.InvariantCulture),
            NativeIndex: nativeIndex));
        Console.WriteLine("[translator] emitted stable translated build graph");
        Console.WriteLine($"  base functions: {result.BaseFunctionCount:N0}");
        Console.WriteLine($"  shared base functions: {result.SharedBaseFunctionCount:N0}");
        Console.WriteLine($"  shards: {result.BaseShardCount:N0}");
        if (result.BaseCommonShardMapPath is { } shardMapPath)
        {
            Console.WriteLine(result.BaseCommonBoundariesReused
                ? $"  base_common boundaries: reused frozen table {shardMapPath}"
                : $"  base_common boundaries: packed and recorded {shardMapPath}");
        }
        if (result.BaseCommonBalance is { } balance)
        {
            // Frozen membership drifts as new functions are translated into
            // existing ranges. Surfacing the spread is what makes that visible
            // before it starts costing build throughput.
            Console.WriteLine(
                $"  base_common weight spread: {balance.ShardCount:N0} shards, " +
                $"min {balance.MinCompileCostWeight:N0}, mean {balance.MeanCompileCostWeight:N0}, " +
                $"max {balance.MaxCompileCostWeight:N0} (max/min {balance.SpreadRatio:F2}x)");
        }
        Console.WriteLine($"  CMake graph: {result.CMakeManifestPath}");
        return 0;
    }
    catch (Exception ex) when (ex is IOException or InvalidDataException or ArgumentException)
    {
        Console.Error.WriteLine($"emit-build-shards failed: {ex.Message}");
        return 1;
    }
}

int RunInfo()
{
    var asm = Assembly.GetExecutingAssembly();
    var version = asm.GetName().Version?.ToString() ?? "0.0.0";
    Console.WriteLine($"PowerPC static recompiler (C#)\nVersion : {version}\nRuntime : {System.Runtime.InteropServices.RuntimeInformation.FrameworkDescription}");
    if (project is not null)
    {
        Console.WriteLine($"Project : {project.Identity.DisplayName} ({project.Identity.Id})");
        Console.WriteLine($"Memory  : 0x{project.Memory.Base:X8}+0x{project.Memory.Size:X}");
        Console.WriteLine($"DOL     : {project.Inputs.Dol.Path}");
        Console.WriteLine(project.Inputs.Rel is null
            ? "REL     : none"
            : $"REL     : {project.Inputs.Rel.Path} @ 0x{project.Inputs.Rel.LoadAddress:X8}");
    }
    return 0;
}

int RunTranslateRecursive(string[] argsTail)
{
    if (argsTail.Length == 0 || argsTail[0].StartsWith("--", StringComparison.Ordinal))
    {
        Console.Error.WriteLine("Usage: translator translate-recursive <start_addr> [--outdir path] [--output-metadata path] [--threads N] [--prune-stale]");
        return 1;
    }

    var startAddr = ParseAddress(argsTail[0]);
    var allowUnsupportedInstructions = RequireProject().Translation.AllowUnsupportedInstructions;
    var outDir = OptionValue(argsTail, "--outdir") ?? RequireProject().Output.Functions;
    var outputMetadataPath = OptionValue(argsTail, "--output-metadata");
    var productionSourceBundlePath = OptionValue(argsTail, "--production-source-bundle");
    var emitFunctionFiles = !HasFlag(argsTail, "--no-function-files");
    if (OptionValue(argsTail, "--data-in-text") is { } dataInTextPath)
    {
        foreach (var line in File.ReadAllLines(dataInTextPath))
        {
            var fields = line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (fields.Length < 2 || line.TrimStart().StartsWith('#')) continue;
            var start = uint.Parse(fields[0], NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            var end = uint.Parse(fields[1], NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            dataInTextRanges.Add(AddressRange.FromStartAndSize(start, end - start));
        }
    }
    // Mod support (guest code guards) needs every call to enter through func_<addr>, never a state-free variant.
    var disableStateFreeAbi = HasFlag(argsTail, "--no-state-free-abi");
    if (!emitFunctionFiles && string.IsNullOrWhiteSpace(productionSourceBundlePath))
        throw new ArgumentException("--no-function-files requires --production-source-bundle.");
    BaseTranslationOutputMetadata? previousOutputMetadata = null;
    if (HasFlag(argsTail, "--prune-stale") &&
        outputMetadataPath is not null &&
        File.Exists(outputMetadataPath))
    {
        try
        {
            previousOutputMetadata = BaseTranslationOutputMetadataFile.Read(outputMetadataPath);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(
                $"[translator] Warning: existing output metadata could not be read; stale pruning will use one-time tree migration: {ex.Message}");
        }
    }
    var pruneStale = HasFlag(argsTail, "--prune-stale");
    var threadsOption = OptionValue(argsTail, "--threads");
    var maxThreads = threadsOption != null ? Math.Max(1, ParseInt(threadsOption)) : Math.Max(1, Environment.ProcessorCount);
    // Discovery batches: cheap front-end-only work items, so the batch is sized
    // generously to keep queue overhead off the hot path. Deliberately larger
    // than the mod emission wave below, which carries whole C++ emissions.
    var batchSize = Math.Max(64, maxThreads * 8);
    Console.WriteLine($"[translator] Using up to {maxThreads} worker thread(s).");

    // Log start time
    var startTime = DateTime.Now;
    Console.WriteLine($"[translator] Starting translation at {startTime:HH:mm:ss}");

    try
    {
        Directory.CreateDirectory(outDir);

        var visited = new HashSet<uint>();
        var translated = new HashSet<uint>();
        // Per translated function: sorted, coalesced [start, endExclusive) pairs of
        // the instruction addresses its own control flow executes (leaf-inlined
        // callees are decoded separately and never appear here). Feeds the
        // fall-through interior-entry detection for the base manifest.
        var translatedFlowCoverage = new Dictionary<uint, uint[]>();
        var gqrUnknownEntryRoots = new HashSet<uint> { startAddr };
        var translatedFunctionEnds = new Dictionary<uint, uint>();
        // Every map entry is a function start, so the whole boundary set is known
        // before the first decode instead of accumulating as discovery proceeds.
        var knownBaseFunctionEntryPoints = BuildKnownBaseFunctionEntryPoints(
            outDir,
            RequireProject().Translation.EntryPoints
                .Append(startAddr)
                .Concat(functionMap?.Addresses ?? Array.Empty<uint>()),
            includeGeneratedHistory: false);
        Console.WriteLine($"[translator] Loaded {knownBaseFunctionEntryPoints.Count:N0} known base function start(s) for tail-call boundary detection.");
        if (functionMap is not null)
        {
            Console.WriteLine(
                $"[translator] Function map: {functionMap.Addresses.Count:N0} entry point(s), " +
                $"{functionMap.NamedCount:N0} named ({functionMap.SourcePath}).");
        }

        var nativeGuestEffects = runtimeNativeGuestEffects.Value;
        Console.WriteLine($"[translator] Native guest-effect contracts: {nativeGuestEffects.Contracts.Count:N0} total, " +
                          $"{nativeGuestEffects.PreciseContracts.Count:N0} precise, " +
                          $"{nativeGuestEffects.ConservativeContracts.Count:N0} full-context.");
        var residentTranslationExclusions = baseTranslationExclusions.Value;
        Console.WriteLine(
            $"[translator] Leaf inlining: {leafInliningBlockedTargets.Value.Count:N0} address(es) are not " +
            "inlinable (native, excluded from translation, or mod-patched).");

        var queue = new Queue<(uint addr, int depth)>();
        queue.Enqueue((startAddr, 0));
        visited.Add(startAddr);

        // Map entries the call-graph walk never reaches. They are translated, but
        // on a decode or lift failure they are skipped with a count instead of
        // failing the run: unlike a walked call target, nothing proves the map
        // entry is really code, and one noisy line must not block a release.
        var speculativeSeeds = new Queue<uint>(functionMap?.Addresses ?? Array.Empty<uint>());
        var speculativeSkips = new List<(uint Address, string Reason)>();

        var count = 0;
        var totals = new List<TranslationMetrics>();
        var printedDepths = new HashSet<int>();
        var filesUpdated = 0;
        var filesUnchanged = 0;
        var emittedPaths = pruneStale ? new HashSet<string>(StringComparer.OrdinalIgnoreCase) : null;
        var baseSummaries = new Dictionary<uint, GqrFunctionSummary>();
        var baseWork = new Dictionary<uint, (
            string Name,
            string Path,
            int MaxInstructions,
            int MaxBytes,
            bool AllowUnsupported)>();
        var baseOutputPaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        BaseTranslationOutputMetadata? completedOutputMetadata = null;

        void TrackEmittedPath(string path)
        {
            if (emittedPaths is null)
            {
                return;
            }

            lock (emittedPaths)
            {
                emittedPaths.Add(Path.GetFullPath(path));
            }
        }

        var parallelOptions = new ParallelOptions { MaxDegreeOfParallelism = maxThreads };

        void ProcessQueue(bool speculative = false)
        {
            while (queue.Count > 0)
            {
                var batch = new List<(uint Address, int Depth, string Name)>(batchSize);
                var batchAddresses = new HashSet<uint>();
                var candidates = QueueBatch.Dequeue(
                    queue,
                    batchSize,
                    next => ShouldSkip(next) || !batchAddresses.Add(next.addr));
                foreach (var next in candidates)
                {
                    var fallbackName = $"func_{next.addr:X8}";
                    var name = ResolveFunctionName(next.addr, preferred: null, fallbackName);
                    // Every queued item is a translator-discovered function start.
                    // Register the whole wave before decoding so peer boundaries
                    // are deterministic and cannot be swallowed by batch order.
                    knownBaseFunctionEntryPoints.Add(next.addr);
                    batch.Add((next.addr, next.depth, name));
                }

                if (batch.Count == 0)
                {
                    continue;
                }

                foreach (var depth in batch.Select(b => b.Depth).Distinct().OrderBy(d => d))
                {
                    LogDepth(depth);
                }

                TranslateBatch(batch, speculative);
            }
        }

        // Batches drain one at a time so a callee of a prior speculative entry arrives with a known
        // caller/GQR state instead of as another unknown root. Everything here is speculative, so a
        // "call" decoded from an unreached map entry isn't proven code; treat failures as soft, not fatal.
        void ProcessSpeculativeSeeds()
        {
            while (speculativeSeeds.Count > 0)
            {
                var batch = new List<(uint Address, int Depth, string Name)>(batchSize);
                var candidates = QueueBatch.Dequeue(
                    speculativeSeeds,
                    batchSize,
                    address => translated.Contains(address) || !visited.Add(address));
                foreach (var address in candidates)
                {
                    // Nothing calls this from the translated set, so its entry
                    // GQR state is unconstrained.
                    gqrUnknownEntryRoots.Add(address);
                    batch.Add((address, 0, ResolveFunctionName(address, preferred: null, $"func_{address:X8}")));
                }

                if (batch.Count == 0)
                {
                    continue;
                }

                TranslateBatch(batch, speculative: true);
                ProcessQueue(speculative: true);
            }
        }

        void LogDepth(int depth)
        {
            if (printedDepths.Add(depth))
            {
                Console.WriteLine($"Translating functions on depth {depth}...");
            }
        }

        bool ShouldSkip((uint addr, int depth) item)
        {
            if (translated.Contains(item.addr))
            {
                return true;
            }

            return false;
        }

        // `speculative` selects the failure policy for the whole batch: a walked
        // call target that will not decode is a translator bug and stops the run,
        // while an address the configured map claims - directly, or through the calls
        // decoded out of one - is dropped with a counted warning.
        void TranslateBatch(List<(uint Address, int Depth, string Name)> batch, bool speculative = false)
        {
            var results = new FunctionDiscoveryResult?[batch.Count];
            IndexedParallel.For(batch.Count, parallelOptions, i => results[i] = Attempt(batch[i], speculative));

            // Re-run only translations that swallowed a newly revealed sibling entry, or direct
            // branches could bypass a mod overlay registered there. Both scans are pure per-result
            // so they run on the same worker pool; the dedup filter below keeps first sighting.
            var discoveryTargets = new uint[results.Length][];
            var swallowedSiblingEntries = new uint[results.Length][];
            IndexedParallel.For(results.Length, parallelOptions, i =>
            {
                if (results[i] is not { } result) return;
                discoveryTargets[i] = DiscoverDiscoveryTranslationTargets(result).ToArray();
                swallowedSiblingEntries[i] = DiscoverSwallowedSiblingEntries(result).ToArray();
            });
            var newlyKnownEntries = Enumerable.Range(0, results.Length)
                .Where(i => results[i] is not null)
                .SelectMany(i => discoveryTargets[i].Concat(swallowedSiblingEntries[i]))
                .Where(knownBaseFunctionEntryPoints.Add)
                .ToHashSet();
            if (newlyKnownEntries.Count != 0)
            {
                // The boundary set is final for this batch once the filter above has run,
                // so the affected re-translations are independent of one another.
                var affected = new List<int>();
                for (var i = 0; i < results.Length; i++)
                {
                    if (results[i] is not { } result) continue;
                    var entry = result.EntryPoint;
                    if (result.Instructions.Any(instruction =>
                            instruction.Address != entry && newlyKnownEntries.Contains(instruction.Address)))
                    {
                        affected.Add(i);
                    }
                }

                IndexedParallel.For(affected.Count, parallelOptions, k =>
                {
                    var i = affected[k];
                    results[i] = Attempt(batch[i], speculative);
                    if (results[i] is { } result)
                    {
                        discoveryTargets[i] = DiscoverDiscoveryTranslationTargets(result).ToArray();
                    }
                });
            }

            // The compact GQR projection is a pure walk of the final SSA graph and the
            // canonical store serializes its own dictionary, so both move off the serial
            // commit. Every address in a batch is distinct and untranslated, so the store
            // sees one Put per key regardless of completion order.
            var summaries = new GqrFunctionSummary[results.Length];
            IndexedParallel.For(results.Length, parallelOptions, i =>
            {
                if (results[i] is not { } result) return;
                summaries[i] = GqrFunctionSummary.Create(result.Ssa.Function);
                canonicalIrStore.Put(batch[i].Address, result.Ssa.Function);
            });

            // Update state sequentially (thread-safe)
            for (var i = 0; i < batch.Count; i++)
            {
                if (results[i] is not { } result) continue;
                CommitResult(batch[i], result, discoveryTargets[i], summaries[i]);
            }
        }

        FunctionDiscoveryResult? Attempt((uint Address, int Depth, string Name) work, bool speculative)
        {
            if (!speculative)
            {
                return TranslateWork(work);
            }

            try
            {
                return TranslateWork(work);
            }
            catch (Exception ex)
            {
                lock (speculativeSkips)
                {
                    speculativeSkips.Add((work.Address, ex.Message));
                }

                return null;
            }
        }

        static IEnumerable<uint> DiscoverSwallowedSiblingEntries(FunctionDiscoveryResult result)
        {
            var instructionsByAddress = result.Instructions.ToDictionary(instruction => instruction.Address);
            foreach (var branch in result.Instructions.Where(instruction => instruction.IsUnconditionalBranch))
            {
                foreach (var target in branch.BranchTargets ?? Array.Empty<uint>())
                {
                    if (target == result.EntryPoint ||
                        !instructionsByAddress.TryGetValue(target, out var targetInstruction))
                    {
                        continue;
                    }

                    if (targetInstruction.Mnemonic.Equals("stwu", StringComparison.OrdinalIgnoreCase) &&
                        targetInstruction.OperandText.StartsWith("r1,", StringComparison.OrdinalIgnoreCase))
                    {
                        yield return target;
                    }
                }
            }
        }

        FunctionDiscoveryResult TranslateWork((uint Address, int Depth, string Name) work)
        {
            var options = WithProjectFrontEndPolicy(new TranslationOptions(
                work.Name,
                KnownFunctionEntryPoints: knownBaseFunctionEntryPoints),
                RequireProject().Translation);
            return translator.Value.Discover(work.Address, options);
        }

        void CommitResult(
            (uint Address, int Depth, string Name) work,
            FunctionDiscoveryResult result,
            IReadOnlyList<uint> discoveryTargets,
            GqrFunctionSummary summary)
        {
            // File writing moved to parallel section above
            totals.Add(result.Metrics);
            count++;
            translated.Add(work.Address);
            translatedFunctionEnds[work.Address] = GetFunctionInstructionEnd(result);
            translatedFlowCoverage[work.Address] = CoalesceInstructionRanges(result);

            foreach (var target in discoveryTargets)
            {
                knownBaseFunctionEntryPoints.Add(target);
                if (visited.Add(target))
                {
                    queue.Enqueue((target, work.Depth + 1));
                }
            }

            // Discovery needs decoded PPC instructions and SSA temporarily. The
            // whole-program pass only needs its compact GQR projection; release
            // the full graph here and rebuild it once, with final facts, during
            // the emission wave.
            var outputPath = Path.Combine(outDir, $"{work.Name}.cpp");
            if (!baseOutputPaths.Add(Path.GetFullPath(outputPath)))
            {
                throw new InvalidOperationException(
                    $"Translator output path collision for '{outputPath}' while retaining function 0x{work.Address:X8}.");
            }
            // Both the projection and the canonical encoding were produced by the
            // parallel stage above; this only records them.
            baseSummaries[work.Address] = summary;
            baseWork[work.Address] = (
                work.Name,
                outputPath,
                TranslationOptions.Default.MaxInstructions,
                TranslationOptions.Default.MaxBytes,
                allowUnsupportedInstructions);
        }

        // The call-graph walk from the entry point, then everything the configured map
        // knows about that the walk never reached. Those two sources replace the
        // constructor, vtable, function-pointer and adjacent-prologue scans, which
        // guessed at exactly this set - and the hand-maintained force-translate
        // list that existed to paper over the addresses they still missed.
        ProcessQueue();
        var walkReachable = count;
        ProcessSpeculativeSeeds();
        if (functionMap is not null)
        {
            Console.WriteLine(
                $"[translator] Function starts: {walkReachable:N0} reached by the call-graph walk, " +
                $"{count - walkReachable:N0} added speculatively from the map.");
            if (speculativeSkips.Count != 0)
            {
                foreach (var (address, reason) in speculativeSkips.OrderBy(static skip => skip.Address).Take(20))
                {
                    Console.WriteLine($"[translator]   skipped map entry 0x{address:X8}: {reason}");
                }
                Console.WriteLine(
                    $"[translator] Skipped {speculativeSkips.Count:N0} map entry/entries that did not translate; " +
                    "these are seeded only by the map and are never reached by a call.");
            }
        }

        if (baseSummaries.Count > 0)
        {
            // Wall-clock accounting for the serial whole-program section; prints
            // per-phase elapsed time so regressions are visible in build logs.
            var lastPhaseSeconds = 0d;
            void LogPhase(string label)
            {
                var now = (DateTime.Now - startTime).TotalSeconds;
                Console.WriteLine($"[translator] phase '{label}': {now - lastPhaseSeconds:F1}s (t={now:F1}s)");
                lastPhaseSeconds = now;
            }
            LogPhase("discovery");
            // Function starts are discovered from the current inputs, not from a
            // previous generated tree. If a later discovery source identified a
            // boundary inside an earlier decode, rebuild only that owner's compact
            // summary with the final deterministic boundary set.
            var finalBoundaries = knownBaseFunctionEntryPoints.OrderBy(address => address).ToArray();
            var boundaryRepairAddresses = translatedFunctionEnds
                .Where(pair => ContainsInteriorBoundary(pair.Key, pair.Value, finalBoundaries))
                .Select(pair => pair.Key)
                .OrderBy(address => address)
                .ToArray();
            if (boundaryRepairAddresses.Length != 0)
            {
                Console.WriteLine($"[translator] Reanalyzing {boundaryRepairAddresses.Length:N0} function(s) affected by later-discovered boundaries...");
                var repaired = new FunctionDiscoveryResult[boundaryRepairAddresses.Length];
                IndexedParallel.For(boundaryRepairAddresses.Length, parallelOptions, i =>
                {
                    var address = boundaryRepairAddresses[i];
                    var work = baseWork[address];
                    repaired[i] = translator.Value.Discover(address, WithProjectFrontEndPolicy(new TranslationOptions(
                        work.Name,
                        MaxInstructions: work.MaxInstructions,
                        MaxBytes: work.MaxBytes,
                        AllowUnsupportedInstructions: work.AllowUnsupported,
                        KnownFunctionEntryPoints: knownBaseFunctionEntryPoints),
                        RequireProject().Translation));
                });
                for (var i = 0; i < boundaryRepairAddresses.Length; i++)
                {
                    var address = boundaryRepairAddresses[i];
                    baseSummaries[address] = GqrFunctionSummary.Create(repaired[i].Ssa.Function);
                    canonicalIrStore.Put(address, repaired[i].Ssa.Function);
                    translatedFunctionEnds[address] = GetFunctionInstructionEnd(repaired[i]);
                    translatedFlowCoverage[address] = CoalesceInstructionRanges(repaired[i]);
                }
            }

            LogPhase("boundary repair");

            // Entries another translation's own flow executes through. Branch
            // targets surface as block labels and are excluded downstream by the
            // manifest builder's label rule; this catches the label-less
            // fall-through case (e.g. a function map's split-switch artifact),
            // which must not split the patch-homing base function ranges.
            var interiorEntryPoints = ComputeInteriorEntryPoints(translatedFlowCoverage);

            {
                var inlinedSites = totals.Sum(static metrics => metrics.InlinedCallSites);
                var inlinedFunctions = totals.Count(static metrics => metrics.InlinedCallSites > 0);
                var inlinedGuestInstructions = totals.Sum(static metrics => metrics.InlinedGuestInstructions);
                Console.WriteLine(
                    $"[translator] Leaf inlining: {inlinedSites:N0} direct call site(s) spliced across " +
                    $"{inlinedFunctions:N0} function(s), {inlinedGuestInstructions:N0} guest instruction(s) duplicated.");
            }

            Console.WriteLine("[translator] Computing interprocedural GQR constants and callee write summaries...");
            var gqrAnalysis = GqrInterproceduralAnalysis.Analyze(
                baseSummaries,
                gqrUnknownEntryRoots);
            var directGqrTargets = baseSummaries.Values
                .SelectMany(summary => summary.DirectGuestTargets)
                .Where(baseSummaries.ContainsKey)
                .ToHashSet();
            var guardedRoots = baseSummaries.Keys.Where(address => !directGqrTargets.Contains(address)).ToHashSet();
            guardedRoots.Add(startAddr);
            var guardedGqrAnalysis = GqrInterproceduralAnalysis.Analyze(
                baseSummaries,
                guardedRoots);
            var gqrSpecializedFunctions = gqrAnalysis.EntryConstants.Count(pair => pair.Value.Count != 0);
            var gqrSpecializedRegisters = gqrAnalysis.EntryConstants.Sum(pair => pair.Value.Count);
            var guardedGqrFunctions = guardedGqrAnalysis.EntryConstants.Count(pair =>
                pair.Value.Count > gqrAnalysis.EntryConstants[pair.Key].Count);
            Console.WriteLine($"[translator] Proven GQR entry constants: {gqrSpecializedRegisters:N0} across {gqrSpecializedFunctions:N0} function(s); reachable functions: {gqrAnalysis.ReachableFunctions.Count:N0}.");
            Console.WriteLine($"[translator] Runtime-guarded direct-call GQR contexts: {guardedGqrFunctions:N0} function(s).");
            LogPhase("GQR interprocedural x2");

            var emissionAddresses = baseSummaries.Keys.OrderBy(address => address).ToArray();
            var localGuestAbiContracts = new Dictionary<uint, GuestAbiContract>(emissionAddresses.Length);
            var canonicalGuestFunctions = new Dictionary<uint, IrFunction>(emissionAddresses.Length);
            var irInstructionCounts = new Dictionary<uint, int>(emissionAddresses.Length);
            // Each slot is an independent pure decode plus contract analysis, so
            // compute them in parallel into position-indexed arrays and fill the
            // dictionaries afterwards in the original ascending order. Dictionary
            // insertion order is preserved, so every downstream enumeration of
            // these maps is byte-for-byte the same as the serial version.
            var canonicalByIndex = new IrFunction[emissionAddresses.Length];
            var localContractByIndex = new GuestAbiContract[emissionAddresses.Length];
            var irInstructionCountByIndex = new int[emissionAddresses.Length];
            IndexedParallel.For(emissionAddresses.Length, parallelOptions, i =>
            {
                var address = emissionAddresses[i];
                if (!canonicalIrStore.TryGet(address, out var canonicalSsa))
                    throw new InvalidOperationException($"Missing canonical SSA while building ABI contract for 0x{address:X8}.");
                canonicalByIndex[i] = canonicalSsa;
                localContractByIndex[i] = GuestAbiContractAnalyzer.Analyze(canonicalSsa);
                irInstructionCountByIndex[i] = canonicalSsa.Blocks.Sum(block => block.Instructions.Count);
            });
            for (var i = 0; i < emissionAddresses.Length; i++)
            {
                var address = emissionAddresses[i];
                canonicalGuestFunctions[address] = canonicalByIndex[i];
                localGuestAbiContracts[address] = localContractByIndex[i];
                irInstructionCounts[address] = irInstructionCountByIndex[i];
            }

            LogPhase("canonical decode + local contracts");

            // Compose guest calls in execution order. This preserves a callee
            // input only when it is still read-before-write at the caller entry,
            // rather than retaining every descendant dependency forever.
            var interproceduralGuestAbi = GuestAbiInterproceduralAnalyzer.Analyze(
                canonicalGuestFunctions,
                nativeGuestEffects.Contracts);
            var guestAbiContracts = interproceduralGuestAbi.Contracts
                .ToDictionary(static pair => pair.Key, static pair => pair.Value);
            var recursiveComponents = interproceduralGuestAbi.StronglyConnectedComponents
                .Count(component => component.Count > 1 ||
                    guestAbiContracts[component[0]].DirectCallTargets.Contains(component[0]));
            Console.WriteLine(
                $"[translator] SSA guest-state call graph: " +
                $"{interproceduralGuestAbi.StronglyConnectedComponents.Count:N0} component(s), " +
                $"{recursiveComponents:N0} recursive.");

            LogPhase("interprocedural guest ABI");

            // Calls to a registered native observe the native implementation's
            // contract, not the excluded PPC body that happens to share its address.
            foreach (var pair in nativeGuestEffects.Contracts)
                guestAbiContracts[pair.Key] = pair.Value;

            // Determine the state that is actually consumed after each direct
            // call. Public CpuContext wrappers retain the complete architectural
            // behavior; native internal entries return only this union of
            // call-site-live values.
            var demandedStateFreeOutputs = new Dictionary<uint, GuestStateMask>();
            var stateLivenessByFunction = new Dictionary<uint, GuestStateLivenessResult>(emissionAddresses.Length);
            // The per-function liveness solve is pure; only the demanded-output
            // union is order sensitive, so that stays sequential in ascending
            // address order below.
            var livenessByIndex = new GuestStateLivenessResult[emissionAddresses.Length];
            IndexedParallel.For(emissionAddresses.Length, parallelOptions, i =>
            {
                var address = emissionAddresses[i];
                var contract = guestAbiContracts[address];
                // Start from the public CpuContext wrapper, not the ABI return masks alone: masks-only
                // let compact descendant variants drop live volatile GPR/FPR/CR/LR effects (e.g. f2-f4
                // through 0x8022F80C -> 0x80085040) that aren't source-language return values.
                var exitLive = GuestStateLivenessAnalyzer.MaterializedContextExit(contract);
                livenessByIndex[i] = GuestStateLivenessAnalyzer.Analyze(
                    canonicalGuestFunctions[address], guestAbiContracts, exitLive);
            });
            for (var i = 0; i < emissionAddresses.Length; i++)
            {
                var address = emissionAddresses[i];
                var liveness = livenessByIndex[i];
                stateLivenessByFunction[address] = liveness;
                foreach (var call in liveness.DirectCalls)
                {
                    demandedStateFreeOutputs[call.Target] = demandedStateFreeOutputs
                        .GetValueOrDefault(call.Target)
                        .Union(call.Outputs);
                }
            }

            LogPhase("guest state liveness");

            // Residency, boundary narrowing and the stack elisions all come from
            // WithProjectCodegenPolicy below, which every emitting path shares.
            var residentPolicy = RequireProject().Translation;
            var modOverridableCallTargets = new HashSet<uint>();
            var directlyCalledGuestFunctions = localGuestAbiContracts.Values
                .SelectMany(static contract => contract.DirectCallTargets)
                .Where(localGuestAbiContracts.ContainsKey)
                .ToHashSet();
            static int StateFreeInputValueCount(GuestAbiContract contract) =>
                BitOperations.PopCount(contract.GprReadBeforeWriteMask) +
                BitOperations.PopCount(contract.FprReadBeforeWriteMask | contract.FprPossibleWriteMask) +
                ((contract.CrReadBeforeWriteMask | contract.CrPossibleWriteMask) != 0 ? 1 : 0) +
                (contract.ReadsXerBeforeWrite || contract.MayWriteXer ? 1 : 0) +
                (contract.ReadsCtrBeforeWrite || contract.MayWriteCtr ? 1 : 0) +
                (contract.ReadsLrBeforeWrite || contract.MayWriteLr ? 1 : 0) +
                (contract.ReadsFpscrBeforeWrite || contract.MayWriteFpscr ? 1 : 0) +
                BitOperations.PopCount((uint)(contract.GqrReadBeforeWriteMask | contract.GqrPossibleWriteMask)) +
                BitOperations.PopCount((uint)(contract.HidReadBeforeWriteMask | contract.HidPossibleWriteMask));
            static int StateFreeOutputValueCount(GuestAbiContract contract) =>
                BitOperations.PopCount(contract.GprPossibleWriteMask) +
                BitOperations.PopCount(contract.FprPossibleWriteMask) +
                (contract.CrPossibleWriteMask != 0 ? 1 : 0) +
                (contract.MayWriteXer ? 1 : 0) + (contract.MayWriteCtr ? 1 : 0) +
                (contract.MayWriteLr ? 1 : 0) + (contract.MayWriteFpscr ? 1 : 0) +
                BitOperations.PopCount((uint)contract.GqrPossibleWriteMask) +
                BitOperations.PopCount((uint)contract.HidPossibleWriteMask);

            GuestAbiContract PlannedStateFreeContract(uint address)
            {
                var publicContract = guestAbiContracts[address];
                var demandedOutputs = demandedStateFreeOutputs.GetValueOrDefault(address);
                var demandedGprOutputs = publicContract.GprPossibleWriteMask & demandedOutputs.Gpr;
                return publicContract with
                {
                    // A may-write value is also an input unless every exit
                    // overwrites it. On the no-write path the native result
                    // must return the incoming architectural value.
                    GprReadBeforeWriteMask = GuestStateLivenessAnalyzer.RequiredStateFreeGprInputs(
                        publicContract, demandedOutputs.Gpr),
                    GprPossibleWriteMask = demandedGprOutputs,
                    FprPossibleWriteMask = publicContract.FprPossibleWriteMask & demandedOutputs.Fpr,
                    CrPossibleWriteMask = (byte)(publicContract.CrPossibleWriteMask & demandedOutputs.Cr),
                    MayWriteXer = publicContract.MayWriteXer && demandedOutputs.Xer,
                    MayWriteCtr = publicContract.MayWriteCtr && demandedOutputs.Ctr,
                    MayWriteLr = publicContract.MayWriteLr && demandedOutputs.Lr,
                    MayWriteFpscr = publicContract.MayWriteFpscr && demandedOutputs.Fpscr,
                    GqrPossibleWriteMask = (byte)(publicContract.GqrPossibleWriteMask & demandedOutputs.Gqr),
                    HidPossibleWriteMask = (byte)(publicContract.HidPossibleWriteMask & demandedOutputs.Hid)
                };
            }

            // Admit closed direct-call components as a unit. A region is
            // state-free only when all of its translated descendants can use
            // the same representation; external/native targets remain real
            // synchronization boundaries and exclude that component for now.
            var components = interproceduralGuestAbi.StronglyConnectedComponents;
            var componentByAddress = components
                .SelectMany(static (component, index) => component.Select(address => (address, index)))
                .ToDictionary(static pair => pair.address, static pair => pair.index);
            var eligibleComponents = components
                .Select((component, index) => new
                {
                    index,
                    eligible = component.All(address =>
                        !residentTranslationExclusions.Contains(address) &&
                        !localGuestAbiContracts[address].HasFullSynchronizationFence &&
                        GuestStateLivenessAnalyzer.CanDeconstructWithoutContext(canonicalGuestFunctions[address]) &&
                        (!directlyCalledGuestFunctions.Contains(address) ||
                         (StateFreeInputValueCount(PlannedStateFreeContract(address)) <= 4 &&
                          StateFreeOutputValueCount(PlannedStateFreeContract(address)) <= 2)))
                })
                .Where(static item => item.eligible)
                .Select(static item => item.index)
                .ToHashSet();
            bool removedComponent;
            do
            {
                removedComponent = false;
                foreach (var componentIndex in eligibleComponents.ToArray())
                {
                    var hasUnsafeDescendant = components[componentIndex]
                        .SelectMany(address => localGuestAbiContracts[address].DirectCallTargets)
                        .Any(target => !componentByAddress.TryGetValue(target, out var targetComponent) ||
                                       !eligibleComponents.Contains(targetComponent));
                    if (!hasUnsafeDescendant) continue;
                    eligibleComponents.Remove(componentIndex);
                    removedComponent = true;
                }
            } while (removedComponent);
            var stateFreeAbiFunctions = disableStateFreeAbi
                ? new HashSet<uint>()
                : eligibleComponents
                    .SelectMany(index => components[index])
                    .Where(directlyCalledGuestFunctions.Contains)
                    .ToHashSet();
            Console.WriteLine(
                $"[translator] Automatic state-free region ABI: {stateFreeAbiFunctions.Count:N0} directly called function(s) " +
                $"across {eligibleComponents.Count:N0} closed component(s).");

            // The two suffixes are appended to the same sanitized stem rather than
            // derived from each other: rewriting "_resident" into "_statefree"
            // with an unanchored Replace also rewrote any guest symbol whose own
            // name happened to contain that substring.
            static string CxxSymbolStem(string name)
            {
                var builder = new StringBuilder(name.Length + 16);
                foreach (var ch in name.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? name[2..] : name)
                    builder.Append(char.IsLetterOrDigit(ch) || ch == '_' ? ch : '_');
                if (builder.Length == 0 || !(char.IsLetter(builder[0]) || builder[0] == '_'))
                    builder.Insert(0, "func_");
                return builder.ToString();
            }
            var stateFreeCallSymbols = stateFreeAbiFunctions.ToDictionary(
                address => address,
                address => CxxSymbolStem(baseWork[address].Name) + "_statefree");
            var stateFreeCallSiteVariantsByCaller = new Dictionary<
                uint, IReadOnlyDictionary<GuestStateFreeCallSiteKey, GuestStateFreeCallVariant>>();
            var stateFreeEntryVariantsByTarget = new Dictionary<
                uint, IReadOnlyList<GuestStateFreeCallVariant>>();
            Dictionary<string, uint> SelectGqrConstants(uint address, out bool useGuardedConstants)
            {
                var safeConstants = gqrAnalysis.EntryConstants[address];
                var guardedConstants = guardedGqrAnalysis.EntryConstants[address];
                useGuardedConstants = guardedConstants.Count > safeConstants.Count;
                return new Dictionary<string, uint>(
                    useGuardedConstants ? guardedConstants : safeConstants,
                    StringComparer.OrdinalIgnoreCase);
            }

            TranslationOptions BuildEmissionOptions(
                uint address,
                IReadOnlyDictionary<uint, GuestAbiContract> stateFreeAbiContracts)
            {
                var work = baseWork[address];
                var selectedGqrConstants = SelectGqrConstants(address, out var useGuardedConstants);
                return WithProjectCodegenPolicy(
                    new TranslationOptions(
                        work.Name,
                        MaxInstructions: work.MaxInstructions,
                        MaxBytes: work.MaxBytes,
                        AllowUnsupportedInstructions: work.AllowUnsupported,
                        KnownFunctionEntryPoints: knownBaseFunctionEntryPoints,
                        GqrEntryConstants: selectedGqrConstants,
                        GqrCalleeWriteMasks: gqrAnalysis.WriteMasks,
                        GqrConstantsRequireRuntimeGuard: useGuardedConstants && selectedGqrConstants.Count != 0,
                        GuestAbiContracts: guestAbiContracts,
                        EmitStateFreeLeafVariant: stateFreeAbiFunctions.Contains(address),
                        StateFreeAbiContracts: stateFreeAbiContracts,
                        StateFreeCallSymbols: stateFreeCallSymbols,
                        StateFreeCallSiteVariants: stateFreeCallSiteVariantsByCaller.GetValueOrDefault(address),
                         StateFreeEntryVariants: stateFreeEntryVariantsByTarget.GetValueOrDefault(address),
                         ModOverridableCallTargets: modOverridableCallTargets),
                    residentPolicy,
                    address);
            }

            // Populate the recursive ABI inference cache once, in stable global order, before
            // refinement and parallel emission, so cyclic call graphs don't depend on thread count.
            LogPhase("state-free selection + leaf residency");
            ReportAbiPrewarm(inferredGuestAbi.Value.Prewarm(emissionAddresses));
            LogPhase("ABI prewarm");

            // State-free entries use the transformed leaf's exact live-at-entry
            // interface rather than the public architectural contract. Generate
            // that signature first so direct callers never receive a state pointer
            // or unrelated register dependencies.
            var stateFreeAbiContracts = new Dictionary<uint, GuestAbiContract>(stateFreeAbiFunctions.Count);
            var stateFreeInterfaceLeaves = stateFreeAbiFunctions
                .OrderBy(static address => address)
                .ToArray();
            // Seed every admitted signature before lowering any body. A caller
            // must never fall back to CpuContext merely because its callee has a
            // numerically higher address and has not reached the prepass yet.
            foreach (var address in stateFreeInterfaceLeaves)
                stateFreeAbiContracts[address] = PlannedStateFreeContract(address);

            GuestAbiContract RefineStateFreeContract(uint address)
            {
                if (!canonicalIrStore.TryGet(address, out var canonicalSsa))
                    throw new InvalidOperationException($"Missing canonical SSA for state-free ABI prepass at 0x{address:X8}.");
                var analyzed = translator.Value.LowerCanonical(
                    address,
                    canonicalSsa,
                    BuildEmissionOptions(address, stateFreeAbiContracts));
                // The emitter reports the refined interface directly now. The RECOMP_STATE_FREE_ABI
                // comment it still emits is only for cross-run consumers that parse source files.
                var emitted = analyzed.Emission?.StateFree;
                if (emitted is null || emitted.EntryPoint != address)
                    throw new InvalidOperationException($"Missing state-free ABI metadata for 0x{address:X8}.");
                var plannedContract = PlannedStateFreeContract(address);
                return plannedContract with
                {
                    GprReadBeforeWriteMask = emitted.GprInputMask,
                    GprPossibleWriteMask = emitted.Contract.GprPossibleWriteMask & plannedContract.GprPossibleWriteMask,
                    FprReadBeforeWriteMask = emitted.Contract.FprReadBeforeWriteMask,
                    FprPossibleWriteMask = emitted.Contract.FprPossibleWriteMask & plannedContract.FprPossibleWriteMask,
                    CrReadBeforeWriteMask = emitted.Contract.CrReadBeforeWriteMask,
                    GqrReadBeforeWriteMask = emitted.Contract.GqrReadBeforeWriteMask,
                    HidReadBeforeWriteMask = emitted.Contract.HidReadBeforeWriteMask,
                    HidPossibleWriteMask = (byte)(emitted.Contract.HidPossibleWriteMask & plannedContract.HidPossibleWriteMask),
                    ReadsXerBeforeWrite = emitted.Contract.ReadsXerBeforeWrite,
                    ReadsCtrBeforeWrite = emitted.Contract.ReadsCtrBeforeWrite,
                    ReadsLrBeforeWrite = emitted.Contract.ReadsLrBeforeWrite
                };
            }

            // Refine callees before callers. Recursive SCC members are solved
            // together to a stable signature; the component graph itself is a
            // DAG, so every ordinary component needs only one lowering pass.
            var eligibleComponentEdges = eligibleComponents.ToDictionary(
                componentIndex => componentIndex,
                componentIndex => components[componentIndex]
                    .SelectMany(address => localGuestAbiContracts[address].DirectCallTargets)
                    .Where(componentByAddress.ContainsKey)
                    .Select(target => componentByAddress[target])
                    .Where(targetComponent => targetComponent != componentIndex && eligibleComponents.Contains(targetComponent))
                    .Distinct()
                    .ToArray());
            var callersByComponent = eligibleComponents.ToDictionary(
                componentIndex => componentIndex,
                static _ => new List<int>());
            foreach (var (callerComponent, calleeComponents) in eligibleComponentEdges)
                foreach (var calleeComponent in calleeComponents)
                    callersByComponent[calleeComponent].Add(callerComponent);
            var pendingCallees = eligibleComponentEdges.ToDictionary(
                static pair => pair.Key,
                static pair => pair.Value.Length);
            var readyComponents = new SortedSet<int>(pendingCallees
                .Where(static pair => pair.Value == 0)
                .Select(static pair => pair.Key));
            var processedComponents = 0;
            while (readyComponents.Count != 0)
            {
                var componentIndex = readyComponents.Min;
                readyComponents.Remove(componentIndex);
                ++processedComponents;
                var members = components[componentIndex]
                    .Where(stateFreeAbiFunctions.Contains)
                    .OrderBy(static address => address)
                    .ToArray();
                var changed = true;
                var refinementPass = 0;
                while (changed)
                {
                    if (++refinementPass > 64)
                        throw new InvalidOperationException($"State-free ABI signatures did not converge for component {componentIndex}.");
                    changed = false;
                    foreach (var address in members)
                    {
                        var refined = RefineStateFreeContract(address);
                        if (stateFreeAbiContracts[address] == refined) continue;
                        stateFreeAbiContracts[address] = refined;
                        changed = true;
                    }
                }

                foreach (var callerComponent in callersByComponent[componentIndex])
                    if (--pendingCallees[callerComponent] == 0)
                        readyComponents.Add(callerComponent);
            }
            if (processedComponents != eligibleComponents.Count)
                throw new InvalidOperationException("State-free component graph was not acyclic after SCC condensation.");

            // A single union signature recreates CpuContext traffic as Win x64 stack args/hidden
            // struct-return. Materialize compact variants instead; the four-input/two-output cap
            // fits the native ABI, larger regions need inlining/fusion, not another oversized signature.
            static string StateMaskSortKey(GuestStateMask mask) =>
                $"{mask.Gpr:X8}:{mask.Fpr:X8}:{mask.Cr:X2}:{(mask.Xer ? 1 : 0)}:{(mask.Ctr ? 1 : 0)}:" +
                $"{(mask.Lr ? 1 : 0)}:{(mask.Fpscr ? 1 : 0)}:{mask.Gqr:X2}:{mask.Hid:X2}";
            var stateFreeLivenessContracts = new Dictionary<uint, GuestAbiContract>(guestAbiContracts);
            foreach (var (address, contract) in stateFreeAbiContracts)
                stateFreeLivenessContracts[address] = contract;
            var callsByTarget = stateLivenessByFunction
                .SelectMany(pair => pair.Value.DirectCalls.Select(call => (Caller: pair.Key, Call: call)))
                .Where(item => stateFreeAbiFunctions.Contains(item.Call.Target))
                .GroupBy(static item => item.Call.Target)
                .OrderBy(static group => group.Key);
            var mutableCallSiteVariants = new Dictionary<
                uint, Dictionary<GuestStateFreeCallSiteKey, GuestStateFreeCallVariant>>();
            var compactVariantCount = 0;
            foreach (var targetGroup in callsByTarget)
            {
                var target = targetGroup.Key;
                var variantsByOutput = new Dictionary<GuestStateMask, GuestStateFreeCallVariant>();
                var rankedOutputs = targetGroup
                    .GroupBy(static item => item.Call.Outputs)
                    .OrderByDescending(static group => group.Count())
                    .ThenBy(group => StateMaskSortKey(group.Key), StringComparer.Ordinal)
                    .Take(4)
                    .ToArray();
                var variantIndex = 0;
                foreach (var outputGroup in rankedOutputs)
                {
                    var output = outputGroup.Key;
                    var targetFunction = canonicalGuestFunctions[target];
                    var entryLive = GuestStateLivenessAnalyzer.Analyze(
                        targetFunction, stateFreeLivenessContracts, output).BlockLiveIn[targetFunction.EntryLabel];
                    var publicContract = guestAbiContracts[target];
                    var specializedContract = publicContract with
                    {
                        GprReadBeforeWriteMask = entryLive.Gpr,
                        FprReadBeforeWriteMask = entryLive.Fpr,
                        CrReadBeforeWriteMask = entryLive.Cr,
                        ReadsXerBeforeWrite = entryLive.Xer,
                        ReadsCtrBeforeWrite = entryLive.Ctr,
                        ReadsLrBeforeWrite = entryLive.Lr ||
                            targetFunction.Blocks.SelectMany(static block => block.Instructions)
                                .OfType<IrCall>()
                                .Any(static call => string.IsNullOrWhiteSpace(call.Destination)),
                        ReadsFpscrBeforeWrite = entryLive.Fpscr,
                        // The state-free refinement pass also discovers GQR
                        // reads hidden inside templated PSQ helpers.  Preserve
                        // those body-derived inputs in every narrower variant;
                        // otherwise its definition and caller prototype disagree.
                        GqrReadBeforeWriteMask = (byte)(entryLive.Gqr |
                            stateFreeAbiContracts[target].GqrReadBeforeWriteMask),
                        HidReadBeforeWriteMask = entryLive.Hid,
                        GprPossibleWriteMask = publicContract.GprPossibleWriteMask & output.Gpr,
                        FprPossibleWriteMask = publicContract.FprPossibleWriteMask & output.Fpr,
                        CrPossibleWriteMask = (byte)(publicContract.CrPossibleWriteMask & output.Cr),
                        MayWriteXer = publicContract.MayWriteXer && output.Xer,
                        MayWriteCtr = publicContract.MayWriteCtr && output.Ctr,
                        MayWriteLr = publicContract.MayWriteLr && output.Lr,
                        MayWriteFpscr = publicContract.MayWriteFpscr && output.Fpscr,
                        GqrPossibleWriteMask = (byte)(publicContract.GqrPossibleWriteMask & output.Gqr),
                        HidPossibleWriteMask = (byte)(publicContract.HidPossibleWriteMask & output.Hid)
                    };
                    if (StateFreeInputValueCount(specializedContract) > 4 ||
                        StateFreeOutputValueCount(specializedContract) > 2 ||
                        irInstructionCounts[target] > 96 ||
                        specializedContract == stateFreeAbiContracts[target])
                        continue;
                    var variant = new GuestStateFreeCallVariant(
                        target,
                        $"{stateFreeCallSymbols[target]}_v{variantIndex++}",
                        specializedContract);
                    variantsByOutput[output] = variant;
                    ++compactVariantCount;
                }
                if (variantsByOutput.Count == 0) continue;
                stateFreeEntryVariantsByTarget[target] = variantsByOutput.Values
                    .OrderBy(static variant => variant.Symbol, StringComparer.Ordinal)
                    .ToArray();
                foreach (var item in targetGroup)
                {
                    if (!variantsByOutput.TryGetValue(item.Call.Outputs, out var variant)) continue;
                    if (!mutableCallSiteVariants.TryGetValue(item.Caller, out var callerVariants))
                    {
                        callerVariants = new Dictionary<GuestStateFreeCallSiteKey, GuestStateFreeCallVariant>();
                        mutableCallSiteVariants[item.Caller] = callerVariants;
                    }
                    callerVariants[new GuestStateFreeCallSiteKey(
                        item.Call.BlockLabel, item.Call.Target, item.Call.CallOrdinal)] = variant;
                }
            }
            foreach (var (caller, variants) in mutableCallSiteVariants)
                stateFreeCallSiteVariantsByCaller[caller] = variants;

            if (stateFreeInterfaceLeaves.Length != 0)
                Console.WriteLine($"[translator] Derived {stateFreeInterfaceLeaves.Length:N0} state-free interfaces and " +
                                  $"{compactVariantCount:N0} compact call-site specialization(s) from liveness.");
            LogPhase("state-free refinement");

            var outputMetadata = outputMetadataPath is null
                ? null
                : new BaseTranslationFunctionMetadata[emissionAddresses.Length];
            using var bundleWriter = productionSourceBundlePath is null
                ? null
                : TranslationSourceBundle.CreateWriter(productionSourceBundlePath, emissionAddresses.Length);
            // Emission is one barrier-free parallel pass: waves used to re-synchronise every few
            // hundred functions and ran only as fast as the slowest straggler. Now each worker
            // publishes into an index slot and a single writer appends in ascending index order,
            // so the bundle stays byte-identical without workers ever waiting on each other.
            var collectSources = bundleWriter is not null;
            var pendingSources = new string?[collectSources ? emissionAddresses.Length : 0];
            // `emissionDone[i]` is published with a release write after the source
            // slot is filled, so the writer's acquire read of it also makes the
            // source visible. `completionPulse` only wakes the writer; it carries
            // no state of its own.
            var emissionDone = new bool[pendingSources.Length];
            using var completionPulse = new ManualResetEventSlim(false);
            var emissionAborted = false;
            Task? pendingBundleWrite = null;
            try
            {
                if (bundleWriter is not null && emissionAddresses.Length != 0)
                {
                    TranslationSourceBundleWriter writer = bundleWriter;
                    // A dedicated thread rather than a pool thread: this task now
                    // lives for the whole emission and spends most of it blocked
                    // waiting on the next index, which is exactly the shape the
                    // thread pool is worst at while the parallel pass saturates it.
                    pendingBundleWrite = Task.Factory.StartNew(
                        () =>
                        {
                            for (var nextToWrite = 0; nextToWrite < emissionAddresses.Length; nextToWrite++)
                            {
                                while (!Volatile.Read(ref emissionDone[nextToWrite]))
                                {
                                    // Reset before re-testing so a completion signalled
                                    // between the test and the wait can never be lost,
                                    // and re-test the abort flag so a failed emission
                                    // never leaves this task waiting on an index that
                                    // will never arrive.
                                    completionPulse.Reset();
                                    if (Volatile.Read(ref emissionDone[nextToWrite])) break;
                                    if (Volatile.Read(ref emissionAborted)) return;
                                    completionPulse.Wait();
                                }

                                var address = emissionAddresses[nextToWrite];
                                writer.Write(new TranslationSourceBundleEntry(
                                    address,
                                    Path.GetRelativePath(outDir, baseWork[address].Path).Replace('\\', '/'),
                                    pendingSources[nextToWrite]!));
                                // Drop the reference once appended so completed-but-
                                // unwritten source stays bounded by how far the writer
                                // trails the workers.
                                pendingSources[nextToWrite] = null;
                            }
                        },
                        CancellationToken.None,
                        TaskCreationOptions.LongRunning,
                        TaskScheduler.Default);
                }

                try
                {
                    IndexedParallel.For(emissionAddresses.Length, parallelOptions, i =>
                    {
                        var address = emissionAddresses[i];
                        var work = baseWork[address];
                        var options = BuildEmissionOptions(address, stateFreeAbiContracts);
                        if (!canonicalIrStore.TryGet(address, out var canonicalSsa))
                            throw new InvalidOperationException($"Missing canonical SSA for final lowering at 0x{address:X8}.");
                        var emitted = translator.Value.LowerCanonical(address, canonicalSsa, options);
                        var path = work.Path;
                        if (outputMetadata is not null)
                        {
                            var metadata = BaseTranslationFunctionMetadata.FromTranslation(outDir, path, emitted);
                            if (interiorEntryPoints.Contains(address))
                            {
                                metadata = metadata with { InteriorToOtherTranslation = true };
                            }
                            outputMetadata[i] = metadata;
                        }
                        if (collectSources)
                        {
                            pendingSources[i] = emitted.CxxCode;
                            Volatile.Write(ref emissionDone[i], true);
                            completionPulse.Set();
                        }
                        if (emitFunctionFiles)
                        {
                            if (FileOutput.WriteTextIfChanged(path, emitted.CxxCode)) Interlocked.Increment(ref filesUpdated);
                            else Interlocked.Increment(ref filesUnchanged);
                            TrackEmittedPath(path);
                        }
                    });
                }
                catch
                {
                    // Release the writer before unwinding: the index it is waiting
                    // on may never complete now.
                    Volatile.Write(ref emissionAborted, true);
                    completionPulse.Set();
                    throw;
                }

                var last = pendingBundleWrite;
                pendingBundleWrite = null;
                last?.GetAwaiter().GetResult();
            }
            finally
            {
                // Only reached with work still in flight when emission itself threw.
                // Wait without observing the writer's own fault so the original
                // failure propagates, but never let the task outlive the writer the
                // enclosing `using` is about to dispose.
                if (pendingBundleWrite is not null)
                {
                    Task.WaitAny(pendingBundleWrite);
                }
            }
            bundleWriter?.Complete();
            LogPhase("emission wave + bundle write");
            if (outputMetadata is not null)
            {
                var bundleReference = productionSourceBundlePath is null
                    ? null
                    : Path.GetRelativePath(
                        Path.GetDirectoryName(Path.GetFullPath(outputMetadataPath!))!,
                        Path.GetFullPath(productionSourceBundlePath)).Replace('\\', '/');
                var metadataUnsupportedInstructionCount = emissionAddresses.Sum(address =>
                {
                    if (!canonicalIrStore.TryGet(address, out var canonicalSsa))
                    {
                        throw new InvalidOperationException(
                            $"Missing canonical SSA while computing translation quality for 0x{address:X8}.");
                    }
                    return canonicalSsa.Blocks.Sum(block =>
                        block.Instructions.Count(static instruction => instruction is IrUndefined));
                });
                completedOutputMetadata = BaseTranslationOutputMetadata.Create(
                    outputMetadata,
                    new TranslationQualityMetadata(
                        metadataUnsupportedInstructionCount,
                        InvalidSsaFunctionCount: 0),
                    null,
                    bundleReference);

            }
        }

        static bool ContainsInteriorBoundary(uint start, uint end, uint[] boundaries)
        {
            var index = Array.BinarySearch(boundaries, start);
            index = index >= 0 ? index + 1 : ~index;
            return index < boundaries.Length && boundaries[index] < end;
        }

        Console.WriteLine($"Recursively translated {count} functions.");
        var unsupportedInstructionCount = totals.Sum(item => item.UnsupportedInstructionCount);
        if (unsupportedInstructionCount > 0)
        {
            Console.WriteLine(
                $"[translator] DEVELOPER OUTPUT: preserved {unsupportedInstructionCount:N0} unsupported instruction(s) " +
                "as runtime traps; release metadata will reject this translation.");
        }
        if (pruneStale && emittedPaths is not null)
        {
            var removed = PruneStaleGeneratedFiles(outDir, emittedPaths, previousOutputMetadata);
            Console.WriteLine($"[translator] Pruned {removed} stale generated file(s).");
        }
        if (completedOutputMetadata is not null)
        {
            var updated = BaseTranslationOutputMetadataFile.WriteIfChangedAtomic(
                outputMetadataPath!,
                completedOutputMetadata);
            Console.WriteLine(updated
                ? $"[translator] Updated base translation metadata: {outputMetadataPath}"
                : $"[translator] Base translation metadata unchanged: {outputMetadataPath}");
        }

        Console.WriteLine($"[translator] File writes: {filesUpdated} updated, {filesUnchanged} unchanged.");
    }
    catch (Exception ex)
    {
        // Log end time and total duration even if there's an exception
        var endTime = DateTime.Now;
        var totalDuration = endTime - startTime;
        Console.WriteLine($"[translator] Translation failed at {endTime:HH:mm:ss}, total time: {totalDuration.TotalSeconds:F2} seconds");
        Console.WriteLine($"[translator] Error: {ex.Message}");
        throw; // Re-throw the exception to maintain original behavior
    }

    // Log end time and total duration
    var endTime2 = DateTime.Now;
    var totalDuration2 = endTime2 - startTime;
    Console.WriteLine($"[translator] Translation completed at {endTime2:HH:mm:ss}, total time: {totalDuration2.TotalSeconds:F2} seconds");

    return 0;
}

int RunGenerateDataInit()
{
    var loadedProject = RequireProject();
    var output = loadedProject.Output.DataInitializer;
    var runtimeConfigOutput = loadedProject.Output.RuntimeConfig;
    var dolPath = loadedProject.Inputs.Dol.Path;
    string? relPath = loadedProject.Inputs.Rel?.Path;

    var dol = DolFile.Load(dolPath);

    // Generate runtime configuration header with the manifest's SDA base pointers
    var (dataInitSda1Base, dataInitSda2Base) = loadedProject.RequireSdaBases();
    RuntimeConfigGenerator.GenerateConfigHeader(
        dataInitSda1Base, dataInitSda2Base, runtimeConfigOutput, loadedProject.Identity.DisplayName);

    // Load the REL file for runtime embedding (apply relocations since runtime does not OSLink)
    RelImage? relImage = null;
    if (!string.IsNullOrWhiteSpace(relPath) && loadedProject.Inputs.Rel is { } configuredRel && File.Exists(relPath))
    {
        relImage = RelFile.Load(relPath).BuildImage(
            configuredRel.LoadAddress,
            applyRelocations: true);
    }

    // Generate the data section initializer
    var outputDir = Path.GetDirectoryName(output);
    if (!string.IsNullOrEmpty(outputDir))
    {
        Directory.CreateDirectory(outputDir);
    }
    DataSectionGenerator.Generate(
        dol,
        relImage,
        output,
        loadedProject.Identity.DisplayName,
        relPath is null ? "rel_module" : Path.GetFileNameWithoutExtension(relPath));

    // Emit the guest symbol table for crash-report symbolization. The function
    // map is already the project's authoritative name source; the runtime only
    // consults this table on the fatal-crash path.
    var guestSymbolOutput = Path.Combine(string.IsNullOrEmpty(outputDir) ? "." : outputDir, "guest_symbol_table.cpp");
    var guestSymbolCount = EmitGuestSymbolTable(functionMap, guestSymbolOutput);

    // Calculate total size
    var dolDataSize = dol.Sections.Where(s => s.HasData).Sum(s => s.Size);
    var relDataSize = relImage?.Data.Length ?? 0;
    var totalSize = dolDataSize + relDataSize;

    Console.WriteLine($"[translator] Generated data section initializer: {output}");
    Console.WriteLine($"[translator] Guest symbol table: {guestSymbolCount:N0} named entries -> {guestSymbolOutput}");
    Console.WriteLine($"[translator] DOL sections: {dolDataSize:N0} bytes");
    Console.WriteLine($"[translator] REL data: {relDataSize:N0} bytes");
    Console.WriteLine($"[translator] Total embedded: {totalSize:N0} bytes ({totalSize / 1024.0 / 1024.0:F2} MB)");

    return 0;
}

// Writes generated/guest_symbol_table.cpp: sorted guest addresses paired with function-map names,
// skipping unnamed rows. Reuses the map already loaded at startup rather than re-parsing the file;
// the hand-rolled loop this replaced split on spaces only and silently lost tab-separated entries.
int EmitGuestSymbolTable(FunctionMap? map, string outputPath)
{
    var entries = new SortedDictionary<uint, string>();
    if (map is not null)
    {
        foreach (var address in map.Addresses)
        {
            if (map.NameOf(address) is { } name)
            {
                entries[address] = name;
            }
        }
    }

    var sb = new StringBuilder(entries.Count * 48 + 1024);
    sb.Append("// Auto-generated by generate-data-init from the project's function map.\n");
    sb.Append("// Guest address -> symbol name table, consumed by the crash reporter.\n");
    sb.Append("#include <cstdint>\n\n");
    sb.Append("extern \"C\" {\n\n");
    // The extern declarations force external linkage: namespace-scope const
    // objects are internal-linkage by default in C++, even inside extern "C".
    sb.Append("extern const uint32_t kGuestMapSymbolCount;\n");
    sb.Append("extern const uint32_t kGuestMapSymbolAddresses[];\n");
    sb.Append("extern const char* const kGuestMapSymbolNames[];\n\n");
    sb.Append("const uint32_t kGuestMapSymbolCount = ")
      .Append(entries.Count.ToString(System.Globalization.CultureInfo.InvariantCulture))
      .Append("u;\n\n");
    sb.Append("const uint32_t kGuestMapSymbolAddresses[] = {\n");
    if (entries.Count == 0)
    {
        sb.Append("    0u,\n");
    }
    foreach (var entry in entries)
    {
        sb.Append("    0x").Append(entry.Key.ToString("X8", System.Globalization.CultureInfo.InvariantCulture)).Append("u,\n");
    }
    sb.Append("};\n\n");
    sb.Append("const char* const kGuestMapSymbolNames[] = {\n");
    if (entries.Count == 0)
    {
        sb.Append("    \"\",\n");
    }
    foreach (var entry in entries)
    {
        sb.Append("    \"");
        foreach (var ch in entry.Value)
        {
            if (ch == '\\' || ch == '"')
            {
                sb.Append('\\').Append(ch);
            }
            else if (ch < 0x20 || ch > 0x7E)
            {
                sb.Append('?');
            }
            else
            {
                sb.Append(ch);
            }
        }
        sb.Append("\",\n");
    }
    sb.Append("};\n\n} // extern \"C\"\n");
    File.WriteAllText(outputPath, sb.ToString());
    return entries.Count;
}

static uint GetFunctionInstructionEnd(FunctionDiscoveryResult result)
{
    return result.Instructions.Count == 0
        ? result.EntryPoint
        : result.Instructions.Max(i => i.EndAddress);
}

// Sorted, coalesced [start, endExclusive) pairs of the instruction addresses the
// function's own control flow executes. Discovery decodes leaf-inlined callees
// separately, so their addresses never appear in result.Instructions.
static uint[] CoalesceInstructionRanges(FunctionDiscoveryResult result)
{
    var addresses = result.Instructions
        .Select(instruction => instruction.Address)
        .Distinct()
        .ToArray();
    Array.Sort(addresses);
    var ranges = new List<uint>();
    foreach (var address in addresses)
    {
        if (ranges.Count != 0 && ranges[^1] == address)
        {
            ranges[^1] = address + 4;
            continue;
        }
        ranges.Add(address);
        ranges.Add(address + 4);
    }
    return ranges.ToArray();
}

// Entry points that lie strictly inside another translation's executed flow: the
// owner decodes and runs the instruction at that address as part of its own body.
static HashSet<uint> ComputeInteriorEntryPoints(IReadOnlyDictionary<uint, uint[]> flowCoverage)
{
    var entries = flowCoverage.Keys.ToArray();
    Array.Sort(entries);
    var interior = new HashSet<uint>();
    foreach (var (owner, ranges) in flowCoverage)
    {
        for (var i = 0; i < ranges.Length; i += 2)
        {
            var index = Array.BinarySearch(entries, ranges[i]);
            if (index < 0) index = ~index;
            for (; index < entries.Length && entries[index] < ranges[i + 1]; index++)
            {
                if (entries[index] != owner)
                {
                    interior.Add(entries[index]);
                }
            }
        }
    }
    return interior;
}

int PruneStaleGeneratedFiles(
    string baseOutDir,
    HashSet<string> emittedPaths,
    BaseTranslationOutputMetadata? previousOutputMetadata = null)
{
    var result = GeneratedOutputPruner.Prune(baseOutDir, emittedPaths, previousOutputMetadata);
    foreach (var warning in result.Warnings)
    {
        Console.Error.WriteLine($"[translator] Warning: {warning}");
    }
    return result.RemovedFiles;
}

IEnumerable<uint> DiscoverDiscoveryTranslationTargets(FunctionDiscoveryResult result)
{
    foreach (var target in DiscoverTranslationTargetsCore(result.Instructions, result.LinearIr))
    {
        // Debug-stub code (MetroTRK) branches into the low-memory exception vectors, which are not
        // part of the loaded image. A function map normally keeps discovery away from them; without
        // one, skip the target instead of aborting the whole run.
        if (!image.Value.Contains(target, 4))
        {
            if (outOfImageTargets.TryAdd(target, result.EntryPoint))
            {
                Console.Error.WriteLine(
                    $"[translator] warning: 0x{result.EntryPoint:X8} branches to 0x{target:X8}, outside the loaded image; not translated.");
            }

            continue;
        }

        yield return target;
    }
}

IEnumerable<uint> DiscoverTranslationTargetsCore(
    IReadOnlyList<PpcInstruction> instructions,
    IrFunction linearIr)
{
    var localInstructions = new HashSet<uint>(instructions.Select(i => i.Address));
    var emitted = new HashSet<uint>();

    foreach (var ins in instructions)
    {
        if (ins.BranchTargets.Count == 0)
        {
            continue;
        }

        var shouldFollow = ins.IsCall;
        if (!shouldFollow && ins.IsUnconditionalBranch)
        {
            // If the branch jumps outside this function, treat it as a tail call.
            shouldFollow = ins.BranchTargets.Any(target => !localInstructions.Contains(target));
        }

        if (!shouldFollow)
        {
            continue;
        }

        foreach (var target in ins.BranchTargets)
        {
            if (emitted.Add(target))
            {
                yield return target;
            }
        }
    }

    foreach (var target in DiscoverCallbackTargetsCore(instructions, argumentRegisters, requireFunctionStart: true))
    {
        if (emitted.Add(target))
        {
            yield return target;
        }
    }

    foreach (var call in linearIr.Blocks.SelectMany(static block => block.Instructions.OfType<IrCall>()))
    {
        if (GuestTargetParser.TryParseAddress(call.Target, out var target) && emitted.Add(target))
        {
            yield return target;
        }
    }
}

// Discovers callback function pointers by checking all argument registers at every call site
IEnumerable<uint> DiscoverCallbackTargetsCore(
    IReadOnlyList<PpcInstruction> instructions,
    string[] argRegs,
    bool requireFunctionStart = true)
{
    var constants = new Dictionary<string, uint>(StringComparer.OrdinalIgnoreCase);

    foreach (var ins in instructions)
    {
        TrackConstants(ins, constants);

        // Discover function pointers stored to memory (vtable/dispatcher init).
        if (IsStoreMnemonic(ins.Mnemonic) && TryGetReg(ins, 0, out var rs))
        {
            if (constants.TryGetValue(rs, out var stored) &&
                LooksExecutable(stored) &&
                (!requireFunctionStart || LooksLikeFunctionStart(stored)))
            {
                yield return stored;
            }
        }

        // Discover function-pointer getters that return an executable callback in r3.
        if (ins.IsReturn &&
            constants.TryGetValue("r3", out var returned) &&
            LooksExecutable(returned) &&
            (!requireFunctionStart || LooksLikeFunctionStart(returned)))
        {
            yield return returned;
        }

        // Check both regular calls and tail calls (unconditional branches to external functions)
        var isCallOrTailCall = ins.IsCall || ins.IsUnconditionalBranch;
        if (isCallOrTailCall && ins.BranchTargets.Count == 1)
        {
            // Check all argument registers for values that look like function pointers
            foreach (var reg in argRegs)
            {
                if (constants.TryGetValue(reg, out var candidate) &&
                    LooksExecutable(candidate) &&
                    (!requireFunctionStart || LooksLikeFunctionStart(candidate)))
                {
                    yield return candidate;
                }
            }

            ClearVolatileAfterCall(constants);
        }
    }

    // PpcDecoder only ever produces lowercase mnemonics (every literal and the
    // `opc_`/`xo_`/`fp_` fallbacks), so the per-instruction ToLowerInvariant
    // allocation this used to make was pure overhead.
    static bool IsStoreMnemonic(string mnemonic) =>
        mnemonic.StartsWith("stw", StringComparison.Ordinal) ||
        mnemonic.StartsWith("std", StringComparison.Ordinal);

    void ClearVolatileAfterCall(IDictionary<string, uint> constants)
    {
        constants.Remove("r3");
        constants.Remove("r4");
        constants.Remove("r5");
        constants.Remove("r6");
        constants.Remove("r7");
        constants.Remove("r8");
        constants.Remove("r9");
        constants.Remove("r10");
        constants.Remove("r11");
        constants.Remove("r12");
    }

    void TrackConstants(PpcInstruction ins, IDictionary<string, uint> constants)
    {
        // Decoder mnemonics are lowercase by construction; comparing the string
        // directly avoids one allocation per decoded instruction.
        var mnem = ins.Mnemonic;

        if (TryHandleMove(ins, mnem, constants))
        {
            return;
        }

        if (mnem == "li")
        {
            if (TryGetReg(ins, 0, out var rd) && TryGetImm(ins, 1, out var imm))
            {
                constants[rd] = unchecked((uint)imm);
                return;
            }
        }
        else if (mnem == "lis")
        {
            if (TryGetReg(ins, 0, out var rd) && TryGetImm(ins, 1, out var imm))
            {
                constants[rd] = unchecked((uint)(imm << 16));
                return;
            }
        }
        else if (mnem == "addi")
        {
            if (TryGetReg(ins, 0, out var rd) && TryGetReg(ins, 1, out var ra) && TryGetImm(ins, 2, out var imm))
            {
                if (TryGetBaseValue(ra, constants, out var baseVal))
                {
                    constants[rd] = AddImm(baseVal, imm);
                }
                else
                {
                    constants.Remove(rd);
                }
                return;
            }
        }
        else if (mnem == "addis")
        {
            if (TryGetReg(ins, 0, out var rd) && TryGetReg(ins, 1, out var ra) && TryGetImm(ins, 2, out var imm))
            {
                if (TryGetBaseValue(ra, constants, out var baseValHi))
                {
                    constants[rd] = AddImm(baseValHi, imm << 16);
                }
                else
                {
                    constants.Remove(rd);
                }
                return;
            }
        }
        else if (mnem == "ori")
        {
            if (TryGetReg(ins, 0, out var rd) && TryGetReg(ins, 1, out var ra) && TryGetImm(ins, 2, out var imm))
            {
                if (TryGetBaseValue(ra, constants, out var orBase))
                {
                    constants[rd] = unchecked(orBase | (uint)(imm & 0xFFFF));
                }
                else
                {
                    constants.Remove(rd);
                }
                return;
            }
        }
        else if (mnem == "oris")
        {
            if (TryGetReg(ins, 0, out var rd) && TryGetReg(ins, 1, out var ra) && TryGetImm(ins, 2, out var imm))
            {
                if (TryGetBaseValue(ra, constants, out var orisBase))
                {
                    constants[rd] = unchecked(orisBase | ((uint)imm << 16));
                }
                else
                {
                    constants.Remove(rd);
                }
                return;
            }
        }

        if (TryWritesDest(ins, out var dest))
        {
            constants.Remove(dest);
        }
    }

    bool TryHandleMove(PpcInstruction ins, string mnemonic, IDictionary<string, uint> constants)
    {
        if (string.Equals(mnemonic, "mr", StringComparison.OrdinalIgnoreCase) && TryGetReg(ins, 0, out var rd) && TryGetReg(ins, 1, out var rs))
        {
            if (constants.TryGetValue(rs, out var value))
            {
                constants[rd] = value;
            }
            else
            {
                constants.Remove(rd);
            }
            return true;
        }

        if (string.Equals(mnemonic, "or", StringComparison.OrdinalIgnoreCase) &&
            ins.Operands.Count == 3 &&
            ins.Operands[0] is PpcRegisterOperand dest &&
            ins.Operands[1] is PpcRegisterOperand src1 &&
            ins.Operands[2] is PpcRegisterOperand src2 &&
            string.Equals(src1.Name, src2.Name, StringComparison.OrdinalIgnoreCase))
        {
            var srcName = NormalizeReg(src1.Name);
            var destName = NormalizeReg(dest.Name);
            if (constants.TryGetValue(srcName, out var value))
            {
                constants[destName] = value;
            }
            else
            {
                constants.Remove(destName);
            }
            return true;
        }

        return false;
    }

    bool TryGetBaseValue(string reg, IDictionary<string, uint> constants, out uint value)
    {
        if (string.Equals(reg, "r0", StringComparison.OrdinalIgnoreCase))
        {
            value = 0;
            return true;
        }

        return constants.TryGetValue(reg, out value);
    }

    bool TryWritesDest(PpcInstruction ins, out string dest)
    {
        dest = string.Empty;
        if (ins.Operands.Count == 0 || ins.Operands[0] is not PpcRegisterOperand reg)
        {
            return false;
        }

        var m = ins.Mnemonic.ToLowerInvariant();
        if (m.StartsWith("st", StringComparison.Ordinal) || m.StartsWith("b", StringComparison.Ordinal) || m.StartsWith("cmp", StringComparison.Ordinal))
        {
            return false;
        }

        dest = NormalizeReg(reg.Name);
        return true;
    }

    bool TryGetReg(PpcInstruction ins, int index, out string reg)
    {
        if (ins.Operands.Count > index && ins.Operands[index] is PpcRegisterOperand r)
        {
            reg = NormalizeReg(r.Name);
            return true;
        }

        reg = string.Empty;
        return false;
    }

    bool TryGetImm(PpcInstruction ins, int index, out int imm)
    {
        if (ins.Operands.Count > index && ins.Operands[index] is PpcImmediateOperand i)
        {
            imm = i.Value;
            return true;
        }

        imm = 0;
        return false;
    }

    uint AddImm(uint baseVal, int imm) => unchecked(baseVal + (uint)imm);

    string NormalizeReg(string reg) => reg.ToLowerInvariant();
}

/// <summary>
/// Function entry points stored as a data-section word (vtables, dispatch tables, callbacks). Not a
/// discovery seeder; answers which mod-build winners must stay reachable via dispatch table, not a static call.
/// </summary>
    bool LooksExecutable(uint address)
    {
        // Build into a local and publish the finished list under a gate. The
        // previous shape assigned an empty list to the field and filled it in
        // afterwards, which a concurrent reader could observe half-populated.
        var ranges = Volatile.Read(ref _executableRanges);
        if (ranges == null)
        {
            lock (_executableRangesGate)
            {
                ranges = _executableRanges;
                if (ranges == null)
                {
                    ranges = new List<AddressRange>();
                    if (dolFile.IsValueCreated)
                    {
                        foreach (var s in dolFile.Value.Sections)
                        {
                            if (s.IsExecutable && s.Size > 0)
                            {
                                ranges.Add(AddressRange.FromStartAndSize(s.VirtualAddress, s.Size));
                            }
                        }
                    }

                    if (relFile.IsValueCreated && relFile.Value is not null && image.IsValueCreated)
                    {
                        var relBase = image.Value.RelRange.Start;
                        foreach (var s in relFile.Value.Sections)
                        {
                            if (s.Executable && s.Size > 0 && s.FileOffset != 0)
                            {
                                ranges.Add(AddressRange.FromStartAndSize(relBase + s.FileOffset, s.Size));
                            }
                        }
                    }

                    Volatile.Write(ref _executableRanges, ranges);
                }
            }
        }

        return ranges.Any(r => r.Contains(address)) && (address & 3) == 0 &&
               !dataInTextRanges.Any(r => r.Contains(address));
    }

bool LooksLikeFunctionStart(uint address)
{
    var mem = image.Value.Memory;
    if (!image.Value.Contains(address, sizeof(uint)))
    {
        return false;
    }

    var offset = image.Value.GetOffset(address, sizeof(uint));
    var firstWord = BinaryPrimitives.ReadUInt32BigEndian(mem.AsSpan(offset, 4));
    if (firstWord == 0 || firstWord == 0xFFFFFFFF)
    {
        return false;
    }

    // Decode the first instruction and reject if it's an unknown opcode.
    // This prevents treating string data (e.g., "Metrowerks...") as code.
    var firstIns = PpcDecoder.Decode(address, firstWord);
    if (firstIns.Mnemonic.StartsWith("unk", StringComparison.OrdinalIgnoreCase))
    {
        return false;
    }

    return true;
}

TranslationProjectConfig RequireProject() => project ?? throw new InvalidOperationException(
    "This command requires --project <path>. See projects/brawl/recomp.yml for an example.");

int ShowHelp(string invalidCommand)
{
    Console.Error.WriteLine($"Unrecognized command '{invalidCommand}'.");
    WriteUsage(Console.Error, KnownCommands());
    return 1;
}

static int ShowGlobalHelp()
{
    WriteUsage(Console.Out, KnownCommands());
    return 0;
}

static int ShowCommandHelp(string command)
{
    WriteUsage(Console.Out, new[] { command });
    return 0;
}

static void WriteUsage(TextWriter writer, IReadOnlyList<string> commands)
{
    writer.WriteLine("Usage:");
    foreach (var name in commands)
    {
        writer.WriteLine(CommandUsage(name));
    }
    writer.WriteLine("Every command also accepts [--project path] [--help].");
}

static string[] KnownCommands() => new[]
{
    "info",
    "generate-data-init",
    "translate-recursive",
    "emit-build-shards",
};

/// <summary>
/// Single source of truth for what each command accepts; usage text and the argument validator
/// both derive from this so a flag can't exist in one but not the other, as happened with the
/// hand-written usage strings this replaced.
/// </summary>
static (string? Positional, CommandOption[] Options)? CommandSpec(string command) => command switch
{
    "info" or "--info" or "--version" =>
        (null, Array.Empty<CommandOption>()),
    "generate-data-init" => (null, Array.Empty<CommandOption>()),
    "translate-recursive" => ("<start_addr>", new CommandOption[]
    {
        new("--outdir", "path"),
        new("--output-metadata", "path"),
        new("--production-source-bundle", "path"),
        new("--threads", "N"),
        new("--no-function-files"),
        new("--no-state-free-abi"),
        new("--data-in-text", "path"),
        new("--prune-stale")
    }),
    "emit-build-shards" => (null, new CommandOption[]
    {
        new("--base-metadata", "path"),
        new("--base-functions-dir", "path"),
        new("--native-source-dir", "path"),
        new("--out", "generated/build_shards"),
        new("--base-shards", "72")
    }),
    _ => null
};

static string CommandUsage(string command)
{
    if (CommandSpec(command) is not { } spec)
    {
        return $"  translator {command}";
    }

    var text = new StringBuilder("  translator ").Append(command);
    if (spec.Positional is { } positional)
    {
        text.Append(' ').Append(positional);
    }
    foreach (var option in spec.Options)
    {
        var body = option.Value is null ? option.Name : $"{option.Name} {option.Value}";
        text.Append(' ').Append(option.Required ? body : $"[{body}]");
    }
    return text.ToString();
}

// Options every command tolerates. --project and --profile are consumed before
// dispatch, so they only reach a command's tail when they were repeated.
static string[] GlobalValueOptions() => new[] { "--project" };

static string[] GlobalFlagOptions() => Array.Empty<string>();

/// <summary>
/// Returns an exit code when the command line was fully handled here (help, or an option the
/// command's parser would discard), null otherwise. Only inspects option-like tokens; positional
/// addresses, paths and passthrough values are never rejected.
/// </summary>
static int? HandleCommandLineOptions(string command, string[] argsTail)
{
    if (CommandSpec(command) is not { } spec)
    {
        return null;
    }

    if (argsTail.Any(static token =>
            string.Equals(token, "--help", StringComparison.Ordinal) ||
            string.Equals(token, "-h", StringComparison.Ordinal) ||
            string.Equals(token, "-?", StringComparison.Ordinal)))
    {
        return ShowCommandHelp(command);
    }

    var values = new HashSet<string>(StringComparer.Ordinal);
    values.UnionWith(spec.Options.Where(static option => option.Value is not null).Select(static option => option.Name));
    values.UnionWith(GlobalValueOptions());
    var flags = new HashSet<string>(StringComparer.Ordinal);
    flags.UnionWith(spec.Options.Where(static option => option.Value is null).Select(static option => option.Name));
    flags.UnionWith(GlobalFlagOptions());

    for (var index = 0; index < argsTail.Length; index++)
    {
        var token = argsTail[index];
        if (values.Contains(token))
        {
            // Step over the value so an option argument that happens to look
            // like an option of its own is never rejected.
            index++;
            continue;
        }
        if (flags.Contains(token) || !token.StartsWith('-'))
        {
            continue;
        }

        Console.Error.WriteLine($"Unrecognized option '{token}' for command '{command}'.");
        WriteUsage(Console.Error, new[] { command });
        return 2;
    }

    return null;
}

ProgramImage LoadImage()
{
    var loadedProject = RequireProject();
    var dol = dolFile.Value;

    // Generate runtime configuration header with the manifest's SDA base pointers
    var (sda1Base, sda2Base) = loadedProject.RequireSdaBases();
    RuntimeConfigGenerator.GenerateConfigHeader(
        sda1Base,
        sda2Base,
        loadedProject.Output.RuntimeConfig,
        loadedProject.Identity.DisplayName);
    Console.WriteLine(
        $"[translator] SDA bases: r13 (_SDA_BASE_) 0x{sda1Base:X8}, r2 (_SDA2_BASE_) 0x{sda2Base:X8} " +
        $"(entry 0x{dol.EntryPoint:X8}).");

    var relImage = relFile.Value?.BuildImage(loadedProject.Inputs.Rel!.LoadAddress);
    return new ProgramImageBuilder().Build(dol, relImage, loadedProject.Memory.Base, loadedProject.Memory.Size);
}

DolFile LoadDol()
{
    return DolFile.Load(RequireProject().Inputs.Dol.Path);
}

RelFile? LoadRel()
{
    return RequireProject().Inputs.Rel is { } rel ? RelFile.Load(rel.Path) : null;
}

static uint ParseAddress(string text) => GuestTargetParser.ParseHexAddress(text);

// Small prewarm passes happen constantly (every discovered continuation wave);
// only a whole-image-sized one is worth a line of output.
static void ReportAbiPrewarm(GuestAbiPrewarmReport report)
{
    if (report.AddressCount <= 1024) return;
    Console.WriteLine(
        $"[translator] ABI prewarm: {report.AddressCount:N0} address(es); " +
        $"phase A {report.PhaseASeconds:F1}s, phase B {report.PhaseBSeconds:F1}s " +
        $"over {report.PhaseBComputedCount:N0} deferred address(es).");
}

string ResolveFunctionName(uint entry, string? preferred = null, string? fallback = null)
{
    var candidate = !string.IsNullOrWhiteSpace(preferred)
        ? preferred!
        : !string.IsNullOrWhiteSpace(fallback)
            ? fallback!
            : $"func_{entry:X8}";

    var entryToken = entry.ToString("X8");
    return NameContainsAddressToken(candidate, entryToken)
        ? candidate
        : $"{candidate}_{entryToken}";
}

static bool NameContainsAddressToken(string text, string addressToken)
{
    if (string.IsNullOrEmpty(text) || string.IsNullOrEmpty(addressToken))
    {
        return false;
    }

    return text.IndexOf(addressToken, StringComparison.OrdinalIgnoreCase) >= 0;
}

HashSet<uint> BuildKnownBaseFunctionEntryPoints(
    string generatedFunctionsDir,
    IEnumerable<uint> seeds,
    bool includeGeneratedHistory = true)
{
    var starts = new HashSet<uint>(seeds);
    if (includeGeneratedHistory)
    {
        foreach (var start in DiscoverFunctionStartAddressesFromFileNames(generatedFunctionsDir))
        {
            starts.Add(start);
        }
    }

    return starts;
}

static IEnumerable<uint> DiscoverFunctionStartAddressesFromFileNames(string generatedFunctionsDir)
{
    if (!Directory.Exists(generatedFunctionsDir))
    {
        yield break;
    }

    var addressRegex = new Regex("([0-9A-Fa-f]{8})", RegexOptions.CultureInvariant);
    foreach (var path in Directory.EnumerateFiles(generatedFunctionsDir, "*.cpp", SearchOption.AllDirectories))
    {
        var match = addressRegex.Match(Path.GetFileNameWithoutExtension(path));
        if (match.Success &&
            GuestTargetParser.TryParseHexAddress(match.Groups[1].Value, out var value))
        {
            yield return value;
        }
    }
}

static int ParseInt(string text) =>
    text.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
        ? int.Parse(text[2..], NumberStyles.HexNumber, CultureInfo.InvariantCulture)
        : int.Parse(text, CultureInfo.InvariantCulture);

static string? OptionValue(string[] argsTail, string name)
{
    var index = Array.IndexOf(argsTail, name);
    if (index >= 0 && index + 1 < argsTail.Length)
    {
        return argsTail[index + 1];
    }
    return null;
}

static (string? Value, string[] Remaining) ExtractOption(string[] values, string name)
{
    for (var index = 0; index < values.Length; index++)
    {
        if (!string.Equals(values[index], name, StringComparison.Ordinal))
        {
            continue;
        }
        if (index + 1 >= values.Length)
        {
            throw new ArgumentException($"{name} requires a value.");
        }

        var remaining = values.Where((_, itemIndex) => itemIndex != index && itemIndex != index + 1).ToArray();
        return (values[index + 1], remaining);
    }

    return (null, values);
}

static bool HasFlag(string[] argsTail, string name) => argsTail.Any(a => string.Equals(a, name, StringComparison.Ordinal));

/// <summary>
/// Addresses a leaf splice must never cover: the decoded bytes aren't what the runtime
/// executes there (native registration/fatal stub, exclusion, or a mod overlay rewrite).
/// </summary>
static IReadOnlySet<uint> BuildLeafInliningBlockedTargets(
    RuntimeNativeGuestEffectSet nativeGuestEffects,
    IReadOnlySet<uint> translationExclusions)
{
    var blocked = new HashSet<uint>(nativeGuestEffects.Contracts.Keys);
    blocked.UnionWith(translationExclusions);
    return blocked;
}

// Apply the project policy to each translated function.
static TranslationOptions WithProjectCodegenPolicy(
    TranslationOptions options,
    ProjectTranslation translation,
    uint entryPoint) =>
    options with
    {
        AllowUnsupportedInstructions = translation.AllowUnsupportedInstructions,
        // Leaf ABI spill elision is structurally self-guarding: it requires a
        // matched save/restore pair inside the emitted body of a single-return
        // leaf, so a partial mod chunk that only carries one half of an ABI
        // prologue is rejected rather than mis-transformed.
        EnableLeafAbiSpillElision = true,
    };

/// <summary>
/// One option carried by one command. <paramref name="Value"/> is the metavariable
/// printed after the option name, or null for a flag; <paramref name="Required"/>
/// only controls whether usage brackets it.
/// </summary>
readonly record struct CommandOption(string Name, string? Value = null, bool Required = false);
