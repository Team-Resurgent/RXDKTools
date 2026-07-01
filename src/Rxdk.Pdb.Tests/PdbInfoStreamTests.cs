using Rxdk.Pdb;

namespace Rxdk.Pdb.Tests;

public sealed class PdbInfoStreamTests
{
    private static string PdbPath => Path.Combine(AppContext.BaseDirectory, "TestData", "TriangleXDK.pdb");

    [Fact]
    public void Info_ParsesHeaderAndNamedStreams()
    {
        var pdb = PdbImage.OpenFile(PdbPath);
        var info = pdb.Info;

        Assert.NotEqual(Guid.Empty, info.Guid);
        Assert.True(info.Age >= 1, $"age should be >= 1, was {info.Age}");
        // Every real PDB carries the "/names" string table in its named-stream map.
        Assert.True(info.NamedStreams.ContainsKey("/names"), "expected a /names named stream");
        Assert.True(info.NamedStreams["/names"] > 0);
    }
}
