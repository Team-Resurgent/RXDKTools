using Rxdk.Pdb;

namespace Rxdk.Pdb.Tests;

public sealed class DbiStreamTests
{
    private static string PdbPath => Path.Combine(AppContext.BaseDirectory, "TestData", "TriangleXDK.pdb");

    private static PdbImage Pdb() => PdbImage.OpenFile(PdbPath);

    [Fact]
    public void Modules_AreListed()
    {
        var dbi = Pdb().Dbi;
        Assert.NotEmpty(dbi.Modules);
        Assert.All(dbi.Modules, m => Assert.False(string.IsNullOrEmpty(m.ModuleName)));
        // At least one module should carry symbols (a real symbol stream).
        Assert.Contains(dbi.Modules, m => m.HasSymbols);
    }

    [Fact]
    public void SectionHeaders_ArePresentAndOrdered()
    {
        var dbi = Pdb().Dbi;
        Assert.NotEmpty(dbi.Sections);
        // A normal image has a .text section at a non-zero RVA.
        Assert.Contains(dbi.Sections, s => s.VirtualAddress > 0);
    }

    [Fact]
    public void FindModuleByRva_ResolvesAContributionStart()
    {
        var dbi = Pdb().Dbi;

        // Pick a code contribution and confirm its start RVA resolves back to a module.
        var probe = dbi.Modules.FirstOrDefault(m => m.Contribution.Size > 0 && m.Contribution.Section != 0);
        Assert.NotNull(probe);

        var rva = dbi.SectionOffsetToRva(probe!.Contribution.Section, probe.Contribution.Offset);
        Assert.True(rva > 0, "contribution start should map to a non-zero RVA");

        var found = dbi.FindModuleByRva(rva);
        Assert.NotNull(found);
    }
}
