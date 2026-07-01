using System.Runtime.InteropServices;
using System.Text;
using Rxdk.Xbdm;
using Rxdk.XboxDbgBridge.Interop;

namespace Rxdk.XboxDbgBridge.Symbols;

internal sealed partial class SymbolTypeEngine
{
    private readonly ulong _pdbBase;
    private readonly nuint _moduleBase;

    internal SymbolTypeEngine(ulong pdbBase, nuint moduleBase)
    {
        _pdbBase = pdbBase;
        _moduleBase = moduleBase;
    }

    internal void EmitLocals(ref XbdmContext context, VariableJson variables, KitMemoryAccess memory)
    {
        if (_pdbBase == 0)
            return;

        SetupContext(ref context);
        var buffer = Marshal.AllocHGlobal(DbgHelpNative.SymbolInfoSize + DbgHelpNative.MaxSymName);
        try
        {
            WriteSymbolHeader(buffer);
            // The frame-type tree is a best-effort enrichment; if SymFromAddr can't resolve the
            // function symbol at EIP (common at a function-entry breakpoint), still fall through to
            // the supplemental RegRel enumeration so locals are not silently dropped.
            if (DbgHelpNative.SymFromAddr(DbgHelpNative.PseudoProcess, PdbAddress(context.Eip), out _, buffer))
            {
                var frameType = (uint)Marshal.ReadInt32(buffer, 4);
                EmitTypeTreeLocals(frameType, ref context, variables, memory, depth: 0);
            }
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }

        EnumSupplementalLocals(ref context, variables, memory);
    }

    internal bool TryEmitMembers(
        string symbolBase,
        ref XbdmContext context,
        KitMemoryAccess memory,
        VariableJson variables)
    {
        if (_pdbBase == 0 || !TryLookupBaseSymbol(symbolBase, ref context, out var typeIndex, out var size, out var flags, out var address))
            return false;

        if (!TryResolveAddress(flags, address, ref context, out var baseAddr))
            return false;

        var symSize = size != 0 ? size : GetTypeByteSize(typeIndex);
        if (symSize == 0)
            symSize = 4;

        var udtType = GetUdtTypeIndex(typeIndex);
        if (EmitStdVectorMembers(udtType, baseAddr, symSize, memory, variables))
            return true;
        if (EmitStdMapMembers(udtType, baseAddr, symSize, memory, variables))
            return true;
        if (EmitRawDwordArray(baseAddr, symSize, memory, variables))
            return variables.Count > 0;
        if (TypeLooksLikeStdAggregate(udtType, symSize))
            return false;

        EmitStructMembers(udtType, baseAddr, baseAddr, memory, variables, depth: 0);
        return variables.Count > 0;
    }

    internal bool TryEvaluate(
        string expression,
        ref XbdmContext context,
        KitMemoryAccess memory,
        out string value,
        out string? error)
    {
        value = string.Empty;
        error = null;
        expression = expression.Trim();

        if (TryParseMemberExpr(expression, out var symbolBase, out var member, out var deref))
        {
            if (!TryLookupBaseSymbol(symbolBase, ref context, out var typeIndex, out var size, out var flags, out var symAddress))
            {
                error = "symbolNotFound";
                return false;
            }

            if (!TryResolveAddress(flags, symAddress, ref context, out var baseAddr))
            {
                error = "readFailed";
                return false;
            }

            if (deref)
            {
                var pointer = memory.ReadDword(baseAddr);
                if (pointer is null)
                {
                    error = "readFailed";
                    return false;
                }

                baseAddr = pointer.Value;
            }

            if (!TryFindMemberOffset(GetUdtTypeIndex(typeIndex), member, out var offset) &&
                !TryFindMemberOffset(typeIndex, member, out offset) &&
                !TryD3dppMemberOffset(member, out offset) &&
                !TryIndexedFloatOffset(size, member, out offset))
            {
                error = "memberNotFound";
                return false;
            }

            var dword = memory.ReadDword((nuint)(baseAddr + offset));
            if (dword is null)
            {
                error = "readFailed";
                return false;
            }

            value = FormatScalar(dword.Value, member, 0);
            return true;
        }

        if (!TryLookupBaseSymbol(expression, ref context, out _, out _, out var exprFlags, out var exprAddress))
        {
            error = "symbolNotFound";
            return false;
        }

        if ((exprFlags & DbgHelpNative.SymflagRegister) != 0)
        {
            var register = (uint)exprAddress;
            value = $"0x{GetRegisterValue(ref context, register):x8}";
            return true;
        }

        if (!TryResolveAddress(exprFlags, exprAddress, ref context, out var runtimeAddr))
        {
            error = "readFailed";
            return false;
        }

        var scalar = memory.ReadDword(runtimeAddr);
        if (scalar is null)
        {
            error = "readFailed";
            return false;
        }

        value = FormatScalar(scalar.Value, expression, 0);
        return true;
    }

