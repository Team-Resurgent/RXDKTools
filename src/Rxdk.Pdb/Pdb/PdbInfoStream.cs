using Rxdk.Pdb.Internal;

namespace Rxdk.Pdb.Pdb;

/// <summary>
/// Parsed PDB Information stream (fixed stream index 1): the format version, the signature/age
/// used to match the PDB against its image, the module GUID, and the named-stream table (which
/// maps names like "/names" to stream indices).
/// </summary>
public sealed class PdbInfoStream
{
    public const int StreamIndex = 1;

    public uint Version { get; private init; }
    public uint Signature { get; private init; }
    public uint Age { get; private init; }
    public Guid Guid { get; private init; }
    public IReadOnlyDictionary<string, int> NamedStreams { get; private init; } = new Dictionary<string, int>();

    public static PdbInfoStream Parse(byte[] stream)
    {
        var r = new LeReader(stream);
        var version = r.ReadUInt32();
        var signature = r.ReadUInt32();
        var age = r.ReadUInt32();
        var guid = r.ReadGuid();

        var named = ParseNamedStreamMap(r);

        return new PdbInfoStream
        {
            Version = version,
            Signature = signature,
            Age = age,
            Guid = guid,
            NamedStreams = named,
        };
    }

    /// <summary>
    /// The named-stream map is a string buffer followed by a serialized hash table of
    /// (name-offset → stream-index) pairs. We only need the mapping, so we read the string
    /// buffer, then walk the present entries.
    /// </summary>
    private static Dictionary<string, int> ParseNamedStreamMap(LeReader r)
    {
        var result = new Dictionary<string, int>(StringComparer.Ordinal);

        var stringBufferSize = (int)r.ReadUInt32();
        var stringBufferStart = r.Position;
        r.Position += stringBufferSize;

        // Serialized hash table: Size, Capacity, then a "present" bit vector, then a "deleted"
        // bit vector, then Size (key, value) pairs where key is an offset into the string buffer.
        var size = (int)r.ReadUInt32();
        _ = r.ReadUInt32(); // capacity

        SkipBitVector(r);   // present set
        SkipBitVector(r);   // deleted set

        for (var i = 0; i < size; i++)
        {
            var nameOffset = (int)r.ReadUInt32();
            var streamIndex = (int)r.ReadUInt32();
            var name = ReadStringAt(r, stringBufferStart, nameOffset);
            if (!string.IsNullOrEmpty(name))
                result[name] = streamIndex;
        }

        return result;
    }

    private static void SkipBitVector(LeReader r)
    {
        var wordCount = (int)r.ReadUInt32();
        r.Position += wordCount * 4;
    }

    private static string ReadStringAt(LeReader r, int bufferStart, int offset)
    {
        var saved = r.Position;
        r.Position = bufferStart + offset;
        var value = r.ReadCString();
        r.Position = saved;
        return value;
    }
}
