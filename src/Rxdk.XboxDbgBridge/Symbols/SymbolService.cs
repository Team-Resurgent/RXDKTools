using System.Runtime.InteropServices;
using System.Text;
using Rxdk.Pdb;
using Rxdk.XboxDbgBridge.Interop;

namespace Rxdk.XboxDbgBridge.Symbols;

internal sealed class SymbolService : IDisposable
{
    private bool _initialized;
    private ulong _pdbBase;
    private nuint _moduleBase;
    private string _loadedModule = string.Empty;
    private string _mapPath = string.Empty;
    private uint _mapLinkBase = 0x400000;

    // Pure-managed PDB reader, preferred over dbghelp for locals (dbghelp mis-parses the modern
    // S_LOCAL/S_DEFRANGE records Zig/LLVM emit). Opened lazily from the loaded PDB path.
    private string _pdbPath = string.Empty;
    private PdbImage? _pdbImage;
    private bool _managedUnavailable;

    internal bool IsAvailable => OperatingSystem.IsWindows();

    internal ulong PdbBase => _pdbBase;

    internal nuint ModuleBase
    {
        get => _moduleBase;
        set => _moduleBase = value;
    }

    internal void Load(string exePath, string pdbPath, string? mapPath)
        => LoadModule(exePath, ReadPeImageSize(exePath), pdbPath, mapPath);

    internal void LoadFromXbe(string xbePath, string pdbPath, string? mapPath)
        => LoadModule(xbePath, ReadXbeImageSize(xbePath), pdbPath, mapPath);

    private void LoadModule(string imagePath, uint imageSize, string pdbPath, string? mapPath)
    {
        if (!IsAvailable)
            throw new PlatformNotSupportedException("DbgHelp symbols require Windows.");

        Unload();

        if (!_initialized)
        {
            DbgHelpNative.SymSetOptions(DbgHelpNative.SymoptUndname | DbgHelpNative.SymoptLoadLines);
            if (!DbgHelpNative.SymInitialize(DbgHelpNative.PseudoProcess, null, false))
                throw new InvalidOperationException($"SymInitialize failed: {Marshal.GetLastWin32Error()}");
            _initialized = true;
        }

        // Prefer the PDB directory for the symbol search path: prebuilt/legacy XBEs may
        // ship without a sibling .exe, but the PDB is always present for symbol debugging.
        var searchPath = Path.GetDirectoryName(pdbPath) ?? Path.GetDirectoryName(imagePath) ?? ".";
        DbgHelpNative.SymSetSearchPath(DbgHelpNative.PseudoProcess, searchPath);

        var moduleName = Path.GetFileName(imagePath);
        var baseAddr = DbgHelpNative.SymLoadModuleEx(
            DbgHelpNative.PseudoProcess,
            IntPtr.Zero,
            moduleName,
            pdbPath,
            0x400000,
            imageSize,
            IntPtr.Zero,
            0);
        if (baseAddr == 0)
        {
            baseAddr = DbgHelpNative.SymLoadModuleEx(
                DbgHelpNative.PseudoProcess,
                IntPtr.Zero,
                imagePath,
                pdbPath,
                0x400000,
                imageSize,
                IntPtr.Zero,
                0);
        }

        if (baseAddr == 0)
            throw new InvalidOperationException($"SymLoadModuleEx failed: {Marshal.GetLastWin32Error()}");

        _loadedModule = moduleName;
        _pdbBase = baseAddr;
        _pdbPath = pdbPath;
        _pdbImage = null;
        _managedUnavailable = false;

        var map = string.IsNullOrWhiteSpace(mapPath)
            ? Path.ChangeExtension(imagePath, ".map")
            : mapPath;
        if (File.Exists(map))
        {
            _mapPath = map;
            _mapLinkBase = MapFileGlobals.ReadLinkBase(map) ?? 0x400000;
        }
        else
        {
            _mapPath = string.Empty;
            _mapLinkBase = 0x400000;
        }
    }