    private void SetupContext(ref XbdmContext context)
    {
        var frame = new DbgHelpNative.ImageHlpStackFrame
        {
            InstructionOffset = PdbAddress(context.Eip),
            FrameOffset = context.Ebp,
            StackOffset = context.Esp,
        };
        DbgHelpNative.SymSetContext(DbgHelpNative.PseudoProcess, ref frame, IntPtr.Zero);
    }

    private ulong PdbAddress(nuint runtime) =>
        _moduleBase != 0 && _pdbBase != 0
            ? _pdbBase + (runtime - _moduleBase)
            : runtime;

    private nuint KitAddress(ulong pdbAddress)
    {
        if (_moduleBase == 0 || _pdbBase == 0)
            return (nuint)pdbAddress;
        return _moduleBase + (nuint)(pdbAddress - _pdbBase);
    }

    private void EmitTypeTreeLocals(
        uint typeId,
        ref XbdmContext context,
        VariableJson variables,
        KitMemoryAccess memory,
        int depth)
    {
        if (_pdbBase == 0 || depth > 4 || variables.IsFull)
            return;

        if (TryGetTypeDword(typeId, DbgHelpNative.TiGetSymtag, out var tag) && tag == DbgHelpNative.SymTagData)
        {
            EmitDataTypeLocal(typeId, ref context, variables, memory);
            return;
        }

        if (!TryGetTypeDword(typeId, DbgHelpNative.TiGetChildrencount, out var childCount) || childCount == 0)
            return;

        using var children = TypeChildren.Load(_pdbBase, typeId, childCount);
        if (children is null)
            return;

        foreach (var childId in children.ChildIds)
        {
            if (variables.IsFull)
                break;
            if (!TryGetTypeDword(childId, DbgHelpNative.TiGetSymtag, out var childTag))
                continue;

            switch (childTag)
            {
                case DbgHelpNative.SymTagData:
                    EmitDataTypeLocal(childId, ref context, variables, memory);
                    break;
                case DbgHelpNative.SymTagArrayType:
                    EmitArrayTypeLocal(childId, variables);
                    break;
                case DbgHelpNative.SymTagBlock:
                case DbgHelpNative.SymTagFunction:
                    EmitTypeTreeLocals(childId, ref context, variables, memory, depth + 1);
                    break;
            }
        }
    }

    private void EmitDataTypeLocal(uint typeId, ref XbdmContext context, VariableJson variables, KitMemoryAccess memory)
    {
        if (variables.IsFull || !TryGetTypeName(typeId, out var name) || IsCompilerGenerated(name))
            return;

        TryGetTypeDword(typeId, DbgHelpNative.TiGetOffset, out var offset);
        TryGetTypeDword(typeId, DbgHelpNative.TiGetLength, out var length);
        if (length == 0)
            length = 4;

        TryGetTypeDword(typeId, DbgHelpNative.TiGetType, out var fieldType);
        var addr = (nuint)((long)context.Ebp + (int)offset);
        if (length > 4)
        {
            variables.Append(name, FormatAggregateSummary(fieldType != 0 ? fieldType : typeId, addr, length, memory), expandable: true, expandBase: name);
            return;
        }

        var dword = memory.ReadDword(addr);
        if (dword is null)
            return;
        variables.Append(name, FormatScalar(dword.Value, name, fieldType));
    }

