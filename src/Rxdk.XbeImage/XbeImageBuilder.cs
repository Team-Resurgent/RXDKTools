using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Text;
using Rxdk.XbeImage.Internal;

namespace Rxdk.XbeImage;

public sealed class XbeImageBuilder
{
    private const int TransferBufferSize = 128 * 1024;

    public void Build(ImageBldOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        if (string.IsNullOrWhiteSpace(options.InputFilePath))
        {
            throw new XbeImageException("Input PE path is required.");
        }

        if (string.IsNullOrWhiteSpace(options.OutputFilePath))
        {
            throw new XbeImageException("Output XBE path is required.");
        }

        Build(options.InputFilePath, options.OutputFilePath, options);
    }

    public void Build(string inputPe, string outputXbe, ImageBldOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        var context = new BuildContext(inputPe, outputXbe, options);
        try
        {
            context.OpenInputOutputFiles();
            context.BuildOutputFile();
        }
        catch
        {
            context.DeleteOutputOnFailure();
            throw;
        }
    }

    private sealed class BuildContext
    {
        private readonly ImageBldOptions _options;
        private readonly string _inputPath;
        private readonly string _outputPath;
        private byte[] _inputImage = Array.Empty<byte>();
        private string _inputFullPath = string.Empty;
        private string _debugSourcePath = string.Empty;
        private string _inputFilePart = string.Empty;
        private FileStream? _outputStream;
        private ImageNtHeaders32 _ntHeaders;
        private int _ntHeaderOffset;
        private readonly XbeBuildImageHeader _xbe = new();

        public BuildContext(string inputPe, string outputXbe, ImageBldOptions options)
        {
            _inputPath = inputPe;
            _outputPath = outputXbe;
            _options = options;
        }

        public void DeleteOutputOnFailure()
        {
            _outputStream?.Dispose();
            _outputStream = null;
            if (File.Exists(_outputPath))
            {
                File.Delete(_outputPath);
            }
        }

        public void OpenInputOutputFiles()
        {
            _inputFullPath = Path.GetFullPath(_inputPath);
            _debugSourcePath = string.IsNullOrWhiteSpace(_options.CanonicalDebugSourcePath)
                ? _inputFullPath
                : _options.CanonicalDebugSourcePath;
            _inputFilePart = Path.GetFileName(_inputFullPath);
            _inputImage = File.ReadAllBytes(_inputFullPath);
            _outputStream = new FileStream(_outputPath, FileMode.Create, FileAccess.ReadWrite, FileShare.None);
        }

        public void BuildOutputFile()
        {
            Pe32Helpers.ValidatePe32Image(_inputImage, _inputPath);
            _ntHeaderOffset = Pe32Helpers.GetNtHeaderOffset(_inputImage);
            _ntHeaders = Pe32Helpers.GetNtHeaders(_inputImage);

            InitializeXbeHeader();
            ProcessInputSectionHeaders();
            ProcessInputImportDescriptors();
            ProcessInputTlsDirectory();
            ProcessLibraryVersions();
            AddDebugPaths();
            AddMicrosoftLogoHeader();
            ProcessInsertFiles();
            FinalizeSectionHeaderVirtualSize();
            LayoutOutputHeaders();
            AddPeHeader();
            RelocateImageAfterHeaders();
            ConfoundHeaderData();
            EmitOutputFile();
            _outputStream!.Dispose();
            _outputStream = null;
        }

        private void InitializeXbeHeader()
        {
            _xbe.ImageHeader = new XbeImageHeader
            {
                Signature = XbeImageConstants.XbeImageSignature,
                BaseAddress = XbeImageConstants.StandardBaseAddress,
                TimeDateStamp = _options.FixedTimeDateStamp ?? (uint)DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
                SizeOfImageHeader = (uint)Marshal.SizeOf<XbeImageHeader>(),
                AddressOfEntryPoint = _ntHeaders.OptionalHeader.AddressOfEntryPoint,
                SizeOfStackCommit = _options.SizeOfStack != 0
                    ? _options.SizeOfStack
                    : _ntHeaders.OptionalHeader.SizeOfStackCommit,
                SizeOfHeapReserve = _ntHeaders.OptionalHeader.SizeOfHeapReserve,
                SizeOfHeapCommit = _ntHeaders.OptionalHeader.SizeOfHeapCommit,
                NtSizeOfImage = _ntHeaders.OptionalHeader.SizeOfImage,
                NtCheckSum = _ntHeaders.OptionalHeader.CheckSum,
                NtTimeDateStamp = _ntHeaders.FileHeader.TimeDateStamp,
                InitFlags = _options.InitFlags,
                EncryptedDigest = new byte[XbeImageConstants.EncryptedSignatureSize],
            };

            _xbe.CertificateHeader.VirtualSize = (uint)XbeImageCertificate.Size;
            _xbe.Headers.Add(_xbe.CertificateHeader);
        }

        private void ProcessInputSectionHeaders()
        {
            var sectionCount = Pe32Helpers.SectionCount(_inputImage);
            var sectionsRemaining = sectionCount;
            while (sectionsRemaining > 0)
            {
                var section = Pe32Helpers.ReadSectionHeader(_inputImage, sectionsRemaining - 1);
                if ((section.Characteristics & XbeImageConstants.ImageScnMemDiscardable) == 0)
                {
                    var firstSection = Pe32Helpers.ReadSectionHeader(_inputImage, 0);
                    _xbe.SizeOfExecutableImage = section.VirtualAddress + section.VirtualSize - firstSection.VirtualAddress;
                    var mask = _ntHeaders.OptionalHeader.SectionAlignment - 1;
                    _xbe.SizeOfExecutableImage = (_xbe.SizeOfExecutableImage + mask) & ~mask;
                    if (_xbe.SizeOfExecutableImage > XbeImageConstants.MaximumImageSize)
                    {
                        throw new XbeImageException("Image is too large.");
                    }

                    break;
                }

                sectionsRemaining--;
            }

            _xbe.ExecutableSectionCount = sectionsRemaining;

            uint lastVirtualAddress = 0;
            uint lastEndingVirtualAddress = 0;
            for (var i = 0; i < sectionsRemaining; i++)
            {
                var section = Pe32Helpers.ReadSectionHeader(_inputImage, i);
                if (section.VirtualAddress <= lastVirtualAddress)
                {
                    throw new XbeImageException($"Invalid or corrupt input file: {_inputPath}");
                }

                if (section.PointerToRawData > _inputImage.Length ||
                    section.PointerToRawData + section.SizeOfRawData > _inputImage.Length)
                {
                    throw new XbeImageException($"Invalid or corrupt input file: {_inputPath}");
                }

                if (section.VirtualSize == 0)
                {
                    throw new XbeImageException($"Invalid or corrupt input file: {_inputPath}");
                }

                var sectionName = Pe32Helpers.GetSectionName(section);
                _xbe.ImageHeader.NumberOfSections++;
                _xbe.SectionHeaders.NumberOfExecutableSections++;
                _xbe.SectionHeaders.SizeOfSectionNames += (uint)(Encoding.ASCII.GetByteCount(sectionName) + 1);

                if (SearchNoPreloadList(sectionName))
                {
                    section.Characteristics &= ~XbeImageConstants.ImageScnMemPreload;
                }
                else
                {
                    section.Characteristics |= XbeImageConstants.ImageScnMemPreload;
                }

                Pe32Helpers.WriteSectionHeader(_inputImage, i, section);

                var firstSectionVa = Pe32Helpers.ReadSectionHeader(_inputImage, 0).VirtualAddress;
                var relativeLoadAddress = section.VirtualAddress - firstSectionVa;
                if (relativeLoadAddress == 0 ||
                    Pe32Helpers.PageAlign((int)relativeLoadAddress) !=
                    Pe32Helpers.PageAlign((int)(lastEndingVirtualAddress - firstSectionVa)))
                {
                    _xbe.SectionHeaders.NumberOfSharedPageReferenceCounts++;
                }

                if (Pe32Helpers.PageAlign((int)relativeLoadAddress) !=
                    Pe32Helpers.PageAlign((int)(relativeLoadAddress + section.VirtualSize - 1)))
                {
                    _xbe.SectionHeaders.NumberOfSharedPageReferenceCounts++;
                }

                lastVirtualAddress = section.VirtualAddress;
                lastEndingVirtualAddress = lastVirtualAddress + section.VirtualSize - 1;
            }

            _xbe.Headers.Add(_xbe.SectionHeaders);
        }

