using Rxdk.Pdb.Dbi;
using Rxdk.Pdb.Internal;
using Rxdk.Pdb.Msf;

namespace Rxdk.Pdb.Symbols;

/// <summary>
/// Reads per-module CodeView symbol streams to answer "what are the locals of the function at this
/// RVA?". Handles the modern S_LOCAL + S_DEFRANGE_FRAMEPOINTER_REL encoding (Zig/LLVM) as well as
/// the legacy S_BPREL32 / S_REGREL32 records, so the same reader works across toolchains.
/// </summary>
public sealed class SymbolReader
{
    private const int CodeViewSignatureC13 = 4;
    private const ushort CvRegEbp = 22; // x86 EBP
    private const ushort LocalFlagIsParameter = 0x0001;
    private const ushort LocalFlagCompilerGenerated = 0x0004;

    private readonly MsfFile _msf;
    private readonly DbiStream _dbi;

    public SymbolReader(MsfFile msf, DbiStream dbi)
    {
        _msf = msf;
        _dbi = dbi;
    }

    /// <summary>Finds the function whose code covers <paramref name="rva"/> and returns its locals.</summary>
    public FrameInfo? FindFrame(uint rva)
    {
        // Prefer the module the section contributions attribute this RVA to; fall back to scanning
        // all symbol-bearing modules (contribution data can be sparse/absent).
        var mapped = _dbi.FindModuleByRva(rva);
        if (mapped is { HasSymbols: true })
        {
            var hit = FindInModule(mapped, rva);
            if (hit is not null)
                return hit;
        }

        foreach (var module in _dbi.Modules)
        {
            if (!module.HasSymbols || ReferenceEquals(module, mapped))
                continue;
            var hit = FindInModule(module, rva);
            if (hit is not null)
                return hit;
        }

        return null;
    }

    /// <summary>Enumerates every function (with its frame-relative locals) across all modules.</summary>
    public IEnumerable<FrameInfo> EnumerateFunctions()
    {
        foreach (var module in _dbi.Modules)
        {
            if (!module.HasSymbols)
                continue;
            foreach (var frame in EnumerateModuleFrames(module))
                yield return frame;
        }
    }

    private FrameInfo? FindInModule(DbiModule module, uint rva)
    {
        foreach (var frame in EnumerateModuleFrames(module))
        {
            if (rva >= frame.FunctionRva && rva < frame.FunctionRva + frame.CodeSize)
                return frame;
        }

        return null;
    }

    private IEnumerable<FrameInfo> EnumerateModuleFrames(DbiModule module)
    {
        var stream = _msf.ReadStream(module.SymbolStreamIndex);
        if (stream.Length < 4)
            yield break;

        var r = new LeReader(stream) { Position = CodeViewSignatureC13 }; // skip the CV signature (C13)

        var depth = 0;
        var inlineDepth = 0; // locals inside inline sites belong to the inlined callee, not this frame
        string funcName = string.Empty;
        uint funcRva = 0;
        uint codeSize = 0;
        List<LocalVariable> locals = new();
        string? pendingName = null;
        uint pendingType = 0;
        bool pendingIsParam = false;

        while (r.Remaining >= 4)
        {
            var recLen = r.ReadUInt16();
            var recEnd = r.Position + recLen;
            if (recEnd > r.Length)
                break;
            var kind = (SymbolKind)r.ReadUInt16();

            switch (kind)
            {
                case SymbolKind.GProc32:
                case SymbolKind.LProc32:
                case SymbolKind.GProc32Id:
                case SymbolKind.LProc32Id:
                {
                    var proc = ReadProc(r);
                    if (depth == 0)
                    {
                        depth = 1;
                        funcName = proc.Name;
                        funcRva = _dbi.SectionOffsetToRva(proc.Segment, (int)proc.CodeOffset);
                        codeSize = proc.CodeSize;
                        locals = new List<LocalVariable>();
                        pendingName = null;
                        inlineDepth = 0;
                    }
                    else
                    {
                        depth++; // nested proc scope
                    }

                    break;
                }

                case SymbolKind.Block32:
                    if (depth > 0)
                        depth++;
                    break;

                case SymbolKind.InlineSite:
                    if (depth > 0)
                    {
                        depth++;
                        inlineDepth++;
                    }

                    break;

                case SymbolKind.End:
                case SymbolKind.ProcIdEnd:
                case SymbolKind.InlineSiteEnd:
                    if (depth > 0)
                    {
                        if (kind == SymbolKind.InlineSiteEnd && inlineDepth > 0)
                            inlineDepth--;
                        if (--depth == 0 && funcRva != 0)
                        {
                            yield return new FrameInfo
                            {
                                FunctionName = funcName,
                                FunctionRva = funcRva,
                                CodeSize = codeSize,
                                Locals = locals,
                            };
                        }
                    }

                    break;

                case SymbolKind.Local when depth > 0 && inlineDepth == 0:
                {
                    var type = r.ReadUInt32();
                    var flags = r.ReadUInt16();
                    var name = r.ReadCString();
                    if ((flags & LocalFlagCompilerGenerated) != 0)
                    {
                        pendingName = null;
                    }
                    else
                    {
                        pendingName = name;
                        pendingType = type;
                        pendingIsParam = (flags & LocalFlagIsParameter) != 0;
                    }

                    break;
                }

                case SymbolKind.DefRangeFramePointerRel when depth > 0 && inlineDepth == 0 && pendingName is not null:
                case SymbolKind.DefRangeFramePointerRelFullScope when depth > 0 && inlineDepth == 0 && pendingName is not null:
                {
                    var offset = r.ReadInt32(); // frame-pointer-relative offset
                    locals.Add(new LocalVariable(pendingName, pendingType, offset, pendingIsParam));
                    pendingName = null; // first range fixes the location
                    break;
                }

                case SymbolKind.BpRel32 when depth > 0 && inlineDepth == 0:
                {
                    var offset = r.ReadInt32();
                    var type = r.ReadUInt32();
                    var name = r.ReadCString();
                    locals.Add(new LocalVariable(name, type, offset, false));
                    break;
                }

                case SymbolKind.RegRel32 when depth > 0 && inlineDepth == 0:
                {
                    var offset = r.ReadUInt32();
                    var type = r.ReadUInt32();
                    var reg = r.ReadUInt16();
                    var name = r.ReadCString();
                    if (reg == CvRegEbp)
                        locals.Add(new LocalVariable(name, type, (int)offset, false));
                    break;
                }
            }

            r.Position = recEnd; // skip trailing bytes (def-range gaps, unparsed fields)
        }
    }

    private static ProcRecord ReadProc(LeReader r)
    {
        _ = r.ReadUInt32(); // parent
        _ = r.ReadUInt32(); // end
        _ = r.ReadUInt32(); // next
        var codeSize = r.ReadUInt32();
        _ = r.ReadUInt32(); // debug start
        _ = r.ReadUInt32(); // debug end
        _ = r.ReadUInt32(); // function type
        var codeOffset = r.ReadUInt32();
        var segment = r.ReadUInt16();
        _ = r.ReadByte();   // flags
        var name = r.ReadCString();
        return new ProcRecord(name, codeSize, codeOffset, segment);
    }

    private readonly record struct ProcRecord(string Name, uint CodeSize, uint CodeOffset, ushort Segment);
}