    internal void Unload()
    {
        if (_initialized)
        {
            DbgHelpNative.SymCleanup(DbgHelpNative.PseudoProcess);
            _initialized = false;
        }

        _pdbBase = 0;
        _moduleBase = 0;
        _loadedModule = string.Empty;
        _mapPath = string.Empty;
        _mapLinkBase = 0x400000;
        _pdbPath = string.Empty;
        _pdbImage = null;
        _managedUnavailable = false;
    }

    /// <summary>Builds a managed-locals reader over the loaded PDB, or null if it can't be used.</summary>
    private ManagedSymbols? TryGetManaged()
    {
        if (_managedUnavailable || _moduleBase == 0 || string.IsNullOrEmpty(_pdbPath))
            return null;

        if (_pdbImage is null)
        {
            try
            {
                _pdbImage = PdbImage.OpenFile(_pdbPath);
            }
            catch (Exception ex)
            {
                _managedUnavailable = true;
                BridgeWriter.Log($"managed PDB open failed ({_pdbPath}): {ex.Message}");
                return null;
            }
        }

        return new ManagedSymbols(_pdbImage, _moduleBase);
    }

    internal nuint RelocateAddress(nuint pdbAddress)
    {
        if (_moduleBase == 0 || _pdbBase == 0)
            return pdbAddress;
        return _moduleBase + (pdbAddress - (nuint)_pdbBase);
    }

    internal nuint NormalizeBreakpointAddress(nuint address)
    {
        if (address == 0 || _moduleBase == 0 || _pdbBase == 0)
            return address;

        var pdbBase = (nuint)_pdbBase;
        if (address >= pdbBase && address < pdbBase + 0x100000 &&
            (address < _moduleBase || address >= _moduleBase + 0x100000))
            return RelocateAddress(address);
        return address;
    }

    internal bool IsKitBreakpointAddress(nuint address)
    {
        if (address == 0)
            return false;
        if (_moduleBase != 0)
            return address >= _moduleBase && address < _moduleBase + 0x01000000;
        return address < 0x00400000 || address >= 0x00600000;
    }

    internal bool TryResolveLine(string file, uint line, out nuint address)
    {
        address = 0;
        if (!_initialized || _pdbBase == 0)
            return false;

        if (TryLookupLineExact(file, line, out var pdbAddr) ||
            TryLookupLineFromFunction("_main", file, line, out pdbAddr) ||
            TryLookupLineFromFunction("main", file, line, out pdbAddr) ||
            TryLookupLineNearest(file, line, out pdbAddr))
        {
            address = _moduleBase != 0 ? RelocateAddress((nuint)pdbAddr) : (nuint)pdbAddr;
            return true;
        }

        return false;
    }

    internal bool TryAddressToLine(nuint kitAddress, out string file, out uint line, out string function)
    {
        file = string.Empty;
        line = 0;
        function = string.Empty;
        if (!_initialized)
            return false;

        var pdbAddr = _moduleBase != 0 && _pdbBase != 0
            ? _pdbBase + (kitAddress - _moduleBase)
            : (ulong)kitAddress;

        if (TrySymFromAddr(pdbAddr, out var symName))
            function = symName;
        else if (pdbAddr > 0 && TrySymFromAddr(pdbAddr - 1, out symName))
            function = symName;

        var imageLine = new DbgHelpNative.ImageHlpLine64 { SizeOfStruct = (uint)Marshal.SizeOf<DbgHelpNative.ImageHlpLine64>() };
        if (!DbgHelpNative.SymGetLineFromAddr64(DbgHelpNative.PseudoProcess, pdbAddr, out _, ref imageLine))
            return false;

        file = Marshal.PtrToStringAnsi(imageLine.FileName) ?? string.Empty;
        line = imageLine.LineNumber;
        return true;
    }

