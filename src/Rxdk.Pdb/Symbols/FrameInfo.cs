namespace Rxdk.Pdb.Symbols;

/// <summary>
/// A local variable located relative to the frame pointer (EBP on x86). <see cref="FrameOffset"/>
/// is added to the frame register to get the variable's address; <see cref="TypeIndex"/> resolves
/// against the TPI type system for size/shape.
/// </summary>
public sealed record LocalVariable(string Name, uint TypeIndex, long FrameOffset, bool IsParameter);

/// <summary>
/// The function whose code contains a queried RVA, plus its frame-pointer-relative locals. This is
/// the managed replacement for dbghelp's (broken) locals enumeration on Zig/LLVM PDBs.
/// </summary>
public sealed class FrameInfo
{
    public required string FunctionName { get; init; }
    public required uint FunctionRva { get; init; }
    public required uint CodeSize { get; init; }
    public required IReadOnlyList<LocalVariable> Locals { get; init; }
}
