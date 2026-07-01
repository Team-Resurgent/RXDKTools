using System.Buffers.Binary;
using System.Text;

namespace Rxdk.Pdb.Internal;

/// <summary>
/// Sequential little-endian reader over a byte buffer. PDB/CodeView structures are all
/// little-endian and densely packed, so a cursor-based reader keeps the parsers readable.
/// </summary>
internal sealed class LeReader
{
    private readonly ReadOnlyMemory<byte> _data;

    internal LeReader(ReadOnlyMemory<byte> data) => _data = data;

    internal int Position { get; set; }

    internal int Length => _data.Length;

    internal bool AtEnd => Position >= _data.Length;

    internal int Remaining => _data.Length - Position;

    internal ReadOnlySpan<byte> Span => _data.Span;

    internal byte ReadByte()
    {
        var value = _data.Span[Position];
        Position += 1;
        return value;
    }

    internal short ReadInt16()
    {
        var value = BinaryPrimitives.ReadInt16LittleEndian(_data.Span.Slice(Position, 2));
        Position += 2;
        return value;
    }

    internal ushort ReadUInt16()
    {
        var value = BinaryPrimitives.ReadUInt16LittleEndian(_data.Span.Slice(Position, 2));
        Position += 2;
        return value;
    }

    internal int ReadInt32()
    {
        var value = BinaryPrimitives.ReadInt32LittleEndian(_data.Span.Slice(Position, 4));
        Position += 4;
        return value;
    }

    internal uint ReadUInt32()
    {
        var value = BinaryPrimitives.ReadUInt32LittleEndian(_data.Span.Slice(Position, 4));
        Position += 4;
        return value;
    }

    internal ulong ReadUInt64()
    {
        var value = BinaryPrimitives.ReadUInt64LittleEndian(_data.Span.Slice(Position, 8));
        Position += 8;
        return value;
    }

    internal Guid ReadGuid()
    {
        var value = new Guid(_data.Span.Slice(Position, 16));
        Position += 16;
        return value;
    }

    internal ReadOnlySpan<byte> ReadBytes(int count)
    {
        var value = _data.Span.Slice(Position, count);
        Position += count;
        return value;
    }

    /// <summary>Reads a NUL-terminated ASCII/UTF-8 string and advances past the terminator.</summary>
    internal string ReadCString()
    {
        var span = _data.Span;
        var start = Position;
        while (Position < span.Length && span[Position] != 0)
            Position++;
        var text = Encoding.UTF8.GetString(span.Slice(start, Position - start));
        if (Position < span.Length)
            Position++; // consume terminator
        return text;
    }

    /// <summary>
    /// Reads a CodeView "numeric leaf": a u16 that is either an immediate value (&lt; LF_NUMERIC
    /// 0x8000) or a type code introducing a wider inline value. Used for struct sizes, array
    /// lengths, and member offsets. Returns the magnitude as a signed 64-bit value.
    /// </summary>
    internal long ReadNumericLeaf()
    {
        var kind = ReadUInt16();
        if (kind < 0x8000)
            return kind;

        return kind switch
        {
            0x8000 => (sbyte)ReadByte(),   // LF_CHAR
            0x8001 => ReadInt16(),          // LF_SHORT
            0x8002 => ReadUInt16(),         // LF_USHORT
            0x8003 => ReadInt32(),          // LF_LONG
            0x8004 => ReadUInt32(),         // LF_ULONG
            0x8009 => (long)ReadUInt64(),   // LF_QUADWORD
            0x800A => (long)ReadUInt64(),   // LF_UQUADWORD
            _ => throw new NotSupportedException($"Unsupported numeric leaf 0x{kind:x4}."),
        };
    }

    /// <summary>CodeView symbol records 4-byte-align their trailing name; skip to the next boundary.</summary>
    internal void Align(int alignment)
    {
        var rem = Position % alignment;
        if (rem != 0)
            Position += alignment - rem;
    }
}
