using Rxdk.Pdb.Internal;
using Rxdk.Pdb.Msf;

namespace Rxdk.Pdb.Dbi;

/// <summary>
/// The DBI stream (fixed index 3) is the table of contents for debug info: it lists the modules
/// (each with its own symbol stream), the section contributions that map image ranges to modules,
/// and — via the optional debug header — the section-header stream used to turn (section:offset)
/// into an RVA. This is what lets us find the module + symbol stream that describes a given frame.
/// </summary>
public sealed class DbiStream
{
    public const int StreamIndex = 3;

    private const int HeaderSize = 64;
    private const int DbgHeaderSectionHeaders = 5; // slot index of the section-headers stream

    private readonly SectionContribution[] _contributions;
    private readonly SectionHeader[] _sections;

    public IReadOnlyList<DbiModule> Modules { get; }
    public IReadOnlyList<SectionHeader> Sections => _sections;

    private DbiStream(DbiModule[] modules, SectionContribution[] contributions, SectionHeader[] sections)
    {
        Modules = modules;
        _contributions = contributions;
        _sections = sections;
    }

    public static DbiStream Parse(MsfFile msf)
    {
        var stream = msf.ReadStream(StreamIndex);
        var r = new LeReader(stream);

        _ = r.ReadInt32();                    // VersionSignature (-1)
        _ = r.ReadUInt32();                   // VersionHeader
        _ = r.ReadUInt32();                   // Age
        _ = r.ReadUInt16();                   // GlobalStreamIndex
        _ = r.ReadUInt16();                   // BuildNumber
        _ = r.ReadUInt16();                   // PublicStreamIndex
        _ = r.ReadUInt16();                   // PdbDllVersion
        _ = r.ReadUInt16();                   // SymRecordStreamIndex
        _ = r.ReadUInt16();                   // PdbDllRbld
        var modInfoSize = r.ReadInt32();
        var secContrSize = r.ReadInt32();
        var sectionMapSize = r.ReadInt32();
        var sourceInfoSize = r.ReadInt32();
        var typeServerMapSize = r.ReadInt32();
        _ = r.ReadUInt32();                   // MFCTypeServerIndex
        var optionalDbgHeaderSize = r.ReadInt32();
        var ecSubstreamSize = r.ReadInt32();
        _ = r.ReadUInt16();                   // Flags
        _ = r.ReadUInt16();                   // Machine
        _ = r.ReadUInt32();                   // Padding

        var modules = ParseModuleInfo(stream, HeaderSize, modInfoSize);
        var contributions = ParseSectionContributions(stream, HeaderSize + modInfoSize, secContrSize);

        // Optional Debug Header is the last physical substream.
        var dbgHeaderOffset = HeaderSize + modInfoSize + secContrSize + sectionMapSize +
                              sourceInfoSize + typeServerMapSize + ecSubstreamSize;
        var sections = ParseSectionHeaders(msf, stream, dbgHeaderOffset, optionalDbgHeaderSize);

        return new DbiStream(modules, contributions, sections);
    }

    /// <summary>Converts a (1-based section, offset) pair to an image RVA, or 0 if out of range.</summary>
    public uint SectionOffsetToRva(ushort section, int offset)
    {
        if (section == 0 || section > _sections.Length)
            return 0;
        return _sections[section - 1].VirtualAddress + (uint)offset;
    }

    /// <summary>Finds the module whose section contribution covers the given RVA, or null.</summary>
    public DbiModule? FindModuleByRva(uint rva)
    {
        foreach (var c in _contributions)
        {
            var start = SectionOffsetToRva(c.Section, c.Offset);
            if (start == 0)
                continue;
            if (rva >= start && rva < start + (uint)c.Size)
                return c.ModuleIndex < Modules.Count ? Modules[c.ModuleIndex] : null;
        }

        return null;
    }

    private static DbiModule[] ParseModuleInfo(byte[] stream, int start, int size)
    {
        var modules = new List<DbiModule>();
        var r = new LeReader(stream) { Position = start };
        var end = start + size;

        var index = 0;
        while (r.Position + 64 <= end)
        {
            _ = r.ReadUInt32();               // Unused1
            var contribution = ReadSectionContribution(r);
            _ = r.ReadUInt16();               // Flags
            var symStream = r.ReadInt16();
            var symByteSize = r.ReadUInt32();
            _ = r.ReadUInt32();               // C11ByteSize
            _ = r.ReadUInt32();               // C13ByteSize
            _ = r.ReadUInt16();               // SourceFileCount
            _ = r.ReadUInt16();               // Padding
            _ = r.ReadUInt32();               // Unused2
            _ = r.ReadUInt32();               // SourceFileNameIndex
            _ = r.ReadUInt32();               // PdbFilePathNameIndex
            var moduleName = r.ReadCString();
            var objName = r.ReadCString();
            r.Align(4);

            modules.Add(new DbiModule
            {
                Index = index++,
                ModuleName = moduleName,
                ObjectFileName = objName,
                SymbolStreamIndex = symStream,
                SymbolByteSize = symByteSize,
                Contribution = contribution,
            });
        }

        return modules.ToArray();
    }

    private static SectionContribution[] ParseSectionContributions(byte[] stream, int start, int size)
    {
        if (size <= 4)
            return Array.Empty<SectionContribution>();

        var r = new LeReader(stream) { Position = start };
        var version = r.ReadUInt32();
        // Ver60 = 0xF12EBA2D (28-byte entries). V2 = 0xF13151E4 (adds a trailing ISectCoff u32).
        var hasIsectCoff = version == 0xF13151E4;
        var entrySize = hasIsectCoff ? 32 : 28;
        var end = start + size;

        var result = new List<SectionContribution>();
        while (r.Position + entrySize <= end)
        {
            var contribution = ReadSectionContribution(r);
            if (hasIsectCoff)
                r.ReadUInt32();
            result.Add(contribution);
        }

        return result.ToArray();
    }

    private static SectionContribution ReadSectionContribution(LeReader r)
    {
        var section = r.ReadUInt16();
        _ = r.ReadUInt16();                   // padding
        var offset = r.ReadInt32();
        var size = r.ReadInt32();
        _ = r.ReadUInt32();                   // characteristics
        var moduleIndex = r.ReadUInt16();
        _ = r.ReadUInt16();                   // padding
        _ = r.ReadUInt32();                   // data CRC
        _ = r.ReadUInt32();                   // reloc CRC
        return new SectionContribution(section, offset, size, moduleIndex);
    }

    private static SectionHeader[] ParseSectionHeaders(MsfFile msf, byte[] stream, int dbgHeaderOffset, int dbgHeaderSize)
    {
        var slotCount = dbgHeaderSize / 2;
        if (slotCount <= DbgHeaderSectionHeaders)
            return Array.Empty<SectionHeader>();

        var r = new LeReader(stream) { Position = dbgHeaderOffset + DbgHeaderSectionHeaders * 2 };
        var streamIndex = r.ReadUInt16();
        if (streamIndex == 0xFFFF || streamIndex >= msf.StreamCount)
            return Array.Empty<SectionHeader>();

        return SectionHeader.ParseAll(msf.ReadStream(streamIndex));
    }
}