        private void EnsureVirtualAddressIsPreload(uint virtualAddress)
        {
            var section = Pe32Helpers.VirtualAddressToSectionHeader(_inputImage, ref _ntHeaders, virtualAddress, _inputPath);
            if ((section.Characteristics & XbeImageConstants.ImageScnMemPreload) == 0)
            {
                Warn($"Section '{Pe32Helpers.GetSectionName(section)}' is not marked preload; forcing preload.");
                section.Characteristics |= XbeImageConstants.ImageScnMemPreload;
                for (var i = 0; i < Pe32Helpers.SectionCount(_inputImage); i++)
                {
                    var candidate = Pe32Helpers.ReadSectionHeader(_inputImage, i);
                    if (candidate.VirtualAddress == section.VirtualAddress)
                    {
                        Pe32Helpers.WriteSectionHeader(_inputImage, i, section);
                        break;
                    }
                }
            }
        }

        private void ProcessInputImportDescriptors()
        {
            var boundImport = Pe32Reader.GetDataDirectory(_inputImage, XbeImageConstants.ImageDirectoryEntryBoundImport);
            if (boundImport.Size != 0)
            {
                throw new XbeImageException("Bound import images are unsupported.");
            }

            var importDirectory = Pe32Reader.GetDataDirectory(_inputImage, XbeImageConstants.ImageDirectoryEntryImport);
            var importBytesRemaining = (int)importDirectory.Size;
            var importData = Pe32Helpers.ImageDataDirectoryToData(_inputImage, ref _ntHeaders, XbeImageConstants.ImageDirectoryEntryImport, _inputPath);
            var importOffset = 0;
            uint numberOfNonKernelImports = 0;
            uint sizeOfNonKernelImageNames = 0;

            while (importBytesRemaining >= Marshal.SizeOf<ImageImportDescriptor>())
            {
                ref var descriptor = ref MemoryMarshal.AsRef<ImageImportDescriptor>(importData[importOffset..]);
                if (descriptor.OriginalFirstThunk == 0)
                {
                    break;
                }

                var imageNameBytes = Pe32Helpers.VirtualAddressToData(_inputImage, ref _ntHeaders, descriptor.Name, _inputPath);
                var imageName = ReadAsciiNullTerminated(imageNameBytes);
                if (string.Equals(imageName, "xboxkrnl.exe", StringComparison.OrdinalIgnoreCase))
                {
                    _xbe.ImageHeader.XboxKernelThunkData = descriptor.FirstThunk;
                }
                else
                {
                    numberOfNonKernelImports++;
                    sizeOfNonKernelImageNames += (uint)((imageName.Length + 1) * sizeof(char));
                }

                var originalThunk = Pe32Helpers.VirtualAddressToData(_inputImage, ref _ntHeaders, descriptor.OriginalFirstThunk, _inputPath);
                var imageThunk = Pe32Helpers.VirtualAddressToData(_inputImage, ref _ntHeaders, descriptor.FirstThunk, _inputPath);
                var thunkOffset = 0;
                while (true)
                {
                    var ordinal = BinaryPrimitives.ReadUInt32LittleEndian(imageThunk[thunkOffset..]);
                    if (ordinal == 0)
                    {
                        break;
                    }

                    var originalOrdinal = BinaryPrimitives.ReadUInt32LittleEndian(originalThunk[thunkOffset..]);
                    if (originalOrdinal != ordinal)
                    {
                        throw new XbeImageException($"Invalid or corrupt input file: {_inputPath}");
                    }

                    if (!Pe32Helpers.SnapByOrdinal(ordinal))
                    {
                        throw new XbeImageException($"Import by name is not supported for '{imageName}'.");
                    }

                    thunkOffset += 4;
                }

                EnsureVirtualAddressIsPreload(descriptor.FirstThunk);
                importOffset += Marshal.SizeOf<ImageImportDescriptor>();
                importBytesRemaining -= Marshal.SizeOf<ImageImportDescriptor>();
            }

            if (numberOfNonKernelImports != 0)
            {
                _xbe.ImportDescriptorHeader.NumberOfNonKernelImports = numberOfNonKernelImports;
                _xbe.ImportDescriptorHeader.VirtualSize =
                    (numberOfNonKernelImports + 1) * (uint)Marshal.SizeOf<XbeImageImportDescriptor>() +
                    sizeOfNonKernelImageNames;
                _xbe.Headers.Add(_xbe.ImportDescriptorHeader);
            }
        }

        private void ProcessInputTlsDirectory()
        {
            var tlsDirectoryEntry = Pe32Reader.GetDataDirectory(_inputImage, XbeImageConstants.ImageDirectoryEntryTls);
            if (tlsDirectoryEntry.Size == 0)
            {
                return;
            }

            // Resolve the RVA of the real IMAGE_TLS_DIRECTORY. Normally the PE TLS
            // data directory points straight at it (_tls_used), but some linkers --
            // notably lld-link -- aim IMAGE_DIRECTORY_ENTRY_TLS at compiler metadata
            // instead. If the structure at the data-directory RVA is not a valid TLS
            // directory whose template lives in .tls, scan the read-only data for the
            // real one (same heuristic used by ScanForAdditionalTlsDirectories).
            var tlsDirectoryRva = tlsDirectoryEntry.VirtualAddress;
            if (!IsTlsDirectory(tlsDirectoryRva) && TryFindTlsDirectoryRva(out var resolvedRva))
            {
                tlsDirectoryRva = resolvedRva;
            }

            var directorySection = Pe32Helpers.VirtualAddressToSectionHeader(_inputImage, ref _ntHeaders, tlsDirectoryRva, _inputPath);
            _xbe.TlsRawDataHeader.TlsDirectoryOffset = (int)(directorySection.PointerToRawData + (tlsDirectoryRva - directorySection.VirtualAddress));
            _xbe.ImageHeader.TlsDirectory = tlsDirectoryRva;
            EnsureVirtualAddressIsPreload(tlsDirectoryRva);

            var tlsBytes = Pe32Helpers.VirtualAddressToData(_inputImage, ref _ntHeaders, tlsDirectoryRva, _inputPath);
            ref var tlsDirectory = ref MemoryMarshal.AsRef<ImageTlsDirectory32>(tlsBytes);
            if (tlsDirectory.EndAddressOfRawData - tlsDirectory.StartAddressOfRawData != 0)
            {
                var rawData = Pe32Helpers.LoadAddressToData(_inputImage, ref _ntHeaders, tlsDirectory.StartAddressOfRawData, _inputPath);
                var endExclusive = (int)(tlsDirectory.EndAddressOfRawData - tlsDirectory.StartAddressOfRawData);
                while (endExclusive > 0 && rawData[endExclusive - 1] == 0)
                {
                    tlsDirectory.SizeOfZeroFill++;
                    endExclusive--;
                }

                // Shrink EndAddressOfRawData to the bytes we actually keep. The loop
                // above folded trailing zero bytes into SizeOfZeroFill; without this
                // the directory still advertises the full raw-template size even
                // though no raw-data section backs the trimmed tail. The kernel's
                // per-thread TLS setup (XapiThreadStartup) then RtlCopyMemory's the
                // advertised End-Start bytes from the template pointer into each
                // thread's TLS block, reading past the real template into adjacent
                // or unmapped memory -- harmless on xemu, a fault on real hardware.
                // For an all-zero template this collapses to End == Start (raw size
                // 0) plus pure zero-fill, matching the XDK linker's TLS directory.
                tlsDirectory.EndAddressOfRawData = tlsDirectory.StartAddressOfRawData + (uint)endExclusive;

                if (endExclusive > 0)
                {
                    _xbe.TlsRawDataHeader.VirtualSize = (uint)endExclusive;
                    _xbe.TlsRawDataHeader.RawData = rawData[..endExclusive].ToArray();
                    _xbe.Headers.Add(_xbe.TlsRawDataHeader);
                }
            }
        }

        // True if the structure at tlsDirectoryRva is a valid IMAGE_TLS_DIRECTORY
        // whose raw-data template lives in the .tls section.
        private bool IsTlsDirectory(uint tlsDirectoryRva)
        {
            if (tlsDirectoryRva == 0)
            {
                return false;
            }

            ImageSectionHeader section;
            try
            {
                section = Pe32Helpers.VirtualAddressToSectionHeader(_inputImage, ref _ntHeaders, tlsDirectoryRva, _inputPath);
            }
            catch (XbeImageException)
            {
                return false;
            }

            var fileOffset = (int)(section.PointerToRawData + (tlsDirectoryRva - section.VirtualAddress));
            if (fileOffset < 0 || fileOffset + Marshal.SizeOf<ImageTlsDirectory32>() > _inputImage.Length)
            {
                return false;
            }

            ref var candidate = ref MemoryMarshal.AsRef<ImageTlsDirectory32>(_inputImage.AsSpan(fileOffset));
            return LooksLikeTlsDirectory(candidate, _ntHeaders.OptionalHeader.ImageBase) &&
                   TlsTemplateInTlsSection(candidate.StartAddressOfRawData);
        }

