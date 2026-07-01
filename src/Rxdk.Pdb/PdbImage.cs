using Rxdk.Pdb.Msf;
using Rxdk.Pdb.Pdb;

namespace Rxdk.Pdb;

/// <summary>
/// Top-level entry point for reading a PDB. Opens the MSF container and exposes the well-known
/// streams. Higher layers (TPI types, DBI modules, per-module symbols) build on this.
/// </summary>
public sealed class PdbImage
{
    private readonly MsfFile _msf;
    private PdbInfoStream? _info;

    private PdbImage(MsfFile msf) => _msf = msf;

    public static PdbImage Open(byte[] image) => new(MsfFile.Open(image));

    public static PdbImage OpenFile(string path) => Open(File.ReadAllBytes(path));

    /// <summary>The underlying MSF container (stream access, block size, stream count).</summary>
    public MsfFile Msf => _msf;

    /// <summary>PDB Information stream (version, signature, age, GUID, named streams).</summary>
    public PdbInfoStream Info => _info ??= PdbInfoStream.Parse(_msf.ReadStream(PdbInfoStream.StreamIndex));
}
