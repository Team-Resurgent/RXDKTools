using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Text;

namespace Rxdk.XbeImage;

public static class Pe32Helpers
{
    public static int RoundToPages(int size) =>
        (size + XbeImageConstants.PageSize - 1) & ~(XbeImageConstants.PageSize - 1);

    public static int PageAlign(int virtualAddress) =>
        virtualAddress & ~(XbeImageConstants.PageSize - 1);

    public static int ByteOffset(int virtualAddress) =>
        virtualAddress & (XbeImageConstants.PageSize - 1);

    public static bool SnapByOrdinal(uint ordinal) =>
        (ordinal & XbeImageConstants.ImageOrdinalFlag32) != 0;

    public static int GetNtHeaderOffset(ReadOnlySpan<byte> image)
    {
        return BinaryPrimitives.ReadInt32LittleEndian(image[60..]);
    }

    public static ref ImageNtHeaders32 GetNtHeaders(Span<byte> image)
    {
        var ntOffset = GetNtHeaderOffset(image);
        return ref MemoryMarshal.AsRef<ImageNtHeaders32>(image[ntOffset..]);
    }

    public static int GetFirstSectionOffset(ref ImageNtHeaders32 ntHeaders) =>
        Marshal.SizeOf<uint>() + Marshal.SizeOf<ImageFileHeader>() + ntHeaders.FileHeader.SizeOfOptionalHeader;

    public static int GetSectionHeaderFileOffset(ReadOnlySpan<byte> image, int sectionIndex)
    {
        var ntOffset = GetNtHeaderOffset(image);
        var sizeOfOptionalHeader = BinaryPrimitives.ReadUInt16LittleEndian(image[(ntOffset + Marshal.SizeOf<uint>() + 16)..]);
        return ntOffset + Marshal.SizeOf<uint>() + Marshal.SizeOf<ImageFileHeader>() + sizeOfOptionalHeader +
               sectionIndex * Marshal.SizeOf<ImageSectionHeader>();
    }

    public static ImageSectionHeader ReadSectionHeader(ReadOnlySpan<byte> image, int sectionIndex)
    {
        var offset = GetSectionHeaderFileOffset(image, sectionIndex);
        return ReadSectionHeaderAt(image, offset);
    }

    public static ImageSectionHeader ReadSectionHeaderAt(ReadOnlySpan<byte> image, int offset)
    {
        var header = new ImageSectionHeader
        {
            Name = image.Slice(offset, XbeImageConstants.ImageSizeofShortName).ToArray(),
            VirtualSize = BinaryPrimitives.ReadUInt32LittleEndian(image[(offset + 8)..]),
            VirtualAddress = BinaryPrimitives.ReadUInt32LittleEndian(image[(offset + 12)..]),
            SizeOfRawData = BinaryPrimitives.ReadUInt32LittleEndian(image[(offset + 16)..]),
            PointerToRawData = BinaryPrimitives.ReadUInt32LittleEndian(image[(offset + 20)..]),
            PointerToRelocations = BinaryPrimitives.ReadUInt32LittleEndian(image[(offset + 24)..]),
            PointerToLinenumbers = BinaryPrimitives.ReadUInt32LittleEndian(image[(offset + 28)..]),
            NumberOfRelocations = BinaryPrimitives.ReadUInt16LittleEndian(image[(offset + 32)..]),
            NumberOfLinenumbers = BinaryPrimitives.ReadUInt16LittleEndian(image[(offset + 34)..]),
            Characteristics = BinaryPrimitives.ReadUInt32LittleEndian(image[(offset + 36)..]),
        };
        return header;
    }

    public static void WriteSectionHeader(Span<byte> image, int sectionIndex, ImageSectionHeader header)
    {
        var offset = GetSectionHeaderFileOffset(image, sectionIndex);
        header.Name.AsSpan(0, XbeImageConstants.ImageSizeofShortName).CopyTo(image[offset..]);
        BinaryPrimitives.WriteUInt32LittleEndian(image[(offset + 8)..], header.VirtualSize);
        BinaryPrimitives.WriteUInt32LittleEndian(image[(offset + 12)..], header.VirtualAddress);
        BinaryPrimitives.WriteUInt32LittleEndian(image[(offset + 16)..], header.SizeOfRawData);
        BinaryPrimitives.WriteUInt32LittleEndian(image[(offset + 20)..], header.PointerToRawData);
        BinaryPrimitives.WriteUInt32LittleEndian(image[(offset + 36)..], header.Characteristics);
    }

    public static int SectionCount(Span<byte> image) => GetNtHeaders(image).FileHeader.NumberOfSections;

    public static ImageSectionHeader? NameToSectionHeader(
        Span<byte> image,
        ref ImageNtHeaders32 ntHeaders,
        string searchName,
        out int sectionIndex)
    {
        var count = ntHeaders.FileHeader.NumberOfSections;
        for (sectionIndex = 0; sectionIndex < count; sectionIndex++)
        {
            var section = ReadSectionHeader(image, sectionIndex);
            if (string.Equals(GetSectionName(section), searchName, StringComparison.OrdinalIgnoreCase))
            {
                return section;
            }
        }

        sectionIndex = -1;
        return null;
    }