        // Scan .rdata for a structure that looks like the real IMAGE_TLS_DIRECTORY
        // (where _tls_used lives) and whose template points into .tls.
        private bool TryFindTlsDirectoryRva(out uint tlsDirectoryRva)
        {
            tlsDirectoryRva = 0;
            var imageBase = _ntHeaders.OptionalHeader.ImageBase;
            var structSize = Marshal.SizeOf<ImageTlsDirectory32>();
            var count = _ntHeaders.FileHeader.NumberOfSections;
            for (var i = 0; i < count; i++)
            {
                var section = Pe32Helpers.ReadSectionHeader(_inputImage, i);
                if (section.SizeOfRawData == 0 ||
                    !Pe32Helpers.GetSectionName(section).StartsWith(".rdata", StringComparison.Ordinal))
                {
                    continue;
                }

                var baseOffset = (int)section.PointerToRawData;
                var limit = (int)Math.Min(section.SizeOfRawData, (uint)Math.Max(0, _inputImage.Length - baseOffset));
                for (var offset = 0; offset + structSize <= limit; offset += 4)
                {
                    ref var candidate = ref MemoryMarshal.AsRef<ImageTlsDirectory32>(_inputImage.AsSpan(baseOffset + offset));
                    if (LooksLikeTlsDirectory(candidate, imageBase) &&
                        TlsTemplateInTlsSection(candidate.StartAddressOfRawData))
                    {
                        tlsDirectoryRva = section.VirtualAddress + (uint)offset;
                        return true;
                    }
                }
            }

            return false;
        }

        // True if a TLS template raw-data load address points into the .tls section
        // of the input image (full VA = ImageBase + section RVA).
        private bool TlsTemplateInTlsSection(uint startAddress)
        {
            if (Pe32Helpers.NameToSectionHeader(_inputImage, ref _ntHeaders, ".tls", out _) is not { } tlsSection)
            {
                return false;
            }

            var low = _ntHeaders.OptionalHeader.ImageBase + tlsSection.VirtualAddress;
            var high = low + Math.Max(tlsSection.VirtualSize, tlsSection.SizeOfRawData);
            return startAddress >= low && startAddress < high;
        }

        private void ProcessLibraryVersions()
        {
            _xbe.LibraryVersionHeader.XboxKernelOffset = uint.MaxValue;
            _xbe.LibraryVersionHeader.XapiOffset = uint.MaxValue;
            XbeImageLibraryVersion? xapiVersion = null;

            if (Pe32Helpers.NameToSectionHeader(_inputImage, ref _ntHeaders, ".xbld", out _) is { } sectionHeader)
            {
                var sourceBytes = Pe32Helpers.VirtualAddressToData(_inputImage, ref _ntHeaders, sectionHeader.VirtualAddress, _inputPath);
                var libraryVersions = new List<XbeImageLibraryVersion>();
                var offset = 0;
                while (offset + XbeImageLibraryVersion.Size <= sectionHeader.VirtualSize)
                {
                    if (offset + 4 <= sourceBytes.Length &&
                        BinaryPrimitives.ReadUInt32LittleEndian(sourceBytes[offset..]) == 0)
                    {
                        offset += 4;
                        continue;
                    }

                    if (offset + XbeImageLibraryVersion.Size > sourceBytes.Length)
                    {
                        break;
                    }

                    var version = ReadLibraryVersion(sourceBytes, offset);
                    libraryVersions.Add(version);
                    offset += XbeImageLibraryVersion.Size;

                    if (MatchesLibraryName(version.LibraryName, "XBOXKRNL"))
                    {
                        _xbe.LibraryVersionHeader.XboxKernelOffset =
                            (uint)((libraryVersions.Count - 1) * XbeImageLibraryVersion.Size);
                    }

                    if (MatchesLibraryName(version.LibraryName, "XAPILIB") ||
                        MatchesLibraryName(version.LibraryName, "XAPILIBD"))
                    {
                        _xbe.LibraryVersionHeader.XapiOffset =
                            (uint)((libraryVersions.Count - 1) * XbeImageLibraryVersion.Size);
                        xapiVersion = version;
                    }
                }

                _xbe.ImageHeader.NumberOfLibraryVersions = (uint)libraryVersions.Count;
                _xbe.LibraryVersionHeader.VirtualSize = (uint)(libraryVersions.Count * XbeImageLibraryVersion.Size);
                _xbe.Headers.Add(_xbe.LibraryVersionHeader);

                var approvedStatus = LibraryVersionChecker.CheckLibraryApprovalStatus(
                    xapiVersion,
                    libraryVersions.ToArray(),
                    (lib, level) => ApplyLibraryApproval(libraryVersions, lib, level));

                _xbe.LibraryVersionHeader.LibraryVersions = libraryVersions.ToArray();

                if (_xbe.LibraryVersionHeader.XboxKernelOffset == uint.MaxValue)
                {
                    Warn("No library version detected for XBOXKRNL.");
                }

                if (_xbe.LibraryVersionHeader.XapiOffset == uint.MaxValue)
                {
                    Warn("No library version detected for XAPI.");
                }

                if (!_options.NoWarnLibraryApproval && _xbe.ImportDescriptorHeader.VirtualSize != 0)
                {
                    Warn("Unapproved library: <debug extension import>");
                    approvedStatus = 0;
                }

                if (approvedStatus < 2 && !_options.NoWarnLibraryApproval)
                {
                    Warn("One or more linked libraries are not fully approved.");
                }
            }
            else
            {
                Warn("No library version detected for XBOXKRNL.");
                Warn("No library version detected for XAPI.");
            }
        }

        private void AddDebugPaths()
        {
            _xbe.DebugPathsHeader.VirtualSize =
                (uint)((_debugSourcePath.Length + 1) * sizeof(byte) + (_inputFilePart.Length + 1) * sizeof(char));
            _xbe.Headers.Add(_xbe.DebugPathsHeader);
        }

        private void AddMicrosoftLogoHeader()
        {
            _xbe.MicrosoftLogoHeader.VirtualSize = (uint)MicrosoftLogo.LogoBytes.Length;
            _xbe.Headers.Add(_xbe.MicrosoftLogoHeader);
        }

        private void ProcessInsertFiles()
        {
            if (_options.InsertFiles.Count == 0)
            {
                return;
            }

            foreach (var insertFile in _options.InsertFiles)
            {
                CheckDuplicateInsertFileSection(insertFile);
            }

            var relativeLoadAddress = _xbe.SizeOfExecutableImage;
            foreach (var insertFile in _options.InsertFiles)
            {
                var noPreload = insertFile.NoPreload || SearchNoPreloadList(insertFile.SectionName);

                var fileInfo = new FileInfo(insertFile.FilePath);
                if (!fileInfo.Exists)
                {
                    throw new XbeImageException($"Cannot open input file: {insertFile.FilePath}");
                }

                var runtimeInsert = new RuntimeInsertFile(insertFile, (uint)fileInfo.Length, noPreload);
                var alignedFileSize = (runtimeInsert.FileSize + XbeImageConstants.InsertFileSectionAlignment - 1) &
                                      ~(XbeImageConstants.InsertFileSectionAlignment - 1u);
                if ((ulong)relativeLoadAddress + alignedFileSize > XbeImageConstants.MaximumImageSize)
                {
                    throw new XbeImageException("Image is too large.");
                }

                _xbe.ImageHeader.NumberOfSections++;
                _xbe.SectionHeaders.NumberOfInsertFileSections++;
                _xbe.SectionHeaders.SizeOfSectionNames += (uint)(Encoding.ASCII.GetByteCount(insertFile.SectionName) + 1);

                if (Pe32Helpers.PageAlign((int)relativeLoadAddress) !=
                    Pe32Helpers.PageAlign((int)(relativeLoadAddress - 1)))
                {
                    _xbe.SectionHeaders.NumberOfSharedPageReferenceCounts++;
                }

                if (Pe32Helpers.PageAlign((int)relativeLoadAddress) !=
                    Pe32Helpers.PageAlign((int)(relativeLoadAddress + alignedFileSize - 1)))
                {
                    _xbe.SectionHeaders.NumberOfSharedPageReferenceCounts++;
                }

                relativeLoadAddress += alignedFileSize;
                _xbe.InsertFiles.Add(runtimeInsert);
            }

            _xbe.SizeOfInsertFilesImage = relativeLoadAddress - _xbe.SizeOfExecutableImage;
        }

        private void FinalizeSectionHeaderVirtualSize()
        {
            _xbe.SectionHeaders.VirtualSize =
                _xbe.ImageHeader.NumberOfSections * (uint)Marshal.SizeOf<XbeImageSection>() +
                _xbe.SectionHeaders.NumberOfSharedPageReferenceCounts * sizeof(ushort) +
                _xbe.SectionHeaders.SizeOfSectionNames;
        }

