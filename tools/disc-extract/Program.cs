using BrawlRecompiled;

try
{
    if (args.Length != 2) throw new ArgumentException("Usage: DiscExtract IMAGE OUTPUT_DIRECTORY");
    using var disc = WiiDisc.Open(args[0]);
    if (disc.GameId != WiiDisc.Game)
        throw new InvalidDataException(disc.Problem());
    // Build compatibility is checked against the prelinked DOL hash before translation;
    // the retail revision byte alone does not identify the executable's contents.
    Console.WriteLine($"Extracting {disc.GameId}, revision {disc.Revision}: {disc.Title}");
    disc.Extract(args[1], (done, total, path) =>
    {
        if (done % 100 == 0 || done == total) Console.WriteLine($"{done}/{total}: {path}");
    }, include: path => path.StartsWith(Path.Combine("files", "module") + Path.DirectorySeparatorChar, StringComparison.Ordinal));
    return 0;
}
catch (Exception error)
{
    Console.Error.WriteLine(error.Message);
    return 1;
}