    public static ImageSectionHeader VirtualAddressToSectionHeader(
        Span<byte> image,
        ref ImageNtHeaders32 ntHeaders,
        uint virtualAddress,
        string inputPath)
    {
        for (var i = 0; i < ntHeaders.FileHeader.NumberOfSections; i++)
        {
            var section = ReadSectionHeader(image, i);
            var sectionSpan = Math.Max(section.VirtualSize, section.SizeOfRawData);
            if (virtualAddress >= section.VirtualAddress &&
                virtualAddress < section.VirtualAddress + sectionSpan)
            {
                return section;
            }
        }

        throw new XbeImageException($"Invalid or corrupt input file: {inputPath}");
    }

    public static Span<byte> VirtualAddressToData(
        Span<byte> image,
        ref ImageNtHeaders32 ntHeaders,
        uint virtualAddress,
        string inputPath)
    {
        var section = VirtualAddressToSectionHeader(image, ref ntHeaders, virtualAddress, inputPath);
        var offset = (int)(section.PointerToRawData + (virtualAddress - section.VirtualAddress));
        return image[offset..];
    }

    public static Span<byte> ImageDataDirectoryToData(
        Span<byte> image,
        ref ImageNtHeaders32 ntHeaders,
        int directoryIndex,
        string inputPath)
    {
        var directory = Pe32Reader.GetDataDirectory(image, directoryIndex);
        return VirtualAddressToData(image, ref ntHeaders, directory.VirtualAddress, inputPath);
    }

    public static Span<byte> LoadAddressToData(
        Span<byte> image,
        ref ImageNtHeaders32 ntHeaders,
        uint loadAddress,
        string inputPath)
    {
        var virtualAddress = loadAddress - ntHeaders.OptionalHeader.ImageBase;
        return VirtualAddressToData(image, ref ntHeaders, virtualAddress, inputPath);
    }

    public static void RelocateImage(
        Span<byte> image,
        ref ImageNtHeaders32 ntHeaders,
        uint oldBaseAddress,
        uint newBaseAddress,
        string inputPath)
    {
        var directory = Pe32Reader.GetDataDirectory(image, XbeImageConstants.ImageDirectoryEntryBaseReloc);
        if (directory.Size == 0)
        {
            throw new XbeImageException("Missing relocation records.");
        }

        var remaining = (int)directory.Size;
        var relocDirectory = ImageDataDirectoryToData(image, ref ntHeaders, XbeImageConstants.ImageDirectoryEntryBaseReloc, inputPath);
        var diff = (int)newBaseAddress - (int)oldBaseAddress;
        var blockOffset = 0;
        while (remaining > 0)
        {
            var blockVa = BinaryPrimitives.ReadUInt32LittleEndian(relocDirectory[blockOffset..]);
            var sizeOfBlock = BinaryPrimitives.ReadInt32LittleEndian(relocDirectory[(blockOffset + 4)..]);
            remaining -= sizeOfBlock;
            var entryCount = (sizeOfBlock - Marshal.SizeOf<ImageBaseRelocation>()) / sizeof(ushort);
            var entryIndex = 0;
            while (entryCount-- > 0)
            {
                var entry = BinaryPrimitives.ReadUInt16LittleEndian(
                    relocDirectory[(blockOffset + Marshal.SizeOf<ImageBaseRelocation>() + entryIndex * 2)..]);
                var type = entry >> 12;
                var fixupOffset = entry & 0x0FFF;
                switch (type)
                {
                    case XbeImageConstants.ImageRelBasedHighLow:
                    {
                        var fixup = VirtualAddressToData(image, ref ntHeaders, (uint)(blockVa + fixupOffset), inputPath);
                        BinaryPrimitives.WriteInt32LittleEndian(fixup, BinaryPrimitives.ReadInt32LittleEndian(fixup) + diff);
                        break;
                    }
                    case XbeImageConstants.ImageRelBasedHigh:
                    {
                        var fixup = VirtualAddressToData(image, ref ntHeaders, (uint)(blockVa + fixupOffset), inputPath);
                        var temp = (BinaryPrimitives.ReadInt16LittleEndian(fixup) << 16) + diff;
                        BinaryPrimitives.WriteInt16LittleEndian(fixup, (short)(temp >> 16));
                        break;
                    }
                    case XbeImageConstants.ImageRelBasedHighAdj:
                    {
                        var fixup = VirtualAddressToData(image, ref ntHeaders, (uint)(blockVa + fixupOffset), inputPath);
                        var temp = BinaryPrimitives.ReadInt16LittleEndian(fixup) << 16;
                        entryIndex++;
                        entryCount--;
                        temp += BinaryPrimitives.ReadInt16LittleEndian(
                            relocDirectory[(blockOffset + Marshal.SizeOf<ImageBaseRelocation>() + entryIndex * 2)..]);
                        temp += diff + 0x8000;
                        BinaryPrimitives.WriteInt16LittleEndian(fixup, (short)(temp >> 16));
                        break;
                    }
                    case XbeImageConstants.ImageRelBasedLow:
                    {
                        var fixup = VirtualAddressToData(image, ref ntHeaders, (uint)(blockVa + fixupOffset), inputPath);
                        BinaryPrimitives.WriteInt16LittleEndian(fixup, (short)(BinaryPrimitives.ReadInt16LittleEndian(fixup) + diff));
                        break;
                    }
                }

                entryIndex++;
            }

            blockOffset += sizeOfBlock;
        }
    }