    private void EmitArrayTypeLocal(uint typeId, VariableJson variables)
    {
        if (variables.IsFull || !TryGetTypeName(typeId, out var name) || IsCompilerGenerated(name))
            return;
        if (!TryReadArrayLayout(typeId, out var elemCount, out _))
            return;
        variables.Append(name, $"array[{elemCount}]", expandable: true, expandBase: name);
    }

    internal static bool LocalsDiagnostics = true;

    private void EnumSupplementalLocals(ref XbdmContext context, VariableJson variables, KitMemoryAccess memory)
    {
        SetupContext(ref context);

        if (LocalsDiagnostics)
        {
            var diagBuf = DiagSymBuffer();
            var ok = DbgHelpNative.SymFromAddr(DbgHelpNative.PseudoProcess, PdbAddress(context.Eip), out _, diagBuf);
            var diagName84 = ok ? Marshal.PtrToStringAnsi(diagBuf + 84) : null;
            var diagName88 = ok ? Marshal.PtrToStringAnsi(diagBuf + 88) : null;
            BridgeWriter.Log(
                $"locals-diag: eip=0x{context.Eip:x} pdbEip=0x{PdbAddress(context.Eip):x} ebp=0x{context.Ebp:x} " +
                $"symFromAddr={ok} name84='{diagName84}' name88='{diagName88}'");
        }

        var state = new LocalsEnumState(variables, context, memory, this);
        var handle = GCHandle.Alloc(state);
        try
        {
            DbgHelpNative.SymEnumSymbols(
                DbgHelpNative.PseudoProcess,
                0,
                null,
                EnumSupplementalLocalsCallback,
                GCHandle.ToIntPtr(handle));
        }
        finally
        {
            handle.Free();
        }

        if (LocalsDiagnostics)
            BridgeWriter.Log($"locals-diag: callback fired {state.Examined} time(s), emitted {variables.Count}");
    }

    private static IntPtr _diagSymBuffer;
    private static IntPtr DiagSymBuffer()
    {
        if (_diagSymBuffer == IntPtr.Zero)
            _diagSymBuffer = Marshal.AllocHGlobal(DbgHelpNative.SymbolInfoSize + DbgHelpNative.MaxSymName);
        Marshal.WriteInt32(_diagSymBuffer, 0, DbgHelpNative.SymbolInfoSize);
        Marshal.WriteInt32(_diagSymBuffer, 80, DbgHelpNative.MaxSymName);
        return _diagSymBuffer;
    }

