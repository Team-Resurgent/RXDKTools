namespace Rxdk.Pdb.Tpi;

public enum PdbTypeKind
{
    Unknown,
    Primitive,
    Pointer,
    Array,
    Struct,
    Class,
    Union,
    Enum,
    Modifier,
    Procedure,
}

/// <summary>A single field of a struct/class/union.</summary>
public sealed record PdbMember(string Name, long Offset, uint TypeIndex);

/// <summary>
/// A resolved CodeView type: enough to size a local, format a scalar, and expand an aggregate.
/// <see cref="ReferentType"/> holds the pointed-at / element / modified / underlying type index
/// depending on <see cref="Kind"/>.
/// </summary>
public sealed class PdbType
{
    public required uint TypeIndex { get; init; }
    public required PdbTypeKind Kind { get; init; }
    public required uint ByteSize { get; init; }
    public string? Name { get; init; }
    public bool IsFloatingPoint { get; init; }
    public uint ReferentType { get; init; }
    public uint ElementCount { get; init; }
    public IReadOnlyList<PdbMember> Members { get; init; } = Array.Empty<PdbMember>();

    public bool IsAggregate => Kind is PdbTypeKind.Struct or PdbTypeKind.Class or PdbTypeKind.Union;
}
