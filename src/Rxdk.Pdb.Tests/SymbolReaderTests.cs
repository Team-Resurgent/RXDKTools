using Rxdk.Pdb;
using Rxdk.Pdb.Symbols;
using Rxdk.Pdb.Tpi;

namespace Rxdk.Pdb.Tests;

/// <summary>
/// Exercises the modern S_LOCAL / S_DEFRANGE_FRAMEPOINTER_REL path against MiniLocals.pdb, whose
/// main() has: hr (long, off -8), arr (int[4], off -24), p (Point struct, off -32); and add()
/// has params a/b and local sum. These are exactly the records dbghelp mis-reports.
/// </summary>
public sealed class SymbolReaderTests
{
    private static string PdbPath => Path.Combine(AppContext.BaseDirectory, "TestData", "MiniLocals.pdb");

    private static PdbImage Pdb() => PdbImage.OpenFile(PdbPath);

    private static FrameInfo FindFunction(PdbImage pdb, string name) =>
        pdb.Symbols.EnumerateFunctions().Single(f => f.FunctionName == name);

    [Fact]
    public void Main_HasFrameRelativeLocals_WithCorrectOffsetsAndTypes()
    {
        var pdb = Pdb();
        var main = FindFunction(pdb, "main");

        var hr = main.Locals.Single(l => l.Name == "hr");
        Assert.Equal(-8, hr.FrameOffset);
        var hrType = pdb.Types.Resolve(hr.TypeIndex);
        Assert.Equal(PdbTypeKind.Primitive, hrType.Kind);
        Assert.Equal(4u, hrType.ByteSize);
        Assert.Equal("long", hrType.Name);

        var arr = main.Locals.Single(l => l.Name == "arr");
        Assert.Equal(-24, arr.FrameOffset);
        var arrType = pdb.Types.Resolve(arr.TypeIndex);
        Assert.Equal(PdbTypeKind.Array, arrType.Kind);
        Assert.Equal(16u, arrType.ByteSize);
        Assert.Equal(4u, arrType.ElementCount);

        var p = main.Locals.Single(l => l.Name == "p");
        Assert.Equal(-32, p.FrameOffset);
        var pType = pdb.Types.Resolve(p.TypeIndex);
        Assert.True(pType.IsAggregate);
        Assert.Equal(8u, pType.ByteSize);
        Assert.Equal("Point", pType.Name);
        Assert.Equal(2, pType.Members.Count);
        Assert.Equal(0, pType.Members[0].Offset); // x
        Assert.Equal(4, pType.Members[1].Offset); // y
    }

    [Fact]
    public void Add_ParametersAndLocalAreClassifiedAndLocated()
    {
        var pdb = Pdb();
        var add = FindFunction(pdb, "add");

        var a = add.Locals.Single(l => l.Name == "a");
        var b = add.Locals.Single(l => l.Name == "b");
        var sum = add.Locals.Single(l => l.Name == "sum");

        Assert.True(a.IsParameter);
        Assert.True(b.IsParameter);
        Assert.False(sum.IsParameter);

        Assert.Equal(8, a.FrameOffset);
        Assert.Equal(12, b.FrameOffset);
        Assert.Equal(-4, sum.FrameOffset);
    }

    [Fact]
    public void FindFrame_OutsideAnyFunction_ReturnsNull()
    {
        Assert.Null(Pdb().FindFrame(0));
    }
}
