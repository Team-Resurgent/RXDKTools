using Rxdk.Pdb.Dbi;
using Rxdk.Pdb.Msf;
using Rxdk.Pdb.Pdb;
using Rxdk.Pdb.Symbols;
using Rxdk.Pdb.Tpi;

namespace Rxdk.Pdb;

/// <summary>
/// Top-level entry point for reading a PDB. Opens the MSF container and exposes the well-known
/// streams. Higher layers (TPI types, DBI modules, per-module symbols) build on this.
/// </summary>
public sealed class PdbImage
{
    private readonly MsfFile _msf;
    private PdbInfoStream? _info;
    private TpiStream? _tpi;
    private TypeSystem? _types;
    private DbiStream? _dbi;
    private SymbolReader? _symbols;

    private PdbImage(MsfFile msf) => _msf = msf;

    public static PdbImage Open(byte[] image) => new(MsfFile.Open(image));

    public static PdbImage OpenFile(string path) => Open(File.ReadAllBytes(path));

    /// <summary>The underlying MSF container (stream access, block size, stream count).</summary>
    public MsfFile Msf => _msf;

    /// <summary>PDB Information stream (version, signature, age, GUID, named streams).</summary>
    public PdbInfoStream Info => _info ??= PdbInfoStream.Parse(_msf.ReadStream(PdbInfoStream.StreamIndex));

    /// <summary>TPI stream (CodeView type records).</summary>
    public TpiStream Tpi => _tpi ??= TpiStream.Parse(_msf.ReadStream(TpiStream.StreamIndex));

    /// <summary>Type resolver over the TPI stream (sizes, names, pointer/array shape, members).</summary>
    public TypeSystem Types => _types ??= new TypeSystem(Tpi);

    /// <summary>DBI stream (modules, section contributions, section headers; RVA→module lookup).</summary>
    public DbiStream Dbi => _dbi ??= DbiStream.Parse(_msf);

    /// <summary>Per-module symbol reader (procedures + frame-relative locals).</summary>
    public SymbolReader Symbols => _symbols ??= new SymbolReader(_msf, Dbi);

    /// <summary>Finds the function containing an image RVA and returns its frame-relative locals.</summary>
    public FrameInfo? FindFrame(uint rva) => Symbols.FindFrame(rva);
}
