namespace Translator.Tests;

public sealed class ProductionTranslationPolicyTests
{
    [Fact]
    public void ProductionProjectIsStrict()
    {
        var repositoryRoot = TranslatorCppTestHarness.FindWorkspaceRoot();
        var productionProject = File.ReadAllText(
            Path.Combine(repositoryRoot, "projects", "brawl", "recomp.yml"));
        Assert.Contains("allow_unsupported_instructions: false", productionProject, StringComparison.Ordinal);
        Assert.DoesNotContain("allow_unsupported_instructions: true", productionProject, StringComparison.Ordinal);
    }
}