    internal string Diag()
    {
        if (!_initialized)
            return "pdbBase=0 symType=0";

        var mod = new DbgHelpNative.ImageHlpModule64
        {
            SizeOfStruct = (uint)Marshal.SizeOf<DbgHelpNative.ImageHlpModule64>(),
        };
        if (_pdbBase != 0)
            DbgHelpNative.SymGetModuleInfo64(DbgHelpNative.PseudoProcess, _pdbBase, ref mod);

        var builder = new StringBuilder();
        builder.Append($"pdbBase=0x{_pdbBase:x} symType={mod.SymType}");
        if (TrySymFromName("_main", out var mainAddr, out _))
            builder.Append($" _main=0x{mainAddr:x}");
        return builder.ToString();
    }

    internal bool TryEvaluate(string expression, ref Xbdm.XbdmContext context, KitMemoryAccess memory, out string value, out string? error)
    {
        value = string.Empty;
        error = null;
        if (!_initialized || _pdbBase == 0)
        {
            error = "symbolsNotLoaded";
            return false;
        }

        return CreateTypeEngine().TryEvaluate(expression, ref context, memory, out value, out error);
    }

    internal void EmitLocals(ref Xbdm.XbdmContext context, VariableJson variables, KitMemoryAccess memory)
    {
        if (!_initialized || _pdbBase == 0)
            return;

        var managed = TryGetManaged();
        if (managed is not null)
        {
            try
            {
                if (managed.EmitLocals(ref context, variables, memory))
                    return;
            }
            catch (Exception ex)
            {
                BridgeWriter.Log($"managed EmitLocals failed: {ex.Message}");
            }
        }

        CreateTypeEngine().EmitLocals(ref context, variables, memory);
    }

    internal bool TryEmitMembers(string symbolBase, ref Xbdm.XbdmContext context, KitMemoryAccess memory, VariableJson variables)
    {
        if (!_initialized || _pdbBase == 0)
            return false;

        var managed = TryGetManaged();
        if (managed is not null)
        {
            try
            {
                if (managed.TryEmitMembers(symbolBase, ref context, variables, memory))
                    return true;
            }
            catch (Exception ex)
            {
                BridgeWriter.Log($"managed TryEmitMembers failed: {ex.Message}");
            }
        }

        return CreateTypeEngine().TryEmitMembers(symbolBase, ref context, memory, variables);
    }

    private SymbolTypeEngine CreateTypeEngine() => new(_pdbBase, _moduleBase);

    internal void EmitGlobals(VariableJson variables, int maxVars)
    {
        if (string.IsNullOrEmpty(_mapPath))
            return;
        MapFileGlobals.Emit(_mapPath, _mapLinkBase, _moduleBase, variables, maxVars);
    }

    internal void EmitRegisters(VariableJson variables, ref Xbdm.XbdmContext context)
    {
        variables.Append("EAX", $"0x{context.Eax:x8}");
        variables.Append("EBX", $"0x{context.Ebx:x8}");
        variables.Append("ECX", $"0x{context.Ecx:x8}");
        variables.Append("EDX", $"0x{context.Edx:x8}");
        variables.Append("ESI", $"0x{context.Esi:x8}");
        variables.Append("EDI", $"0x{context.Edi:x8}");
        variables.Append("EBP", $"0x{context.Ebp:x8}");
        variables.Append("ESP", $"0x{context.Esp:x8}");
        variables.Append("EIP", $"0x{context.Eip:x8}");
        variables.Append("EFLAGS", $"0x{context.EFlags:x8}");
    }

    public void Dispose() => Unload();

    private static uint ReadPeImageSize(string exePath)
    {
        try
        {
            using var stream = File.OpenRead(exePath);
            using var reader = new BinaryReader(stream);
            if (stream.Length < 0x40)
                return 0;
            stream.Position = 0x3C;
            var peOffset = reader.ReadInt32();
            if (peOffset <= 0 || peOffset + 0x58 > stream.Length)
                return 0;
            stream.Position = peOffset;
            if (reader.ReadUInt32() != 0x00004550)
                return 0;
            stream.Position = peOffset + 0x50;
            return reader.ReadUInt32();
        }
        catch
        {
            return 0;
        }
    }

