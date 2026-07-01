using Rxdk.Pdb;
using Rxdk.Pdb.Tpi;

namespace Rxdk.Pdb.Tests;

public sealed class TypeSystemTests
{
    private static string PdbPath => Path.Combine(AppContext.BaseDirectory, "TestData", "TriangleXDK.pdb");

    private static PdbImage Pdb() => PdbImage.OpenFile(PdbPath);

    [Fact]
    public void Tpi_HeaderLooksSane()
    {
        var tpi = Pdb().Tpi;
        Assert.Equal(0x1000u, tpi.TypeIndexBegin);
        Assert.True(tpi.TypeIndexEnd > tpi.TypeIndexBegin);
        Assert.True(tpi.RecordCount > 0);
    }

    [Theory]
    [InlineData(0x0012u, "long", 4u, false)]   // T_LONG
    [InlineData(0x0074u, "int", 4u, false)]    // T_INT4
    [InlineData(0x0022u, "unsigned long", 4u, false)]
    [InlineData(0x0040u, "float", 4u, true)]   // T_REAL32
    [InlineData(0x0041u, "double", 8u, true)]  // T_REAL64
    [InlineData(0x0010u, "signed char", 1u, false)]
    public void Resolve_Primitive_HasExpectedSize(uint index, string name, uint size, bool isFloat)
    {
        var t = Pdb().Types.Resolve(index);
        Assert.Equal(PdbTypeKind.Primitive, t.Kind);
        Assert.Equal(size, t.ByteSize);
        Assert.Equal(name, t.Name);
        Assert.Equal(isFloat, t.IsFloatingPoint);
    }

    [Theory]
    [InlineData(0x0403u)] // void*
    [InlineData(0x0470u)] // char* (near32)
    public void Resolve_PrimitivePointer_IsFourBytes(uint index)
    {
        var t = Pdb().Types.Resolve(index);
        Assert.Equal(PdbTypeKind.Pointer, t.Kind);
        Assert.Equal(4u, t.ByteSize);
    }

    [Fact]
    public void EveryRecordType_ResolvesWithoutThrowing()
    {
        var pdb = Pdb();
        var types = pdb.Types;
        var resolved = 0;
        foreach (var index in pdb.Tpi.TypeIndices())
        {
            // Must not throw. Forward-ref types intentionally redirect to their real definition,
            // so the resolved TypeIndex may differ from the requested index.
            _ = types.Resolve(index);
            resolved++;
        }

        Assert.True(resolved > 0);
    }

    [Fact]
    public void AtLeastOneStruct_HasSizeAndResolvableMembers()
    {
        var pdb = Pdb();
        var types = pdb.Types;

        PdbType? found = null;
        foreach (var index in pdb.Tpi.TypeIndices())
        {
            var t = types.Resolve(index);
            if (t.IsAggregate && t.ByteSize > 0 && t.Members.Count > 0)
            {
                found = t;
                break;
            }
        }

        Assert.NotNull(found);
        // Members must sit within the aggregate and reference resolvable types.
        foreach (var m in found!.Members)
        {
            Assert.True(m.Offset >= 0 && m.Offset < found.ByteSize, $"member {m.Name} offset {m.Offset} out of range");
            Assert.NotNull(m.Name);
        }
    }

    [Fact]
    public void TryFindByName_RoundTripsANamedAggregate()
    {
        var pdb = Pdb();
        var types = pdb.Types;

        PdbType? named = null;
        foreach (var index in pdb.Tpi.TypeIndices())
        {
            var t = types.Resolve(index);
            if (t.IsAggregate && !string.IsNullOrEmpty(t.Name) && t.ByteSize > 0)
            {
                named = t;
                break;
            }
        }

        Assert.NotNull(named);
        Assert.True(types.TryFindByName(named!.Name!, out var byName));
        Assert.Equal(named.ByteSize, byName.ByteSize);
    }
}