        private void LayoutOutputHeaders()
        {
            var sizeOfHeaders = Marshal.SizeOf<XbeImageHeader>();
            foreach (var header in _xbe.Headers)
            {
                if (header.VirtualSize == 0)
                {
                    header.VirtualAddress = 0;
                    continue;
                }

                sizeOfHeaders = (sizeOfHeaders + sizeof(uint) - 1) & ~(sizeof(uint) - 1);
                header.VirtualAddress = XbeImageConstants.StandardBaseAddress + (uint)sizeOfHeaders;
                sizeOfHeaders += (int)header.VirtualSize;
            }

            sizeOfHeaders = (sizeOfHeaders + sizeof(uint) - 1) & ~(sizeof(uint) - 1);
            _xbe.ImageHeader.SizeOfHeaders = (uint)sizeOfHeaders;
            _xbe.ImageHeader.SizeOfImage = (uint)(Pe32Helpers.RoundToPages(sizeOfHeaders) +
                                                   _xbe.SizeOfExecutableImage +
                                                   _xbe.SizeOfInsertFilesImage);

            if (_xbe.ImageHeader.SizeOfImage > XbeImageConstants.MaximumImageSize)
            {
                throw new XbeImageException("Image is too large.");
            }

            if (_xbe.CertificateHeader.VirtualSize != 0)
            {
                _xbe.ImageHeader.Certificate = _xbe.CertificateHeader.VirtualAddress;
            }

            if (_xbe.SectionHeaders.VirtualSize != 0)
            {
                _xbe.ImageHeader.SectionHeaders = _xbe.SectionHeaders.VirtualAddress;
            }

            if (_xbe.ImportDescriptorHeader.VirtualSize != 0)
            {
                _xbe.ImageHeader.ImportDirectory = _xbe.ImportDescriptorHeader.VirtualAddress;
            }

            if (_xbe.LibraryVersionHeader.VirtualSize != 0)
            {
                _xbe.ImageHeader.LibraryVersions = _xbe.LibraryVersionHeader.VirtualAddress;
                if (_xbe.LibraryVersionHeader.XboxKernelOffset != uint.MaxValue)
                {
                    _xbe.ImageHeader.XboxKernelLibraryVersion =
                        _xbe.LibraryVersionHeader.VirtualAddress + _xbe.LibraryVersionHeader.XboxKernelOffset;
                }

                if (_xbe.LibraryVersionHeader.XapiOffset != uint.MaxValue)
                {
                    _xbe.ImageHeader.XapiLibraryVersion =
                        _xbe.LibraryVersionHeader.VirtualAddress + _xbe.LibraryVersionHeader.XapiOffset;
                }
            }

            if (_xbe.DebugPathsHeader.VirtualSize != 0)
            {
                _xbe.ImageHeader.DebugUnicodeFileName = _xbe.DebugPathsHeader.VirtualAddress;
                _xbe.ImageHeader.DebugPathName = _xbe.DebugPathsHeader.VirtualAddress +
                                                 (uint)((_inputFilePart.Length + 1) * sizeof(char));
                _xbe.ImageHeader.DebugFileName = _xbe.ImageHeader.DebugPathName +
                                                 (uint)(_debugSourcePath.Length - _inputFilePart.Length);
            }

            if (_xbe.MicrosoftLogoHeader.VirtualSize != 0)
            {
                _xbe.ImageHeader.MicrosoftLogo = _xbe.MicrosoftLogoHeader.VirtualAddress;
                _xbe.ImageHeader.SizeOfMicrosoftLogo = _xbe.MicrosoftLogoHeader.VirtualSize;
            }
        }

        private void AddPeHeader()
        {
            var firstSection = Pe32Helpers.ReadSectionHeader(_inputImage, 0);
            var sizeOfNtHeaders = firstSection.VirtualAddress;
            var adjustedSizeOfHeaders = Pe32Helpers.RoundToPages((int)_xbe.ImageHeader.SizeOfHeaders);
            if (_options.EmitPeHeader)
            {
                adjustedSizeOfHeaders = Pe32Helpers.RoundToPages((int)_xbe.ImageHeader.SizeOfHeaders + (int)sizeOfNtHeaders);
                _xbe.ImageHeader.SizeOfHeaders = (uint)adjustedSizeOfHeaders;
                _xbe.PeHeaderHeader.VirtualSize = sizeOfNtHeaders;
                _xbe.Headers.Add(_xbe.PeHeaderHeader);
                _xbe.ImageHeader.SizeOfImage = (uint)(Pe32Helpers.RoundToPages(adjustedSizeOfHeaders) +
                                                       _xbe.SizeOfExecutableImage +
                                                       _xbe.SizeOfInsertFilesImage);
                if (_xbe.ImageHeader.SizeOfImage > XbeImageConstants.MaximumImageSize)
                {
                    throw new XbeImageException("Image is too large.");
                }
            }

            _xbe.PeHeaderHeader.VirtualAddress = XbeImageConstants.StandardBaseAddress +
                                                 (uint)adjustedSizeOfHeaders - sizeOfNtHeaders;
            _xbe.ImageHeader.NtBaseOfDll = _xbe.PeHeaderHeader.VirtualAddress;
        }

        private void RelocateImageAfterHeaders()
        {
            var sizeOfHeadersPageAligned = Pe32Helpers.RoundToPages((int)_xbe.ImageHeader.SizeOfHeaders);
            var firstSectionVa = Pe32Helpers.ReadSectionHeader(_inputImage, 0).VirtualAddress;
            var newBaseAddress = XbeImageConstants.StandardBaseAddress + (uint)sizeOfHeadersPageAligned - firstSectionVa;

            var relocDirectory = Pe32Reader.GetDataDirectory(_inputImage, XbeImageConstants.ImageDirectoryEntryBaseReloc);
            if (relocDirectory.Size == 0)
            {
                throw new XbeImageException("Missing relocation records.");
            }

            Pe32Helpers.RelocateImage(
                _inputImage,
                ref _ntHeaders,
                _ntHeaders.OptionalHeader.ImageBase,
                newBaseAddress,
                _inputPath);

            if (_xbe.ImageHeader.AddressOfEntryPoint != 0)
            {
                _xbe.ImageHeader.AddressOfEntryPoint += newBaseAddress;
            }

            if (_xbe.ImageHeader.XboxKernelThunkData != 0)
            {
                _xbe.ImageHeader.XboxKernelThunkData += newBaseAddress;
            }

            if (_xbe.ImageHeader.TlsDirectory != 0)
            {
                _xbe.ImageHeader.TlsDirectory += newBaseAddress;
                FixupAllTlsDirectories(newBaseAddress);
            }

            _xbe.NewBaseAddress = newBaseAddress;
        }

        private void FixupAllTlsDirectories(uint firstSectionBaseAddress)
        {
            if (_xbe.TlsRawDataHeader.TlsDirectoryOffset > 0)
            {
                ref var primary = ref MemoryMarshal.AsRef<ImageTlsDirectory32>(
                    _inputImage.AsSpan(_xbe.TlsRawDataHeader.TlsDirectoryOffset));
                FixupTlsDirectory(ref primary, firstSectionBaseAddress);
            }

            ScanForAdditionalTlsDirectories(firstSectionBaseAddress);
        }

        private void FixupTlsDirectory(ref ImageTlsDirectory32 tlsDirectory, uint firstSectionBaseAddress)
        {
            var rawSize = tlsDirectory.EndAddressOfRawData > tlsDirectory.StartAddressOfRawData
                ? tlsDirectory.EndAddressOfRawData - tlsDirectory.StartAddressOfRawData
                : 0u;

            // Match classic imagebld: raw template pointers always come from the header blob.
            tlsDirectory.StartAddressOfRawData = _xbe.TlsRawDataHeader.VirtualAddress;
            tlsDirectory.EndAddressOfRawData =
                _xbe.TlsRawDataHeader.VirtualAddress + _xbe.TlsRawDataHeader.VirtualSize;

            if (_xbe.TlsRawDataHeader.VirtualSize != 0)
            {
                return;
            }

            if (Pe32Helpers.NameToSectionHeader(_inputImage, ref _ntHeaders, ".tls", out var tlsSectionIndex) is not
                { } tlsSection ||
                tlsSectionIndex >= _xbe.ExecutableSectionCount)
            {
                return;
            }

            if (rawSize == 0 && tlsDirectory.SizeOfZeroFill != 0)
            {
                rawSize = tlsDirectory.SizeOfZeroFill;
            }

            if (rawSize == 0)
            {
                rawSize = Math.Max(tlsSection.VirtualSize, tlsSection.SizeOfRawData);
            }

            var tlsVa = tlsSection.VirtualAddress + firstSectionBaseAddress;
            tlsDirectory.StartAddressOfRawData = tlsVa;
            tlsDirectory.EndAddressOfRawData = tlsVa + rawSize;
        }