    public static string GetSectionName(in ImageSectionHeader section)
    {
        var end = section.Name.AsSpan().IndexOf((byte)0);
        if (end < 0)
        {
            end = XbeImageConstants.ImageSizeofShortName;
        }

        return Encoding.ASCII.GetString(section.Name, 0, end);
    }

    public static void ValidatePe32Image(Span<byte> image, string inputPath)
    {
        if (image.Length < Marshal.SizeOf<ImageDosHeader>())
        {
            throw new XbeImageException($"Invalid or corrupt input file: {inputPath}");
        }

        if (BinaryPrimitives.ReadUInt16LittleEndian(image) != XbeImageConstants.ImageDosSignature)
        {
            throw new XbeImageException($"Invalid or corrupt input file: {inputPath}");
        }

        var ntOffset = GetNtHeaderOffset(image);
        if (image.Length < ntOffset + Marshal.SizeOf<ImageNtHeaders32>())
        {
            throw new XbeImageException($"Invalid or corrupt input file: {inputPath}");
        }

        ref var nt = ref GetNtHeaders(image);
        if (nt.Signature != XbeImageConstants.ImageNtSignature ||
            nt.OptionalHeader.Magic != XbeImageConstants.ImageNtOptionalHdr32Magic)
        {
            throw new XbeImageException($"Invalid or corrupt input file: {inputPath}");
        }

        if (nt.FileHeader.Machine != XbeImageConstants.ImageFileMachineI386)
        {
            throw new XbeImageException("Input file is not a PE32 I386 image.");
        }

        // imagebld only ever emits Xbox images, and the i386 machine check above
        // already gates architecture, so the input PE subsystem is informational.
        // The MS Xbox linker emits IMAGE_SUBSYSTEM_XBOX directly; other linkers
        // (lld-link, link.exe) emit CONSOLE/GUI/unknown. Coerce whatever it is to
        // Xbox rather than rejecting it, so callers don't need a separate PE-patch
        // pre-pass just to flip the subsystem byte.
        ref var optional = ref Pe32Reader.GetOptionalHeader(image);
        if (optional.Subsystem != XbeImageConstants.ImageSubsystemXbox)
        {
            optional.Subsystem = XbeImageConstants.ImageSubsystemXbox;
        }
    }

    public static int TrimSectionRawDataSize(Span<byte> image, ref ImageNtHeaders32 ntHeaders, ImageSectionHeader section, string inputPath)
    {
        var rawSize = (int)section.SizeOfRawData;
        if (rawSize <= 0)
        {
            return 0;
        }

        var sectionData = VirtualAddressToData(image, ref ntHeaders, section.VirtualAddress, inputPath);
        var end = rawSize;
        while (end > 0 && (end & 3) != 0)
        {
            if (sectionData[end - 1] != 0)
            {
                break;
            }

            end--;
        }

        while (end > 4)
        {
            if (BinaryPrimitives.ReadUInt32LittleEndian(sectionData[(end - 4)..]) != 0)
            {
                break;
            }

            end -= 4;
        }

        return end;
    }
}

internal static class Pe32Reader
{
    public static ImageDataDirectory GetDataDirectory(ReadOnlySpan<byte> image, int directoryIndex)
    {
        var ntOffset = Pe32Helpers.GetNtHeaderOffset(image);
        var optionalOffset = ntOffset + Marshal.SizeOf<uint>() + Marshal.SizeOf<ImageFileHeader>();
        var directoryOffset = optionalOffset + 96 + directoryIndex * Marshal.SizeOf<ImageDataDirectory>();
        return new ImageDataDirectory
        {
            VirtualAddress = BinaryPrimitives.ReadUInt32LittleEndian(image[directoryOffset..]),
            Size = BinaryPrimitives.ReadUInt32LittleEndian(image[(directoryOffset + 4)..]),
        };
    }

    public static ref ImageOptionalHeader32 GetOptionalHeader(Span<byte> image)
    {
        var ntOffset = Pe32Helpers.GetNtHeaderOffset(image);
        var optionalOffset = ntOffset + Marshal.SizeOf<uint>() + Marshal.SizeOf<ImageFileHeader>();
        return ref MemoryMarshal.AsRef<ImageOptionalHeader32>(image[optionalOffset..]);
    }
}
