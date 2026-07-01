using System.Text;
using Rxdk.Pdb.Internal;

namespace Rxdk.Pdb.Msf;

/// <summary>
/// Reader for the MSF (Multi-Stream File) container that wraps every modern PDB. The MSF splits
/// the file into fixed-size blocks and stores a directory that maps each logical stream to the
/// list of blocks holding its bytes. This class parses the superblock + stream directory and
/// materializes individual streams on demand.
/// </summary>
public sealed class MsfFile
{
    // "Microsoft C/C++ MSF 7.00\r\n" 0x1A "DS" 0 0 0  (32 bytes)
    private static readonly byte[] Magic7 = Encoding.ASCII.GetBytes("Microsoft C/C++ MSF 7.00\r\nDS\0\0\0");

    private readonly ReadOnlyMemory<byte> _image;
    private readonly int _blockSize;
    private readonly uint[] _streamSizes;
    private readonly uint[][] _streamBlocks;

    private MsfFile(ReadOnlyMemory<byte> image, int blockSize, uint[] streamSizes, uint[][] streamBlocks)
    {
        _image = image;
        _blockSize = blockSize;
        _streamSizes = streamSizes;
        _streamBlocks = streamBlocks;
    }

    /// <summary>Block size in bytes (typically 4096).</summary>
    public int BlockSize => _blockSize;

    /// <summary>Number of logical streams described by the directory.</summary>
    public int StreamCount => _streamSizes.Length;

    public static MsfFile Open(byte[] image) => Open((ReadOnlyMemory<byte>)image);

    public static MsfFile Open(ReadOnlyMemory<byte> image)
    {
        var span = image.Span;
        if (span.Length < 56 || !span[..Magic7.Length].SequenceEqual(Magic7))
            throw new PdbFormatException("Not an MSF 7.00 container (bad magic).");

        var header = new LeReader(image) { Position = Magic7.Length };
        var blockSize = (int)header.ReadUInt32();
        _ = header.ReadUInt32();               // FreeBlockMapBlock
        var numBlocks = header.ReadUInt32();   // total blocks in the file
        var numDirectoryBytes = header.ReadUInt32();
        _ = header.ReadUInt32();               // Unknown
        var blockMapAddr = header.ReadUInt32(); // block holding the array of directory-block indices

        if (blockSize <= 0 || (blockSize & (blockSize - 1)) != 0)
            throw new PdbFormatException($"Invalid MSF block size {blockSize}.");
        if ((long)numBlocks * blockSize > image.Length)
            throw new PdbFormatException("MSF block count exceeds file size.");

        // The directory itself is a stream. Its block list lives at blockMapAddr.
        var directoryBlockCount = CeilDiv(numDirectoryBytes, (uint)blockSize);
        var dirBlockList = new uint[directoryBlockCount];
        var mapReader = new LeReader(image) { Position = checked((int)(blockMapAddr * (uint)blockSize)) };
        for (var i = 0; i < directoryBlockCount; i++)
            dirBlockList[i] = mapReader.ReadUInt32();

        var directory = ReadBlocks(image, blockSize, dirBlockList, numDirectoryBytes);
        var dir = new LeReader(directory);

        var numStreams = dir.ReadUInt32();
        var streamSizes = new uint[numStreams];
        for (var i = 0; i < numStreams; i++)
        {
            var size = dir.ReadUInt32();
            streamSizes[i] = size == 0xFFFFFFFF ? 0 : size; // 0xFFFFFFFF = nil stream
        }

        var streamBlocks = new uint[numStreams][];
        for (var i = 0; i < numStreams; i++)
        {
            var count = (int)CeilDiv(streamSizes[i], (uint)blockSize);
            var blocks = new uint[count];
            for (var b = 0; b < count; b++)
                blocks[b] = dir.ReadUInt32();
            streamBlocks[i] = blocks;
        }

        return new MsfFile(image, blockSize, streamSizes, streamBlocks);
    }

    /// <summary>Size in bytes of the given stream (0 for a nil/empty stream).</summary>
    public uint StreamSize(int index) => _streamSizes[index];

    /// <summary>Materializes the given stream's bytes by concatenating its blocks.</summary>
    public byte[] ReadStream(int index)
    {
        if ((uint)index >= (uint)_streamSizes.Length)
            throw new ArgumentOutOfRangeException(nameof(index));
        return ReadBlocks(_image, _blockSize, _streamBlocks[index], _streamSizes[index]);
    }

    private static byte[] ReadBlocks(ReadOnlyMemory<byte> image, int blockSize, uint[] blocks, uint totalBytes)
    {
        var result = new byte[totalBytes];
        var written = 0;
        var span = image.Span;
        foreach (var block in blocks)
        {
            var offset = checked((int)(block * (uint)blockSize));
            var take = Math.Min(blockSize, (int)totalBytes - written);
            if (take <= 0)
                break;
            span.Slice(offset, take).CopyTo(result.AsSpan(written));
            written += take;
        }

        return result;
    }

    private static uint CeilDiv(uint value, uint divisor) => (value + divisor - 1) / divisor;
}