        private void ScanForAdditionalTlsDirectories(uint firstSectionBaseAddress)
        {
            if (_xbe.TlsRawDataHeader.TlsDirectoryOffset <= 0)
            {
                return;
            }

            var imageBase = _ntHeaders.OptionalHeader.ImageBase;
            ImageSectionHeader? tlsSection = null;
            int tlsSectionIndex = -1;
            if (Pe32Helpers.NameToSectionHeader(_inputImage, ref _ntHeaders, ".tls", out tlsSectionIndex) is { } tlsPeSection)
            {
                tlsSection = tlsPeSection;
            }

            for (var i = 0; i < _xbe.ExecutableSectionCount; i++)
            {
                var section = Pe32Helpers.ReadSectionHeader(_inputImage, i);
                if (section.SizeOfRawData == 0)
                {
                    continue;
                }

                var sectionFileOffset = (int)section.PointerToRawData;
                for (var offset = 0; offset + Marshal.SizeOf<ImageTlsDirectory32>() <= section.SizeOfRawData; offset += 4)
                {
                    var fileOffset = sectionFileOffset + offset;
                    if (fileOffset == _xbe.TlsRawDataHeader.TlsDirectoryOffset)
                    {
                        continue;
                    }

                    ref var candidate = ref MemoryMarshal.AsRef<ImageTlsDirectory32>(_inputImage.AsSpan(fileOffset));
                    if (!LooksLikeTlsDirectory(candidate, imageBase))
                    {
                        continue;
                    }

                    if (tlsSection is not null &&
                        !LooksLikeTlsTemplatePointer(
                            candidate.StartAddressOfRawData,
                            tlsSection.Value,
                            firstSectionBaseAddress,
                            imageBase))
                    {
                        continue;
                    }

                    FixupTlsDirectory(ref candidate, firstSectionBaseAddress);
                }
            }
        }

        private bool LooksLikeTlsTemplatePointer(
            uint startAddress,
            in ImageSectionHeader tlsSection,
            uint firstSectionBaseAddress,
            uint imageBase)
        {
            var emittedTlsVa = tlsSection.VirtualAddress + firstSectionBaseAddress;
            var emittedTlsEnd = emittedTlsVa + Math.Max(tlsSection.VirtualSize, tlsSection.SizeOfRawData);
            var peTlsVa = imageBase + tlsSection.VirtualAddress;
            var peTlsEnd = peTlsVa + Math.Max(tlsSection.VirtualSize, tlsSection.SizeOfRawData);
            return (startAddress >= emittedTlsVa && startAddress < emittedTlsEnd) ||
                   (startAddress >= peTlsVa && startAddress < peTlsEnd);
        }

        private static bool LooksLikeTlsDirectory(in ImageTlsDirectory32 candidate, uint imageBase)
        {
            if (candidate.AddressOfCallbacks != 0)
            {
                return false;
            }

            if (candidate.EndAddressOfRawData <= candidate.StartAddressOfRawData)
            {
                return false;
            }

            if (candidate.EndAddressOfRawData - candidate.StartAddressOfRawData > 0x10000)
            {
                return false;
            }

            if (candidate.AddressOfIndex < imageBase)
            {
                return false;
            }

            return candidate.Characteristics is 0 or 0x300000;
        }

        private void ConfoundHeaderData()
        {
            var key = MemoryMarshal.Cast<byte, uint>(XbeImageKeys.PublicKeyData.AsSpan(128));
            _xbe.ImageHeader.AddressOfEntryPoint ^= key[0] ^ key[4];
            _xbe.ImageHeader.XboxKernelThunkData ^= key[1] ^ key[2];
        }

        private void EmitOutputFile()
        {
            WriteZeroPadding((int)_xbe.ImageHeader.SizeOfHeaders);
            foreach (var header in _xbe.Headers)
            {
                if (header.VirtualSize != 0)
                {
                    SeekOutputFile((int)header.VirtualAddress - (int)XbeImageConstants.StandardBaseAddress);
                    header.Write(this);
                }
            }

            SeekOutputFile(0);
            WriteImageHeaderWithoutSignature();
            SignImageHeaders();
        }

        private void SignImageHeaders()
        {
            SeekOutputFile(0);
            var headerBytes = new byte[_xbe.ImageHeader.SizeOfHeaders];
            ReadOutputFile(headerBytes);
            Span<byte> encryptedDigest = stackalloc byte[XbeImageConstants.EncryptedSignatureSize];
            XbeImageCrypto.SignImageHeaders(
                headerBytes,
                XbeImageHeader.BaseAddressFieldOffset,
                XbeImageKeys.PrivateKeyData,
                encryptedDigest);
            encryptedDigest.CopyTo(_xbe.ImageHeader.EncryptedDigest);
            SeekOutputFile(0);
            WriteImageHeaderWithoutSignature();
        }

        private void WriteImageHeaderWithoutSignature()
        {
            var buffer = new byte[Marshal.SizeOf<XbeImageHeader>()];
            BinaryStructWriter.WriteXbeImageHeader(buffer, _xbe.ImageHeader, includeEncryptedDigest: true);
            WriteOutputFile(buffer);
        }

        internal void WriteCertificateHeader()
        {
            var certificate = new XbeImageCertificate
            {
                SizeOfCertificate = (uint)XbeImageCertificate.Size,
                OriginalSizeOfCertificate = (uint)XbeImageCertificate.Size,
                TimeDateStamp = _xbe.ImageHeader.TimeDateStamp,
                TitleId = _options.TestTitleId,
                TitleName = ToFixedTitleName(_options.TestTitleName),
                AlternateTitleIds = BuildAlternateTitleIds(),
                AllowedMediaTypes = _options.TestAllowedMediaTypes,
                GameRegion = _options.TestGameRegion,
                GameRatings = _options.TestGameRatings,
                Version = _options.Version,
                LanKey = (byte[])_options.TestLanKey.Clone(),
                SignatureKey = (byte[])_options.TestSignatureKey.Clone(),
            };

            var flatKeys = new byte[XbeImageConstants.AlternateTitleIdCount * XbeImageConstants.CertificateKeyLength];
            for (var i = 0; i < XbeImageConstants.AlternateTitleIdCount; i++)
            {
                var key = i < _options.TestAlternateTitleIds.Count &&
                          _options.TestAlternateTitleIds[i].SignatureKey is { Length: > 0 }
                    ? _options.TestAlternateTitleIds[i].SignatureKey!
                    : new byte[XbeImageConstants.CertificateKeyLength];
                key.AsSpan(0, Math.Min(key.Length, XbeImageConstants.CertificateKeyLength))
                    .CopyTo(flatKeys.AsSpan(i * XbeImageConstants.CertificateKeyLength));
            }

            certificate.AlternateSignatureKeysFlat = flatKeys;
            WriteOutputFile(BinaryStructWriter.GetCertificateBytes(certificate));
        }

        internal void WriteImportDescriptorHeader()
        {
            var importDirectoryByteOffset = (int)_xbe.ImportDescriptorHeader.VirtualAddress - (int)XbeImageConstants.StandardBaseAddress;
            var importDirectoryNameByteOffset = importDirectoryByteOffset +
                (int)((_xbe.ImportDescriptorHeader.NumberOfNonKernelImports + 1) *
                      Marshal.SizeOf<XbeImageImportDescriptor>());

            var importDirectory = Pe32Reader.GetDataDirectory(_inputImage, XbeImageConstants.ImageDirectoryEntryImport);
            var importBytesRemaining = (int)importDirectory.Size;
            var importData = Pe32Helpers.ImageDataDirectoryToData(_inputImage, ref _ntHeaders, XbeImageConstants.ImageDirectoryEntryImport, _inputPath);
            var importOffset = 0;
            while (importBytesRemaining >= Marshal.SizeOf<ImageImportDescriptor>())
            {
                ref var descriptor = ref MemoryMarshal.AsRef<ImageImportDescriptor>(importData[importOffset..]);
                if (descriptor.OriginalFirstThunk == 0)
                {
                    break;
                }

                var imageName = ReadAsciiNullTerminated(
                    Pe32Helpers.VirtualAddressToData(_inputImage, ref _ntHeaders, descriptor.Name, _inputPath));
                if (!string.Equals(imageName, "xboxkrnl.exe", StringComparison.OrdinalIgnoreCase))
                {
                    var unicodeName = Encoding.Unicode.GetBytes(imageName + "\0");
                    SeekOutputFile(importDirectoryNameByteOffset);
                    WriteOutputFile(unicodeName);

                    var xbeDescriptor = new XbeImageImportDescriptor
                    {
                        ImageThunkData = descriptor.FirstThunk + _xbe.NewBaseAddress,
                        ImageName = (uint)(importDirectoryNameByteOffset + XbeImageConstants.StandardBaseAddress),
                    };
                    SeekOutputFile(importDirectoryByteOffset);
                    WriteOutputFile(BinaryStructWriter.GetImportDescriptorBytes(xbeDescriptor));

                    importDirectoryByteOffset += Marshal.SizeOf<XbeImageImportDescriptor>();
                    importDirectoryNameByteOffset += (imageName.Length + 1) * sizeof(char);
                }

                importOffset += Marshal.SizeOf<ImageImportDescriptor>();
                importBytesRemaining -= Marshal.SizeOf<ImageImportDescriptor>();
            }

            var terminator = new XbeImageImportDescriptor();
            SeekOutputFile(importDirectoryByteOffset);
            WriteOutputFile(BinaryStructWriter.GetImportDescriptorBytes(terminator));
        }