    // XBE header layout: 'XBEH' signature at 0, then a 256-byte encrypted digest at 4,
    // so the base-address header begins at 0x104. SizeOfImage and NtSizeOfImage are read
    // relative to that base (see Rxdk.XbeImage.XbeImageReader).
    private const int XbeBaseAddressOffset = 4 + 256;
    private const uint XbeSignature = 0x48454258; // 'XBEH'

    private static uint ReadXbeImageSize(string xbePath)
    {
        try
        {
            using var stream = File.OpenRead(xbePath);
            using var reader = new BinaryReader(stream);
            if (stream.Length < XbeBaseAddressOffset + 64)
                return 0;
            stream.Position = 0;
            if (reader.ReadUInt32() != XbeSignature)
                return 0;
            stream.Position = XbeBaseAddressOffset + 8; // SizeOfImage
            var sizeOfImage = reader.ReadUInt32();
            if (sizeOfImage != 0)
                return sizeOfImage;
            stream.Position = XbeBaseAddressOffset + 60; // NtSizeOfImage
            return reader.ReadUInt32();
        }
        catch
        {
            return 0;
        }
    }

    private bool TryLookupLineExact(string file, uint line, out ulong address)
    {
        address = 0;
        NormalizePath(file, out var normalized);
        var path = normalized;
        for (var pass = 0; pass < 2; pass++)
        {
            var module = pass == 0 && !string.IsNullOrEmpty(_loadedModule) ? _loadedModule : null;
            if (pass == 0 && module is null)
                continue;

            var imageLine = new DbgHelpNative.ImageHlpLine64
            {
                SizeOfStruct = (uint)Marshal.SizeOf<DbgHelpNative.ImageHlpLine64>(),
            };
            if (DbgHelpNative.SymGetLineFromName64(DbgHelpNative.PseudoProcess, module, path, line, out _, ref imageLine) &&
                imageLine.Address != 0)
            {
                address = imageLine.Address;
                return true;
            }

            var baseName = Path.GetFileName(path.Replace('/', '\\'));
            if (!string.Equals(baseName, path, StringComparison.Ordinal) &&
                DbgHelpNative.SymGetLineFromName64(DbgHelpNative.PseudoProcess, module, baseName, line, out _, ref imageLine) &&
                imageLine.Address != 0)
            {
                address = imageLine.Address;
                return true;
            }
        }

        return false;
    }

    private bool TryLookupLineFromFunction(string function, string file, uint line, out ulong address)
    {
        address = 0;
        if (!TrySymFromName(function, out var funcAddr, out _))
            return false;

        var imageLine = new DbgHelpNative.ImageHlpLine64
        {
            SizeOfStruct = (uint)Marshal.SizeOf<DbgHelpNative.ImageHlpLine64>(),
        };
        if (!DbgHelpNative.SymGetLineFromAddr64(DbgHelpNative.PseudoProcess, funcAddr, out _, ref imageLine))
            return false;

        ulong best = 0;
        var found = false;
        do
        {
            if (imageLine.LineNumber == line && FileBaseMatches(file, Marshal.PtrToStringAnsi(imageLine.FileName) ?? string.Empty) &&
                imageLine.Address != 0 && imageLine.Address >= best)
            {
                best = imageLine.Address;
                found = true;
            }
        } while (DbgHelpNative.SymGetLineNext64(DbgHelpNative.PseudoProcess, ref imageLine));

        imageLine = new DbgHelpNative.ImageHlpLine64
        {
            SizeOfStruct = (uint)Marshal.SizeOf<DbgHelpNative.ImageHlpLine64>(),
        };
        if (DbgHelpNative.SymGetLineFromAddr64(DbgHelpNative.PseudoProcess, funcAddr, out _, ref imageLine))
        {
            do
            {
                if (imageLine.LineNumber == line && FileBaseMatches(file, Marshal.PtrToStringAnsi(imageLine.FileName) ?? string.Empty) &&
                    imageLine.Address != 0 && imageLine.Address >= best)
                {
                    best = imageLine.Address;
                    found = true;
                }
            } while (DbgHelpNative.SymGetLinePrev64(DbgHelpNative.PseudoProcess, ref imageLine));
        }

        if (!found)
            return false;
        address = best;
        return true;
    }