    private static readonly DbgHelpNative.SymEnumSymbolsCallback EnumSupplementalLocalsCallback = static (symbolInfo, _, userContext) =>
    {
        var state = GCHandle.FromIntPtr(userContext).Target as LocalsEnumState;
        if (state is null)
            return true;

        if (state.Variables.IsFull || ++state.Examined > 128)
            return false;

        if (LocalsDiagnostics && state.Examined <= 8)
        {
            var n84 = Marshal.PtrToStringAnsi(symbolInfo + 84) ?? string.Empty;
            var n88 = Marshal.PtrToStringAnsi(symbolInfo + 88) ?? string.Empty;
            BridgeWriter.Log(
                $"locals-diag[{state.Examined}]: name84='{n84}' name88='{n88}' " +
                $"u40=0x{(uint)Marshal.ReadInt32(symbolInfo, 40):x} u56=0x{(uint)Marshal.ReadInt32(symbolInfo, 56):x} " +
                $"u68=0x{(uint)Marshal.ReadInt32(symbolInfo, 68):x} u72=0x{(uint)Marshal.ReadInt32(symbolInfo, 72):x} " +
                $"v48=0x{Marshal.ReadInt64(symbolInfo, 48):x} v52=0x{Marshal.ReadInt64(symbolInfo, 52):x} v56=0x{Marshal.ReadInt64(symbolInfo, 56):x}");
        }

        var flags = (uint)Marshal.ReadInt32(symbolInfo, DbgHelpNative.SymInfoFlags);
        if ((flags & DbgHelpNative.SymflagRegrel) == 0)
            return true;

        var tag = (uint)Marshal.ReadInt32(symbolInfo, DbgHelpNative.SymInfoTag);
        if (tag != DbgHelpNative.SymTagData)
            return true;

        var name = Marshal.PtrToStringAnsi(symbolInfo + DbgHelpNative.SymInfoName) ?? string.Empty;
        if (IsCompilerGenerated(name) || state.Variables.WasEmitted(name))
            return true;

        var address = (long)Marshal.ReadInt64(symbolInfo, DbgHelpNative.SymInfoAddress);
        var runtimeAddr = (nuint)((long)state.Context.Ebp + address);

        // dbghelp cannot describe the modern S_LOCAL / S_DEFRANGE_FRAMEPOINTER_REL locals that
        // Zig/LLVM emit: it hands back the def-range's *code span* as SYMBOL_INFO.Size and a
        // garbage type index (a 4-byte HRESULT comes back as size=256 -> "array[64]"). The
        // frame-relative address is still correct, though. Anything reaching this supplemental
        // fallback had no usable type from the frame-type tree (which handles dbghelp-friendly
        // PDBs with real aggregate expansion), so render it as a scalar at its frame address
        // instead of trusting the bogus size/type. Pass typeId=0 so float formatting relies on
        // the name heuristic rather than the unreliable index.
        var dword = state.Memory.ReadDword(runtimeAddr);
        if (dword is null)
            return true;
        state.Variables.Append(name, state.Engine.FormatScalar(dword.Value, name, 0));
        return true;
    };

    private bool TryLookupBaseSymbol(
        string name,
        ref XbdmContext context,
        out uint typeIndex,
        out uint size,
        out uint flags,
        out long address)
    {
        typeIndex = 0;
        size = 0;
        flags = 0;
        address = 0;

        if (_pdbBase == 0)
            return TrySymFromName(name, out typeIndex, out size, out flags, out address);

        SetupContext(ref context);
        var frameBuffer = Marshal.AllocHGlobal(DbgHelpNative.SymbolInfoSize + DbgHelpNative.MaxSymName);
        try
        {
            WriteSymbolHeader(frameBuffer);
            if (DbgHelpNative.SymFromAddr(DbgHelpNative.PseudoProcess, PdbAddress(context.Eip), out _, frameBuffer))
            {
                var frameType = (uint)Marshal.ReadInt32(frameBuffer, 4);
                if (FindFrameLocalByName(frameType, name, out typeIndex, out var offset, out size))
                {
                    flags = DbgHelpNative.SymflagRegrel;
                    address = offset;
                    return true;
                }
            }
        }
        finally
        {
            Marshal.FreeHGlobal(frameBuffer);
        }

        SetupContext(ref context);
        return TrySymFromName(name, out typeIndex, out size, out flags, out address);
    }

