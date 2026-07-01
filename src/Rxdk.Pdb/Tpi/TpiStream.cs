using Rxdk.Pdb.Internal;

namespace Rxdk.Pdb.Tpi;

/// <summary>
/// The TPI stream (fixed index 2) holds the CodeView type records. Each record is a
/// length-prefixed blob whose first u16 is its leaf kind. Type indices are assigned
/// sequentially from <see cref="TypeIndexBegin"/>; indices below it are primitive types
/// resolved by formula rather than by record.
/// </summary>
public sealed class TpiStream
{
    public const int StreamIndex = 2;

    private readonly byte[] _records;    // the type-record region (after the header)
    private readonly int[] _recordStart; // per-type-index start offset into _records
    private readonly int[] _recordLen;   // per-type-index record length (incl. leaf u16)

    public uint Version { get; }
    public uint TypeIndexBegin { get; }
    public uint TypeIndexEnd { get; }

    private TpiStream(uint version, uint begin, uint end, byte[] records, int[] starts, int[] lens)
    {
        Version = version;
        TypeIndexBegin = begin;
        TypeIndexEnd = end;
        _records = records;
        _recordStart = starts;
        _recordLen = lens;
    }

    public static TpiStream Parse(byte[] stream)
    {
        var r = new LeReader(stream);
        var version = r.ReadUInt32();
        var headerSize = (int)r.ReadUInt32();
        var begin = r.ReadUInt32();
        var end = r.ReadUInt32();
        var typeRecordBytes = (int)r.ReadUInt32();

        if (headerSize < 0 || headerSize > stream.Length)
            throw new PdbFormatException($"Bad TPI header size {headerSize}.");
        if ((long)headerSize + typeRecordBytes > stream.Length)
            throw new PdbFormatException("TPI record bytes exceed stream size.");

        var records = new byte[typeRecordBytes];
        Array.Copy(stream, headerSize, records, 0, typeRecordBytes);

        var count = (int)(end - begin);
        var starts = new int[count];
        var lens = new int[count];

        var rr = new LeReader(records);
        for (var i = 0; i < count && rr.Remaining >= 2; i++)
        {
            var len = rr.ReadUInt16();       // bytes following this length field
            starts[i] = rr.Position;         // points at the leaf u16
            lens[i] = len;
            rr.Position += len;
        }

        return new TpiStream(version, begin, end, records, starts, lens);
    }

    /// <summary>Number of record-backed types (i.e. TypeIndexEnd - TypeIndexBegin).</summary>
    public int RecordCount => _recordStart.Length;

    public bool IsRecordIndex(uint typeIndex) => typeIndex >= TypeIndexBegin && typeIndex < TypeIndexEnd;

    /// <summary>Returns the raw record bytes for a type index (starting at its leaf u16).</summary>
    public ReadOnlyMemory<byte> GetRecord(uint typeIndex)
    {
        if (!IsRecordIndex(typeIndex))
            throw new ArgumentOutOfRangeException(nameof(typeIndex), $"0x{typeIndex:x} is not a record type.");
        var slot = (int)(typeIndex - TypeIndexBegin);
        return _records.AsMemory(_recordStart[slot], _recordLen[slot]);
    }

    /// <summary>Enumerates every record type index in ascending order.</summary>
    public IEnumerable<uint> TypeIndices()
    {
        for (var i = TypeIndexBegin; i < TypeIndexEnd; i++)
            yield return i;
    }
}