        internal void WriteTlsRawDataHeader() =>
            WriteOutputFile(_xbe.TlsRawDataHeader.RawData);

        internal void WriteLibraryVersionHeader()
        {
            foreach (var version in _xbe.LibraryVersionHeader.LibraryVersions)
            {
                var buffer = new byte[XbeImageLibraryVersion.Size];
                version.LibraryName.AsSpan(0, XbeImageConstants.LibraryVersionNameLength).CopyTo(buffer);
                BinaryPrimitives.WriteUInt16LittleEndian(buffer.AsSpan(8), version.MajorVersion);
                BinaryPrimitives.WriteUInt16LittleEndian(buffer.AsSpan(10), version.MinorVersion);
                BinaryPrimitives.WriteUInt16LittleEndian(buffer.AsSpan(12), version.BuildVersion);
                BinaryPrimitives.WriteUInt16LittleEndian(buffer.AsSpan(14), version.VersionFlags);
                WriteOutputFile(buffer);
            }
        }

        internal void WriteDebugPathsHeader()
        {
            var fileNameUnicode = new char[_inputFilePart.Length + 1];
            for (var i = 0; i < _inputFilePart.Length; i++)
            {
                fileNameUnicode[i] = _inputFilePart[i];
            }

            WriteOutputFile(MemoryMarshal.AsBytes(fileNameUnicode.AsSpan()));
            WriteOutputFile(Encoding.ASCII.GetBytes(_debugSourcePath + "\0"));
        }

        internal void WriteMicrosoftLogoHeader() =>
            WriteOutputFile(MicrosoftLogo.LogoBytes);

        internal void WritePeHeaderHeader()
        {
            var peHeaderBytes = new byte[_xbe.PeHeaderHeader.VirtualSize];
            Array.Copy(_inputImage, 0, peHeaderBytes, 0, peHeaderBytes.Length);
            WriteOutputFile(peHeaderBytes);
        }

        internal void WriteSectionHeaders()
        {
            var executableSectionCount = (int)_xbe.SectionHeaders.NumberOfExecutableSections;
            var sectionHeaderByteOffset = (int)_xbe.SectionHeaders.VirtualAddress - (int)XbeImageConstants.StandardBaseAddress;
            var sectionHeaderNameByteOffset = sectionHeaderByteOffset +
                                              (int)_xbe.ImageHeader.NumberOfSections * Marshal.SizeOf<XbeImageSection>() +
                                              (int)_xbe.SectionHeaders.NumberOfSharedPageReferenceCounts * sizeof(ushort);

            var firstSection = Pe32Helpers.ReadSectionHeader(_inputImage, 0);
            var firstSectionBaseAddress = XbeImageConstants.StandardBaseAddress +
                                          (uint)Pe32Helpers.RoundToPages((int)_xbe.ImageHeader.SizeOfHeaders) -
                                          firstSection.VirtualAddress;
            var imageFileByteOffsets = new int[executableSectionCount];
            var fileByteOffset = Pe32Helpers.RoundToPages((int)_xbe.ImageHeader.SizeOfHeaders);

            for (var i = 0; i < executableSectionCount; i++)
            {
                var section = Pe32Helpers.ReadSectionHeader(_inputImage, i);
                section.SizeOfRawData = (uint)Pe32Helpers.TrimSectionRawDataSize(_inputImage, ref _ntHeaders, section, _inputPath);
                Pe32Helpers.WriteSectionHeader(_inputImage, i, section);
                if ((section.Characteristics & XbeImageConstants.ImageScnMemPreload) != 0)
                {
                    imageFileByteOffsets[i] = fileByteOffset;
                    fileByteOffset += Pe32Helpers.RoundToPages((int)section.SizeOfRawData);
                }
            }

            foreach (var insertFile in _xbe.InsertFiles.Where(f => !f.NoPreload))
            {
                insertFile.FileByteOffset = fileByteOffset;
                fileByteOffset += Pe32Helpers.RoundToPages((int)insertFile.FileSize);
            }

            foreach (var sectionName in _options.NoPreloadSections.Select(entry => entry.SectionName))
            {
                if (Pe32Helpers.NameToSectionHeader(_inputImage, ref _ntHeaders, sectionName, out var sectionIndex) is not null &&
                    sectionIndex < executableSectionCount &&
                    imageFileByteOffsets[sectionIndex] == 0)
                {
                    var section = Pe32Helpers.ReadSectionHeader(_inputImage, sectionIndex);
                    section.SizeOfRawData = (uint)Pe32Helpers.TrimSectionRawDataSize(_inputImage, ref _ntHeaders, section, _inputPath);
                    Pe32Helpers.WriteSectionHeader(_inputImage, sectionIndex, section);
                    imageFileByteOffsets[sectionIndex] = fileByteOffset;
                    fileByteOffset += Pe32Helpers.RoundToPages((int)section.SizeOfRawData);
                }
            }

            foreach (var insertFile in _xbe.InsertFiles.Where(f => f.NoPreload))
            {
                insertFile.FileByteOffset = fileByteOffset;
                fileByteOffset += Pe32Helpers.RoundToPages((int)insertFile.FileSize);
            }

            var sharedReferenceCountIndex = 0;
            var sharedReferenceCountOffset = sectionHeaderByteOffset +
                                             (int)_xbe.ImageHeader.NumberOfSections * Marshal.SizeOf<XbeImageSection>();
            var endingSharedReferenceCount = sharedReferenceCountOffset +
                                             (int)_xbe.SectionHeaders.NumberOfSharedPageReferenceCounts * sizeof(ushort);
            var firstSharedReferenceCount = true;
            uint lastEndingVirtualAddress = 0;

            for (var i = 0; i < executableSectionCount; i++)
            {
                var section = Pe32Helpers.ReadSectionHeader(_inputImage, i);
                ReadOnlySpan<byte> sectionData = section.SizeOfRawData > 0
                    ? Pe32Helpers.VirtualAddressToData(_inputImage, ref _ntHeaders, section.VirtualAddress, _inputPath)[..(int)section.SizeOfRawData]
                    : ReadOnlySpan<byte>.Empty;

                SeekOutputFile(imageFileByteOffsets[i]);
                WriteOutputFile(sectionData);
                WriteZeroPadding(XbeImageConstants.PageSize - Pe32Helpers.ByteOffset((int)section.SizeOfRawData));

                var sectionName = Pe32Helpers.GetSectionName(section);
                SeekOutputFile(sectionHeaderNameByteOffset);
                WriteOutputFile(Encoding.ASCII.GetBytes(sectionName + "\0"));

                var xbeSection = new XbeImageSection
                {
                    VirtualAddress = section.VirtualAddress + firstSectionBaseAddress,
                    VirtualSize = Math.Max(section.SizeOfRawData, section.VirtualSize),
                    PointerToRawData = (uint)imageFileByteOffsets[i],
                    SizeOfRawData = section.SizeOfRawData,
                    SectionName = (uint)(sectionHeaderNameByteOffset + XbeImageConstants.StandardBaseAddress),
                    SectionReferenceCount = 0,
                    SectionFlags = XbeImageConstants.SectionExecutable,
                    SectionDigest = new byte[XbeImageConstants.DigestLength],
                };

                if ((section.Characteristics & XbeImageConstants.ImageScnMemWrite) != 0)
                {
                    xbeSection.SectionFlags |= XbeImageConstants.SectionWriteable;
                }

                if ((section.Characteristics & XbeImageConstants.ImageScnMemPreload) != 0)
                {
                    xbeSection.SectionFlags |= XbeImageConstants.SectionPreload;
                }

                XbeImageCrypto.CalcDigest(sectionData, xbeSection.SectionDigest);

                if (!firstSharedReferenceCount &&
                    Pe32Helpers.PageAlign((int)xbeSection.VirtualAddress) !=
                    Pe32Helpers.PageAlign((int)lastEndingVirtualAddress))
                {
                    sharedReferenceCountIndex++;
                }

                xbeSection.HeadSharedPageReferenceCount =
                    (uint)(sharedReferenceCountOffset + sharedReferenceCountIndex * sizeof(ushort) +
                           XbeImageConstants.StandardBaseAddress);

                if (Pe32Helpers.PageAlign((int)xbeSection.VirtualAddress) !=
                    Pe32Helpers.PageAlign((int)(xbeSection.VirtualAddress + xbeSection.VirtualSize - 1)))
                {
                    sharedReferenceCountIndex++;
                }

                xbeSection.TailSharedPageReferenceCount =
                    (uint)(sharedReferenceCountOffset + sharedReferenceCountIndex * sizeof(ushort) +
                           XbeImageConstants.StandardBaseAddress);

                firstSharedReferenceCount = false;
                lastEndingVirtualAddress = xbeSection.VirtualAddress + xbeSection.VirtualSize - 1;

                SeekOutputFile(sectionHeaderByteOffset);
                WriteOutputFile(BinaryStructWriter.GetSectionBytes(xbeSection));
                sectionHeaderByteOffset += Marshal.SizeOf<XbeImageSection>();
                sectionHeaderNameByteOffset += Encoding.ASCII.GetByteCount(sectionName) + 1;
            }

            var insertFileVirtualAddress = XbeImageConstants.StandardBaseAddress +
                                           (uint)Pe32Helpers.RoundToPages((int)_xbe.ImageHeader.SizeOfHeaders) +
                                           _xbe.SizeOfExecutableImage;
            foreach (var insertFile in _xbe.InsertFiles)
            {
                var alignedFileSize = (insertFile.FileSize + XbeImageConstants.InsertFileSectionAlignment - 1) &
                                      ~(XbeImageConstants.InsertFileSectionAlignment - 1u);

                SeekOutputFile(sectionHeaderNameByteOffset);
                WriteOutputFile(Encoding.ASCII.GetBytes(insertFile.SectionName + "\0"));

                var xbeSection = new XbeImageSection
                {
                    VirtualAddress = insertFileVirtualAddress,
                    VirtualSize = insertFile.FileSize,
                    PointerToRawData = (uint)insertFile.FileByteOffset,
                    SizeOfRawData = insertFile.FileSize,
                    SectionName = (uint)(sectionHeaderNameByteOffset + XbeImageConstants.StandardBaseAddress),
                    SectionReferenceCount = 0,
                    SectionFlags = XbeImageConstants.SectionInsertFile,
                    SectionDigest = new byte[XbeImageConstants.DigestLength],
                };

                if (!insertFile.ReadOnly)
                {
                    xbeSection.SectionFlags |= XbeImageConstants.SectionWriteable;
                }

                if (!insertFile.NoPreload)
                {
                    xbeSection.SectionFlags |= XbeImageConstants.SectionPreload;
                }

                if (!firstSharedReferenceCount &&
                    Pe32Helpers.PageAlign((int)xbeSection.VirtualAddress) !=
                    Pe32Helpers.PageAlign((int)lastEndingVirtualAddress))
                {
                    sharedReferenceCountIndex++;
                }

                xbeSection.HeadSharedPageReferenceCount =
                    (uint)(sharedReferenceCountOffset + sharedReferenceCountIndex * sizeof(ushort) +
                           XbeImageConstants.StandardBaseAddress);

                if (Pe32Helpers.PageAlign((int)xbeSection.VirtualAddress) !=
                    Pe32Helpers.PageAlign((int)(xbeSection.VirtualAddress + xbeSection.VirtualSize - 1)))
                {
                    sharedReferenceCountIndex++;
                }

                xbeSection.TailSharedPageReferenceCount =
                    (uint)(sharedReferenceCountOffset + sharedReferenceCountIndex * sizeof(ushort) +
                           XbeImageConstants.StandardBaseAddress);

                firstSharedReferenceCount = false;
                lastEndingVirtualAddress = xbeSection.VirtualAddress + xbeSection.VirtualSize - 1;

                SeekOutputFile(insertFile.FileByteOffset);
                ComputeInsertFileDigest(insertFile, xbeSection.SectionDigest);
                WriteZeroPadding(XbeImageConstants.PageSize - Pe32Helpers.ByteOffset((int)xbeSection.SizeOfRawData));

                SeekOutputFile(sectionHeaderByteOffset);
                WriteOutputFile(BinaryStructWriter.GetSectionBytes(xbeSection));
                sectionHeaderByteOffset += Marshal.SizeOf<XbeImageSection>();
                sectionHeaderNameByteOffset += Encoding.ASCII.GetByteCount(insertFile.SectionName) + 1;
                insertFileVirtualAddress += alignedFileSize;
            }

            if (sharedReferenceCountOffset + sharedReferenceCountIndex * sizeof(ushort) >= endingSharedReferenceCount)
            {
                throw new XbeImageException("Internal tool error while writing section headers.");
            }

            PostProcessSectionHeaders();
        }