    private bool TrySymFromName(string name, out uint typeIndex, out uint size, out uint flags, out long address)
    {
        typeIndex = 0;
        size = 0;
        flags = 0;
        address = 0;
        var buffer = Marshal.AllocHGlobal(DbgHelpNative.SymbolInfoSize + DbgHelpNative.MaxSymName);
        try
        {
            WriteSymbolHeader(buffer);
            if (!DbgHelpNative.SymFromName(DbgHelpNative.PseudoProcess, name, buffer))
                return false;
            typeIndex = (uint)Marshal.ReadInt32(buffer, DbgHelpNative.SymInfoTypeIndex);
            size = (uint)Marshal.ReadInt32(buffer, DbgHelpNative.SymInfoSize);
            flags = (uint)Marshal.ReadInt32(buffer, DbgHelpNative.SymInfoFlags);
            if ((flags & DbgHelpNative.SymflagRegister) != 0)
                address = Marshal.ReadInt32(buffer, DbgHelpNative.SymInfoRegister);
            else
                address = Marshal.ReadInt64(buffer, DbgHelpNative.SymInfoAddress);
            return true;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    private bool TryResolveAddress(uint flags, long symAddress, ref XbdmContext context, out nuint runtimeAddr)
    {
        runtimeAddr = 0;
        if ((flags & DbgHelpNative.SymflagRegister) != 0)
        {
            runtimeAddr = GetRegisterValue(ref context, (uint)symAddress);
            return true;
        }

        if ((flags & DbgHelpNative.SymflagRegrel) != 0)
        {
            runtimeAddr = (nuint)((long)context.Ebp + symAddress);
            return true;
        }

        runtimeAddr = KitAddress((ulong)symAddress);
        return runtimeAddr != 0;
    }

    private bool FindFrameLocalByName(
        uint typeId,
        string name,
        out uint typeIndex,
        out uint offset,
        out uint length,
        int depth = 0)
    {
        typeIndex = 0;
        offset = 0;
        length = 0;
        if (depth > 6 || _pdbBase == 0)
            return false;

        if (TryGetTypeDword(typeId, DbgHelpNative.TiGetSymtag, out var tag) &&
            (tag == DbgHelpNative.SymTagData || tag == DbgHelpNative.SymTagArrayType))
        {
            if (TryGetTypeName(typeId, out var childName) &&
                string.Equals(childName, name, StringComparison.OrdinalIgnoreCase))
            {
                TryGetTypeDword(typeId, DbgHelpNative.TiGetOffset, out offset);
                TryGetTypeDword(typeId, DbgHelpNative.TiGetLength, out length);
                if (length == 0)
                    length = 4;
                TryGetTypeDword(typeId, DbgHelpNative.TiGetType, out var fieldType);
                typeIndex = tag == DbgHelpNative.SymTagArrayType ? typeId : fieldType != 0 ? fieldType : typeId;
                return true;
            }
        }

        if (!TryGetTypeDword(typeId, DbgHelpNative.TiGetChildrencount, out var childCount) || childCount == 0)
            return false;

        using var children = TypeChildren.Load(_pdbBase, typeId, childCount);
        if (children is null)
            return false;

        foreach (var childId in children.ChildIds)
        {
            if (!TryGetTypeDword(childId, DbgHelpNative.TiGetSymtag, out var childTag))
                continue;
            if (childTag is DbgHelpNative.SymTagData or DbgHelpNative.SymTagArrayType or DbgHelpNative.SymTagBlock or DbgHelpNative.SymTagFunction)
            {
                if (FindFrameLocalByName(childId, name, out typeIndex, out offset, out length, depth + 1))
                    return true;
            }
        }

        return false;
    }

    private void EmitStructMembers(
        uint typeId,
        nuint structAddr,
        nuint rootAddr,
        KitMemoryAccess memory,
        VariableJson variables,
        int depth)
    {
        if (_pdbBase == 0 || depth > 4 || variables.IsFull)
            return;

        if (!TryGetTypeDword(typeId, DbgHelpNative.TiGetChildrencount, out var childCount) || childCount == 0)
            return;

        using var children = TypeChildren.Load(_pdbBase, typeId, childCount);
        if (children is null)
            return;

        uint layoutOff = 0;
        foreach (var childId in children.ChildIds)
        {
            if (variables.IsFull)
                break;
            if (!TryGetTypeDword(childId, DbgHelpNative.TiGetSymtag, out var childTag))
                continue;
            if (!TryGetTypeDword(childId, DbgHelpNative.TiGetOffset, out var offset))
                offset = layoutOff;

            if (childTag == DbgHelpNative.SymTagData)
            {
                if (!TryGetTypeName(childId, out var childName) || IsCompilerGenerated(childName))
                    continue;
                TryGetTypeDword(childId, DbgHelpNative.TiGetLength, out var length);
                TryGetTypeDword(childId, DbgHelpNative.TiGetType, out var fieldType);
                if (length == 0)
                    length = 4;
                if (length == 4)
                {
                    var dword = memory.ReadDword(structAddr + offset);
                    if (dword is not null)
                        variables.Append(childName, FormatScalar(dword.Value, childName, fieldType));
                }
                else
                {
                    variables.Append(childName, $"{{{length} bytes}}", expandable: true, expandBase: childName);
                }

                layoutOff = offset + length;
            }
            else if (childTag is DbgHelpNative.SymTagUdt or DbgHelpNative.SymTagBlock)
            {
                TryGetTypeDword(childId, DbgHelpNative.TiGetType, out var nestedType);
                if (nestedType == 0)
                    nestedType = childId;
                EmitStructMembers(GetUdtTypeIndex(nestedType), structAddr + offset, rootAddr, memory, variables, depth + 1);
                var nestedSize = GetTypeByteSize(nestedType);
                if (nestedSize != 0)
                    layoutOff = offset + nestedSize;
            }
        }
    }

    private bool EmitRawDwordArray(nuint baseAddr, uint byteSize, KitMemoryAccess memory, VariableJson variables)
    {
        if (byteSize < 4 || byteSize % 4 != 0 || byteSize > 1024)
            return false;

        var count = byteSize / 4;
        variables.Append("size", count.ToString());
        for (uint i = 0; i < count && !variables.IsFull; i++)
        {
            var dword = memory.ReadDword(baseAddr + i * 4);
            if (dword is null)
                continue;
            variables.Append($"[{i}]", FormatScalar(dword.Value, $"[{i}]", 0));
        }

        return variables.Count > 0;
    }

    private bool TryReadArrayLayout(uint typeId, out uint elemCount, out uint elemSize)
    {
        elemCount = 0;
        elemSize = 0;
        typeId = ResolveArrayTypeId(typeId);
        if (typeId == 0)
            return false;
        if (!TryGetTypeDword(typeId, DbgHelpNative.TiGetCount, out elemCount) || elemCount == 0)
            return false;
        if (!TryGetTypeDword(typeId, DbgHelpNative.TiGetType, out var elemType) || elemType == 0)
            return false;
        if (!TryGetTypeDword(elemType, DbgHelpNative.TiGetLength, out elemSize) || elemSize == 0)
            elemSize = 4;
        return elemCount <= 256;
    }

    private uint ResolveArrayTypeId(uint typeId)
    {
        if (TryGetTypeDword(typeId, DbgHelpNative.TiGetSymtag, out var tag) && tag == DbgHelpNative.SymTagArrayType)
            return typeId;
        if (TryGetTypeDword(typeId, DbgHelpNative.TiGetType, out var fieldType) &&
            TryGetTypeDword(fieldType, DbgHelpNative.TiGetSymtag, out tag) &&
            tag == DbgHelpNative.SymTagArrayType)
            return fieldType;
        return 0;
    }

    private uint GetUdtTypeIndex(uint typeIndex)
    {
        if (TryGetTypeDword(typeIndex, DbgHelpNative.TiGetType, out var udtType) && udtType != 0)
            return udtType;
        return typeIndex;
    }

    private uint GetTypeByteSize(uint typeId)
    {
        if (TryGetTypeDword(GetUdtTypeIndex(typeId), DbgHelpNative.TiGetLength, out var length))
            return length;
        return 0;
    }

    private bool TryFindMemberOffset(uint typeIndex, string member, out uint offset) =>
        FindMemberOffsetInType(typeIndex, member, 0, out offset, depth: 0);

    private bool FindMemberOffsetInType(uint typeIndex, string member, uint baseOff, out uint offset, int depth)
    {
        offset = 0;
        if (depth > 6 || _pdbBase == 0)
            return false;

        if (!TryGetTypeDword(typeIndex, DbgHelpNative.TiGetChildrencount, out var childCount) || childCount == 0)
            return false;

        using var children = TypeChildren.Load(_pdbBase, typeIndex, childCount);
        if (children is null)
            return false;

        uint layoutOff = 0;
        foreach (var childId in children.ChildIds)
        {
            if (!TryGetTypeDword(childId, DbgHelpNative.TiGetSymtag, out var childTag))
                continue;
            if (!TryGetTypeDword(childId, DbgHelpNative.TiGetOffset, out var childOffset))
                childOffset = layoutOff;

            if (childTag == DbgHelpNative.SymTagData)
            {
                if (TryGetTypeName(childId, out var childName) &&
                    string.Equals(childName, member, StringComparison.OrdinalIgnoreCase))
                {
                    offset = baseOff + childOffset;
                    return true;
                }

                TryGetTypeDword(childId, DbgHelpNative.TiGetLength, out var length);
                if (length == 0)
                    length = 4;
                layoutOff = childOffset + length;
            }
            else
            {
                TryGetTypeDword(childId, DbgHelpNative.TiGetType, out var fieldType);
                var nestedType = fieldType != 0 ? fieldType : childId;
                if (FindMemberOffsetInType(GetUdtTypeIndex(nestedType), member, baseOff + childOffset, out offset, depth + 1))
                    return true;
                var nestedSize = GetTypeByteSize(nestedType);
                if (nestedSize != 0)
                    layoutOff = childOffset + nestedSize;
            }
        }

        return false;
    }

    private bool TryGetTypeDword(uint typeId, uint getType, out uint value)
    {
        value = 0;
        if (_pdbBase == 0)
            return false;
        var ptr = Marshal.AllocHGlobal(4);
        try
        {
            if (!DbgHelpNative.SymGetTypeInfo(DbgHelpNative.PseudoProcess, _pdbBase, typeId, getType, ptr))
                return false;
            value = (uint)Marshal.ReadInt32(ptr);
            return true;
        }
        finally
        {
            Marshal.FreeHGlobal(ptr);
        }
    }

    private bool TryGetTypeName(uint typeId, out string name)
    {
        name = string.Empty;
        if (_pdbBase == 0)
            return false;

        var outPtr = Marshal.AllocHGlobal(IntPtr.Size);
        try
        {
            if (!DbgHelpNative.SymGetTypeInfo(DbgHelpNative.PseudoProcess, _pdbBase, typeId, DbgHelpNative.TiGetSymname, outPtr))
                return false;
            var wstrPtr = Marshal.ReadIntPtr(outPtr);
            if (wstrPtr == IntPtr.Zero)
                return false;
            // SymGetTypeInfo can return a pointer that is not a safely NUL-terminated string for our
            // Zig-produced PDBs; a plain PtrToStringUni would scan off the mapping and raise an
            // (uncatchable) AccessViolationException that kills the bridge. Read it defensively.
            return NativeStringReader.TryReadWideString(wstrPtr, 512, out name);
        }
        finally
        {
            Marshal.FreeHGlobal(outPtr);
        }
    }

    private static void WriteSymbolHeader(IntPtr buffer)
    {
        Marshal.WriteInt32(buffer, 0, DbgHelpNative.SymbolInfoSize);
        // MaxNameLen lives at offset 80 (matches the working TrySymFromAddr path in SymbolService);
        // writing it at the wrong slot makes dbghelp think the name buffer is empty and skip names.
        Marshal.WriteInt32(buffer, 80, DbgHelpNative.MaxSymName);
    }

    private static bool IsCompilerGenerated(string name) =>
        name.StartsWith("__", StringComparison.Ordinal);

    private static nuint GetRegisterValue(ref XbdmContext context, uint reg) =>
        reg switch
        {
            0 => context.Eax,
            1 => context.Ecx,
            2 => context.Edx,
            3 => context.Ebx,
            4 => context.Esp,
            5 => context.Ebp,
            6 => context.Esi,
            7 => context.Edi,
            _ => 0,
        };

    private static bool TryParseMemberExpr(string expression, out string symbolBase, out string member, out bool deref)
    {
        symbolBase = string.Empty;
        member = string.Empty;
        deref = false;
        expression = expression.Trim();

        var dot = expression.LastIndexOf('.');
        var arrow = expression.LastIndexOf("->", StringComparison.Ordinal);
        int sepIndex;
        int sepLength;
        if (dot >= 0 && arrow >= 0)
        {
            if (dot > arrow)
            {
                sepIndex = dot;
                sepLength = 1;
            }
            else
            {
                sepIndex = arrow;
                sepLength = 2;
                deref = true;
            }
        }
        else if (dot >= 0)
        {
            sepIndex = dot;
            sepLength = 1;
        }
        else if (arrow >= 0)
        {
            sepIndex = arrow;
            sepLength = 2;
            deref = true;
        }
        else
        {
            return false;
        }

        symbolBase = expression[..sepIndex].Trim();
        member = expression[(sepIndex + sepLength)..].Trim();
        return symbolBase.Length > 0 && member.Length > 0;
    }

    private static bool TryD3dppMemberOffset(string member, out uint offset)
    {
        (string name, uint off)[] members =
        [
            ("BackBufferWidth", 0), ("BackBufferHeight", 4), ("BackBufferFormat", 8),
            ("BackBufferCount", 12), ("MultiSampleType", 16), ("SwapEffect", 20),
            ("hDeviceWindow", 24), ("Windowed", 28), ("EnableAutoDepthStencil", 32),
            ("AutoDepthStencilFormat", 36), ("Flags", 40), ("FullScreen_RefreshRateInHz", 44),
            ("FullScreen_PresentationInterval", 48),
        ];

        foreach (var (name, off) in members)
        {
            if (string.Equals(member, name, StringComparison.OrdinalIgnoreCase))
            {
                offset = off;
                return true;
            }
        }

        offset = 0;
        return false;
    }

    private static bool TryIndexedFloatOffset(uint totalSize, string member, out uint offset)
    {
        offset = 0;
        if (member.Length < 2 || member[0] != 'f' || member[1] < '0' || member[1] > '9')
            return false;
        if (!uint.TryParse(member[1..], out var slot))
            return false;
        if (slot * 4 + 4 > totalSize)
            return false;
        offset = slot * 4;
        return true;
    }

    private sealed class TypeChildren : IDisposable
    {
        private readonly IntPtr _memory;

        internal uint[] ChildIds { get; }

        private TypeChildren(IntPtr memory, uint[] childIds)
        {
            _memory = memory;
            ChildIds = childIds;
        }

        internal static TypeChildren? Load(ulong pdbBase, uint typeId, uint childCount)
        {
            var headerSize = Marshal.SizeOf<DbgHelpNative.TiFindChildrenParams>();
            var memory = Marshal.AllocHGlobal(headerSize + (int)childCount * 4);
            try
            {
                Marshal.WriteInt32(memory, 0, (int)childCount);
                Marshal.WriteInt32(memory, 4, 0);
                if (!DbgHelpNative.SymGetTypeInfo(
                        DbgHelpNative.PseudoProcess,
                        pdbBase,
                        typeId,
                        DbgHelpNative.TiFindchildren,
                        memory))
                {
                    Marshal.FreeHGlobal(memory);
                    return null;
                }

                var ids = new uint[childCount];
                for (var i = 0; i < childCount; i++)
                    ids[i] = (uint)Marshal.ReadInt32(memory, headerSize + i * 4);
                return new TypeChildren(memory, ids);
            }
            catch
            {
                Marshal.FreeHGlobal(memory);
                throw;
            }
        }

        public void Dispose() => Marshal.FreeHGlobal(_memory);
    }

    private sealed class LocalsEnumState
    {
        internal LocalsEnumState(VariableJson variables, XbdmContext context, KitMemoryAccess memory, SymbolTypeEngine engine)
        {
            Variables = variables;
            Context = context;
            Memory = memory;
            Engine = engine;
        }

        internal VariableJson Variables { get; }
        internal XbdmContext Context { get; }
        internal KitMemoryAccess Memory { get; }
        internal SymbolTypeEngine Engine { get; }
        internal int Examined { get; set; }
    }
}
