namespace Rxdk.Pdb.Dbi;

/// <summary>
/// One module (compiland / .obj) described by the DBI module-info substream. The
/// <see cref="SymbolStreamIndex"/> points at the MSF stream that holds this module's CodeView
/// symbols (procedures, locals, def-ranges); it is -1 when the module has no symbols.
/// </summary>
public sealed class DbiModule
{
    public required int Index { get; init; }
    public required string ModuleName { get; init; }
    public required string ObjectFileName { get; init; }
    public required int SymbolStreamIndex { get; init; }
    public required uint SymbolByteSize { get; init; }

    /// <summary>Primary section contribution (section:offset span) attributed to this module.</summary>
    public required SectionContribution Contribution { get; init; }

    public bool HasSymbols => SymbolStreamIndex >= 0 && SymbolByteSize > 0;
}

/// <summary>A (section, offset, size) span of image bytes attributed to a module.</summary>
public readonly record struct SectionContribution(ushort Section, int Offset, int Size, ushort ModuleIndex);
