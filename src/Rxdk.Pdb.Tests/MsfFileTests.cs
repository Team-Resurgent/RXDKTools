using Rxdk.Pdb;
using Rxdk.Pdb.Msf;

namespace Rxdk.Pdb.Tests;

public sealed class MsfFileTests
{
    private static string PdbPath => Path.Combine(AppContext.BaseDirectory, "TestData", "TriangleXDK.pdb");

    [Fact]
    public void Open_ValidPdb_ParsesSuperblock()
    {
        var msf = MsfFile.Open(File.ReadAllBytes(PdbPath));

        // MSF block size is a power of two, one of 512/1024/2048/4096 in practice.
        Assert.Contains(msf.BlockSize, new[] { 512, 1024, 2048, 4096 });
        Assert.True(msf.StreamCount > 4, $"expected the fixed streams plus modules, got {msf.StreamCount}");
    }

    [Fact]
    public void Open_NonMsf_Throws()
    {
        var garbage = new byte[64];
        Assert.Throws<PdbFormatException>(() => MsfFile.Open(garbage));
    }

    [Fact]
    public void ReadStream_PdbInfo_HasExpectedSize()
    {
        var msf = MsfFile.Open(File.ReadAllBytes(PdbPath));

        var info = msf.ReadStream(1);
        Assert.Equal((int)msf.StreamSize(1), info.Length);
        Assert.True(info.Length >= 28, "PDB info stream must hold at least version/sig/age/guid");
    }
}