        private void ComputeInsertFileDigest(RuntimeInsertFile insertFile, byte[] digest)
        {
            var ctx = new AShaContext();
            XbeSha1.Init(ctx);
            var remainingBytes = insertFile.FileSize;
            Span<byte> lengthPrefix = stackalloc byte[4];
            BinaryPrimitives.WriteUInt32LittleEndian(lengthPrefix, remainingBytes);
            XbeSha1.Update(ctx, lengthPrefix);
            using var input = File.OpenRead(insertFile.FilePath);
            var buffer = new byte[TransferBufferSize];
            while (remainingBytes > 0)
            {
                var toRead = (int)Math.Min(remainingBytes, (uint)buffer.Length);
                var read = input.Read(buffer, 0, toRead);
                if (read != toRead)
                {
                    throw new XbeImageException($"Cannot read input file: {insertFile.FilePath}");
                }

                XbeSha1.Update(ctx, buffer.AsSpan(0, read));
                WriteOutputFile(buffer.AsSpan(0, read));
                remainingBytes -= (uint)read;
            }

            XbeSha1.Final(ctx, digest);
        }

        private void PostProcessSectionHeaders()
        {
            var sectionHeaderByteOffset = (int)_xbe.SectionHeaders.VirtualAddress - (int)XbeImageConstants.StandardBaseAddress;
            var sectionSize = Marshal.SizeOf<XbeImageSection>();
            var sectionBytes = new byte[_xbe.ImageHeader.NumberOfSections * sectionSize];
            SeekOutputFile(sectionHeaderByteOffset);
            ReadOutputFile(sectionBytes);

            for (var i = 0; i < _xbe.ImageHeader.NumberOfSections; i++)
            {
                var offset = i * sectionSize;
                var section = ReadSectionFromBytes(sectionBytes.AsSpan(offset));
                if (section.VirtualSize == 0 || (section.SectionFlags & XbeImageConstants.SectionWriteable) != 0)
                {
                    continue;
                }

                if (CheckForReadOnlyPage(sectionBytes, sectionSize, section.VirtualAddress))
                {
                    section.SectionFlags |= XbeImageConstants.SectionHeadPageReadonly;
                }

                if (CheckForReadOnlyPage(sectionBytes, sectionSize, section.VirtualAddress + section.VirtualSize - 1))
                {
                    section.SectionFlags |= XbeImageConstants.SectionTailPageReadonly;
                }

                BinaryStructWriter.WriteSectionToBytes(sectionBytes.AsSpan(offset), section);
            }

            SeekOutputFile(sectionHeaderByteOffset);
            WriteOutputFile(sectionBytes);
        }

        private static XbeImageSection ReadSectionFromBytes(ReadOnlySpan<byte> data)
        {
            return new XbeImageSection
            {
                SectionFlags = BinaryPrimitives.ReadUInt32LittleEndian(data),
                VirtualAddress = BinaryPrimitives.ReadUInt32LittleEndian(data[4..]),
                VirtualSize = BinaryPrimitives.ReadUInt32LittleEndian(data[8..]),
                PointerToRawData = BinaryPrimitives.ReadUInt32LittleEndian(data[12..]),
                SizeOfRawData = BinaryPrimitives.ReadUInt32LittleEndian(data[16..]),
                SectionName = BinaryPrimitives.ReadUInt32LittleEndian(data[20..]),
                SectionReferenceCount = BinaryPrimitives.ReadUInt32LittleEndian(data[24..]),
                HeadSharedPageReferenceCount = BinaryPrimitives.ReadUInt32LittleEndian(data[28..]),
                TailSharedPageReferenceCount = BinaryPrimitives.ReadUInt32LittleEndian(data[32..]),
                SectionDigest = data.Slice(36, XbeImageConstants.DigestLength).ToArray(),
            };
        }

        private static bool CheckForReadOnlyPage(byte[] sectionBytes, int sectionSize, uint virtualAddress)
        {
            for (var i = 0; i < sectionBytes.Length; i += sectionSize)
            {
                var section = ReadSectionFromBytes(sectionBytes.AsSpan(i, sectionSize));
                if (section.VirtualSize == 0 ||
                    (section.SectionFlags & XbeImageConstants.SectionWriteable) != 0)
                {
                    continue;
                }

                var startingAddress = Pe32Helpers.PageAlign((int)section.VirtualAddress);
                var endingAddress = Pe32Helpers.PageAlign((int)(section.VirtualAddress + section.VirtualSize + XbeImageConstants.PageSize - 1));
                if (startingAddress <= virtualAddress && virtualAddress < (uint)endingAddress)
                {
                    return false;
                }
            }

            return true;
        }

