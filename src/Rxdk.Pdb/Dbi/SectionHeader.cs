using Rxdk.Pdb.Internal;

namespace Rxdk.Pdb.Dbi;

/// <summary>
/// A PE IMAGE_SECTION_HEADER as stored in the PDB's section-header debug stream. Symbols address
/// code as (1-based section, offset); these headers convert that pair to an image RVA.
/// </summary>
public sealed class SectionHeader
{
    public required string Name { get; init; }
    public required uint VirtualSize { get; init; }
    public required uint VirtualAddress { get; init; } // RVA of the section
    public required uint SizeOfRawData { get; init; }
    public required uint Characteristics { get; init; }

    internal static SectionHeader[] ParseAll(byte[] stream)
    {
        const int recordSize = 40;
        var count = stream.Length / recordSize;
        var result = new SectionHeader[count];
        var r = new LeReader(stream);
        for (var i = 0; i < count; i++)
        {
            var name = System.Text.Encoding.ASCII.GetString(r.ReadBytes(8)).TrimEnd('\0');
            var virtualSize = r.ReadUInt32();
            var virtualAddress = r.ReadUInt32();
            var sizeOfRawData = r.ReadUInt32();
            r.ReadUInt32(); // PointerToRawData
            r.ReadUInt32(); // PointerToRelocations
            r.ReadUInt32(); // PointerToLinenumbers
            r.ReadUInt16(); // NumberOfRelocations
            r.ReadUInt16(); // NumberOfLinenumbers
            var characteristics = r.ReadUInt32();
            result[i] = new SectionHeader
            {
                Name = name,
                VirtualSize = virtualSize,
                VirtualAddress = virtualAddress,
                SizeOfRawData = sizeOfRawData,
                Characteristics = characteristics,
            };
        }

        return result;
    }
}