    private bool TryLookupLineNearest(string file, uint line, out ulong address)
    {
        for (var delta = 1u; delta <= 12; delta++)
        {
            if (TryLookupLineExact(file, line + delta, out address))
                return true;
            if (line > delta && TryLookupLineExact(file, line - delta, out address))
                return true;
        }

        foreach (var function in new[] { "_main", "main", "wmain", "InitD3D", "InitVB", "InitTime", "UpdateTime", "Update", "Render" })
        {
            if (TryLookupLineFromFunction(function, file, line, out address))
                return true;
        }

        address = 0;
        return false;
    }

    private bool TrySymFromName(string name, out ulong address, out uint flags)
    {
        address = 0;
        flags = 0;
        var buffer = Marshal.AllocHGlobal(88 + DbgHelpNative.MaxSymName);
        try
        {
            Marshal.WriteInt32(buffer, 0, DbgHelpNative.SymbolInfoSize);
            Marshal.WriteInt32(buffer, DbgHelpNative.SymInfoMaxNameLen, DbgHelpNative.MaxSymName);
            if (!DbgHelpNative.SymFromName(DbgHelpNative.PseudoProcess, name, buffer))
                return false;
            flags = (uint)Marshal.ReadInt32(buffer, DbgHelpNative.SymInfoFlags);
            address = (ulong)Marshal.ReadInt64(buffer, DbgHelpNative.SymInfoAddress);
            return true;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    private bool TrySymFromAddr(ulong address, out string name)
    {
        name = string.Empty;
        var buffer = Marshal.AllocHGlobal(88 + DbgHelpNative.MaxSymName);
        try
        {
            Marshal.WriteInt32(buffer, 0, DbgHelpNative.SymbolInfoSize);
            Marshal.WriteInt32(buffer, DbgHelpNative.SymInfoMaxNameLen, DbgHelpNative.MaxSymName);
            if (!DbgHelpNative.SymFromAddr(DbgHelpNative.PseudoProcess, address, out _, buffer))
                return false;
            name = Marshal.PtrToStringAnsi(buffer + DbgHelpNative.SymInfoName) ?? string.Empty;
            return name.Length > 0;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    private static uint GetRegisterValue(ref Xbdm.XbdmContext context, string name) =>
        name.ToUpperInvariant() switch
        {
            "EAX" => context.Eax,
            "EBX" => context.Ebx,
            "ECX" => context.Ecx,
            "EDX" => context.Edx,
            "ESI" => context.Esi,
            "EDI" => context.Edi,
            "EBP" => context.Ebp,
            "ESP" => context.Esp,
            "EIP" => context.Eip,
            "EFLAGS" => context.EFlags,
            _ => 0,
        };

    private static void NormalizePath(string input, out string output)
    {
        input = input.Trim();
        if (input.StartsWith("file:///", StringComparison.OrdinalIgnoreCase))
            input = input[8..];
        else if (input.StartsWith("file://", StringComparison.OrdinalIgnoreCase))
            input = input[7..];
        output = input.Replace('/', '\\');
    }

    private static bool FileBaseMatches(string requested, string pdbPath)
    {
        NormalizePath(requested, out var req);
        NormalizePath(pdbPath, out var got);
        var reqBase = Path.GetFileName(req);
        var gotBase = Path.GetFileName(got);
        return string.Equals(reqBase, gotBase, StringComparison.OrdinalIgnoreCase) ||
               got.Contains(reqBase, StringComparison.OrdinalIgnoreCase) ||
               req.Contains(gotBase, StringComparison.OrdinalIgnoreCase);
    }
}