        private void CheckDuplicateInsertFileSection(ImageBldInsertFileEntry insertFile)
        {
            foreach (var existing in _options.InsertFiles)
            {
                if (!ReferenceEquals(existing, insertFile) &&
                    string.Equals(existing.SectionName, insertFile.SectionName, StringComparison.OrdinalIgnoreCase))
                {
                    throw new XbeImageException($"Insert file section name conflict: {insertFile.SectionName}");
                }
            }

            var sectionCount = Pe32Helpers.SectionCount(_inputImage);
            for (var i = 0; i < sectionCount; i++)
            {
                var section = Pe32Helpers.ReadSectionHeader(_inputImage, i);
                if (string.Equals(Pe32Helpers.GetSectionName(section), insertFile.SectionName, StringComparison.OrdinalIgnoreCase))
                {
                    throw new XbeImageException($"Insert file section conflicts with executable section: {insertFile.SectionName}");
                }
            }
        }

        private bool SearchNoPreloadList(string sectionName) =>
            _options.NoPreloadSections.Any(entry =>
                string.Equals(entry.SectionName, sectionName, StringComparison.OrdinalIgnoreCase));

        private static char[] ToFixedTitleName(string titleName)
        {
            var buffer = new char[XbeImageConstants.TitleNameLength];
            titleName.AsSpan(0, Math.Min(titleName.Length, buffer.Length)).CopyTo(buffer);
            return buffer;
        }

        private uint[] BuildAlternateTitleIds()
        {
            var ids = new uint[XbeImageConstants.AlternateTitleIdCount];
            for (var i = 0; i < ids.Length && i < _options.TestAlternateTitleIds.Count; i++)
            {
                ids[i] = _options.TestAlternateTitleIds[i].TitleId;
            }

            return ids;
        }

        private void ApplyLibraryApproval(List<XbeImageLibraryVersion> libraryVersions, XbeImageLibraryVersion libraryVersion, int approvalLevel)
        {
            for (var i = 0; i < libraryVersions.Count; i++)
            {
                if (libraryVersions[i].LibraryName.AsSpan()
                    .SequenceEqual(libraryVersion.LibraryName.AsSpan(0, XbeImageConstants.LibraryVersionNameLength)))
                {
                    var updated = libraryVersions[i];
                    if (approvalLevel >= 0)
                    {
                        updated.SetApprovedLibrary(approvalLevel);
                    }

                    libraryVersions[i] = updated;
                    if (!_options.NoWarnLibraryApproval)
                    {
                        PrintUnapprovedLibraryWarning(updated, approvalLevel);
                    }

                    return;
                }
            }
        }

        private void PrintUnapprovedLibraryWarning(XbeImageLibraryVersion libraryVersion, int approvalLevel)
        {
            if (approvalLevel >= 0)
            {
                libraryVersion.VersionFlags = (ushort)((libraryVersion.VersionFlags & ~0x6000) | ((approvalLevel & 0x3) << 13));
            }

            var libraryName = Encoding.ASCII.GetString(libraryVersion.LibraryName).TrimEnd('\0');
            Warn(approvalLevel switch
            {
                -1 => $"Expired library: {libraryName}",
                1 => $"Possibly unapproved library: {libraryName}",
                _ => $"Unapproved library: {libraryName}",
            });
        }

        private static XbeImageLibraryVersion ReadLibraryVersion(ReadOnlySpan<byte> source, int offset)
        {
            return new XbeImageLibraryVersion
            {
                LibraryName = source.Slice(offset, XbeImageConstants.LibraryVersionNameLength).ToArray(),
                MajorVersion = BinaryPrimitives.ReadUInt16LittleEndian(source[(offset + 8)..]),
                MinorVersion = BinaryPrimitives.ReadUInt16LittleEndian(source[(offset + 10)..]),
                BuildVersion = BinaryPrimitives.ReadUInt16LittleEndian(source[(offset + 12)..]),
                VersionFlags = BinaryPrimitives.ReadUInt16LittleEndian(source[(offset + 14)..]),
            };
        }

        private static bool MatchesLibraryName(byte[] libraryName, string expected)
        {
            var expectedBytes = Encoding.ASCII.GetBytes(expected.PadRight(XbeImageConstants.LibraryVersionNameLength, '\0'));
            return libraryName.AsSpan(0, XbeImageConstants.LibraryVersionNameLength)
                .SequenceEqual(expectedBytes.AsSpan(0, XbeImageConstants.LibraryVersionNameLength));
        }

        private static string ReadAsciiNullTerminated(ReadOnlySpan<byte> data)
        {
            var length = data.IndexOf((byte)0);
            if (length < 0)
            {
                length = data.Length;
            }

            return Encoding.ASCII.GetString(data[..length]);
        }

        private void Warn(string message)
        {
        }

        private void SeekOutputFile(int offset) => _outputStream!.Seek(offset, SeekOrigin.Begin);

        private void WriteOutputFile(ReadOnlySpan<byte> buffer) => _outputStream!.Write(buffer);

        private void ReadOutputFile(Span<byte> buffer)
        {
            var read = _outputStream!.Read(buffer);
            if (read != buffer.Length)
            {
                throw new XbeImageException($"Cannot read output file: {_outputPath}");
            }
        }

        private void WriteZeroPadding(int numberOfBytes)
        {
            Span<byte> zeroes = stackalloc byte[1024];
            zeroes.Clear();
            while (numberOfBytes > 0)
            {
                var pass = Math.Min(numberOfBytes, zeroes.Length);
                WriteOutputFile(zeroes[..pass]);
                numberOfBytes -= pass;
            }
        }
    }

    private sealed class XbeBuildImageHeader
    {
        public XbeImageHeader ImageHeader;
        public uint NewBaseAddress;
        public int ExecutableSectionCount;
        public uint SizeOfExecutableImage;
        public uint SizeOfInsertFilesImage;
        public List<XbeGenericHeader> Headers { get; } = new();
        public CertificateHeader CertificateHeader { get; } = new();
        public ImportDescriptorHeader ImportDescriptorHeader { get; } = new();
        public TlsRawDataHeader TlsRawDataHeader { get; } = new();
        public LibraryVersionHeader LibraryVersionHeader { get; } = new();
        public SectionHeadersHeader SectionHeaders { get; } = new();
        public DebugPathsHeader DebugPathsHeader { get; } = new();
        public MicrosoftLogoHeader MicrosoftLogoHeader { get; } = new();
        public PeHeaderHeader PeHeaderHeader { get; } = new();
        public List<RuntimeInsertFile> InsertFiles { get; } = new();
    }

    private abstract class XbeGenericHeader
    {
        public uint VirtualAddress;
        public uint VirtualSize;
        public abstract void Write(BuildContext context);
    }

    private sealed class CertificateHeader : XbeGenericHeader
    {
        public override void Write(BuildContext context) => context.WriteCertificateHeader();
    }

    private sealed class ImportDescriptorHeader : XbeGenericHeader
    {
        public uint NumberOfNonKernelImports;
        public override void Write(BuildContext context) => context.WriteImportDescriptorHeader();
    }

    private sealed class TlsRawDataHeader : XbeGenericHeader
    {
        public int TlsDirectoryOffset;
        public byte[] RawData = Array.Empty<byte>();
        public override void Write(BuildContext context) => context.WriteTlsRawDataHeader();
    }

    private sealed class LibraryVersionHeader : XbeGenericHeader
    {
        public XbeImageLibraryVersion[] LibraryVersions = Array.Empty<XbeImageLibraryVersion>();
        public uint XboxKernelOffset = uint.MaxValue;
        public uint XapiOffset = uint.MaxValue;
        public override void Write(BuildContext context) => context.WriteLibraryVersionHeader();
    }

    private sealed class SectionHeadersHeader : XbeGenericHeader
    {
        public uint NumberOfExecutableSections;
        public uint NumberOfInsertFileSections;
        public uint SizeOfSectionNames;
        public uint NumberOfSharedPageReferenceCounts;
        public override void Write(BuildContext context) => context.WriteSectionHeaders();
    }

    private sealed class DebugPathsHeader : XbeGenericHeader
    {
        public override void Write(BuildContext context) => context.WriteDebugPathsHeader();
    }

    private sealed class MicrosoftLogoHeader : XbeGenericHeader
    {
        public override void Write(BuildContext context) => context.WriteMicrosoftLogoHeader();
    }

    private sealed class PeHeaderHeader : XbeGenericHeader
    {
        public override void Write(BuildContext context) => context.WritePeHeaderHeader();
    }

    private sealed class RuntimeInsertFile
    {
        public RuntimeInsertFile(ImageBldInsertFileEntry options, uint fileSize, bool noPreload)
        {
            FilePath = options.FilePath;
            SectionName = options.SectionName;
            NoPreload = noPreload;
            ReadOnly = options.ReadOnly;
            FileSize = fileSize;
        }

        public string FilePath { get; }
        public string SectionName { get; }
        public bool NoPreload { get; }
        public bool ReadOnly { get; }
        public uint FileSize { get; }
        public int FileByteOffset { get; set; }
    }
}
