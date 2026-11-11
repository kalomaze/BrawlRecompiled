namespace Translator.Tests;

public sealed class ProductionTranslationPolicyTests
{
    [Fact]
    public void ProductionProjectIsStrict()
    {
        var repositoryRoot = FindWorkspaceRoot();
        var productionProject = File.ReadAllText(
            Path.Combine(repositoryRoot, "projects", "brawl", "recomp.yml"));
        Assert.Contains("allow_unsupported_instructions: false", productionProject, StringComparison.Ordinal);
        Assert.DoesNotContain("allow_unsupported_instructions: true", productionProject, StringComparison.Ordinal);
    }
    private static string FindWorkspaceRoot()
    {
        for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory is not null; directory = directory.Parent)
        {
            var workspace = Path.Combine(directory.FullName, "wiicompiled");
            if (File.Exists(Path.Combine(workspace, "projects", "brawl", "recomp.yml")))
                return workspace;
        }
        throw new DirectoryNotFoundException("Could not find Brawl workspace.");
    }
}
