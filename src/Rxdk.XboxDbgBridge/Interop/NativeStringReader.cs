using System.Runtime.InteropServices;
using System.Text;

namespace Rxdk.XboxDbgBridge.Interop;

/// <summary>
/// Reads NUL-terminated native strings while tolerating a bad pointer. dbghelp's
/// <c>SymGetTypeInfo(TI_GET_SYMNAME)</c> can hand back a pointer that is not a safely
/// terminated string for our Zig-produced PDBs; a plain <see cref="Marshal.PtrToStringUni(IntPtr)"/>
/// then scans off the end of the mapping and raises an <see cref="AccessViolationException"/>. That
/// exception is not catchable on modern .NET, so it takes the whole bridge process down (observed
/// when VS Code auto-fetches locals on a breakpoint stop). Guarding the read with VirtualQuery keeps
/// a malformed pointer from crashing the debugger — the worst case becomes an empty type name.
/// </summary>
internal static class NativeStringReader
{
    private const uint MemCommit = 0x1000;
    private const uint PageGuard = 0x100;
    private const uint PageNoAccess = 0x01;

    // Any protection that permits reads (write-copy and execute-read variants included).
    private const uint ReadableMask =
        0x02 /* READONLY */ | 0x04 /* READWRITE */ | 0x08 /* WRITECOPY */ |
        0x20 /* EXECUTE_READ */ | 0x40 /* EXECUTE_READWRITE */ | 0x80 /* EXECUTE_WRITECOPY */;

    [StructLayout(LayoutKind.Sequential)]
    private struct MemoryBasicInformation
    {
        public UIntPtr BaseAddress;
        public UIntPtr AllocationBase;
        public uint AllocationProtect;
        public UIntPtr RegionSize;
        public uint State;
        public uint Protect;
        public uint Type;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern UIntPtr VirtualQuery(IntPtr address, out MemoryBasicInformation buffer, UIntPtr length);

    /// <summary>
    /// Reads up to <paramref name="maxChars"/> UTF-16 chars from <paramref name="ptr"/>, stopping at
    /// the NUL terminator, without ever reading past the committed, readable region that contains it.
    /// Returns false (with an empty string) for a null pointer or unreadable memory.
    /// </summary>
    internal static bool TryReadWideString(IntPtr ptr, int maxChars, out string value)
    {
        value = string.Empty;
        if (ptr == IntPtr.Zero || maxChars <= 0)
            return false;

        if (VirtualQuery(ptr, out var mbi, (UIntPtr)Marshal.SizeOf<MemoryBasicInformation>()) == UIntPtr.Zero)
            return false;
        if (mbi.State != MemCommit)
            return false;
        if ((mbi.Protect & (PageGuard | PageNoAccess)) != 0 || (mbi.Protect & ReadableMask) == 0)
            return false;

        // VirtualQuery reports one contiguous run of pages with identical protection, so every byte
        // from BaseAddress up to BaseAddress+RegionSize is safe to read. Bound the scan to that run.
        var start = (ulong)ptr;
        var regionEnd = (ulong)mbi.BaseAddress + (ulong)mbi.RegionSize;
        if (start >= regionEnd)
            return false;

        var maxByRegion = (int)Math.Min((ulong)maxChars, (regionEnd - start) / 2);
        var builder = new StringBuilder(Math.Min(maxByRegion, 128));
        for (var i = 0; i < maxByRegion; i++)
        {
            var ch = (char)(ushort)Marshal.ReadInt16(ptr, i * 2);
            if (ch == '\0')
                break;
            builder.Append(ch);
        }

        value = builder.ToString();
        return value.Length > 0;
    }
}
