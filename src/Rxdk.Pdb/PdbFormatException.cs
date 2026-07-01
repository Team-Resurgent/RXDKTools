namespace Rxdk.Pdb;

/// <summary>Thrown when a PDB/MSF structure is malformed or unsupported.</summary>
public sealed class PdbFormatException : Exception
{
    public PdbFormatException(string message) : base(message) { }

    public PdbFormatException(string message, Exception inner) : base(message, inner) { }
}
