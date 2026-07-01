using Rxdk.Pdb;
using Rxdk.Pdb.Symbols;
using Rxdk.Pdb.Tpi;
using Rxdk.Xbdm;

namespace Rxdk.XboxDbgBridge.Symbols;

/// <summary>
/// Locals emission backed by the pure-managed <see cref="PdbImage"/> reader instead of dbghelp.
/// dbghelp cannot interpret the modern S_LOCAL / S_DEFRANGE_FRAMEPOINTER_REL records that Zig/LLVM
/// emit (it reports a def-range's code span as the variable size, so a 4-byte HRESULT shows as
/// array[64]); the managed reader resolves the real type/size/frame-offset. Falls back to dbghelp
/// via <see cref="SymbolService"/> only when this path yields nothing.
/// </summary>
internal sealed class ManagedSymbols
{
    private readonly PdbImage _pdb;
    private readonly nuint _moduleBase;

    internal ManagedSymbols(PdbImage pdb, nuint moduleBase)
    {
        _pdb = pdb;
        _moduleBase = moduleBase;
    }

    /// <summary>Emits the current frame's locals; returns false if no frame was found at EIP.</summary>
    internal bool EmitLocals(ref XbdmContext context, VariableJson variables, KitMemoryAccess memory)
    {
        var frame = FindFrame(context.Eip);
        if (frame is null)
            return false;

        var emitted = false;
        foreach (var local in frame.Locals)
        {
            if (variables.IsFull)
                break;
            if (IsHidden(local.Name) || variables.WasEmitted(local.Name))
                continue;

            var address = (nuint)((long)context.Ebp + local.FrameOffset);
            EmitValue(local.Name, local.TypeIndex, address, memory, variables, expandBase: local.Name);
            emitted = true;
        }

        return emitted;
    }

    /// <summary>Expands one aggregate local (array elements or struct members) by name.</summary>
    internal bool TryEmitMembers(string baseName, ref XbdmContext context, VariableJson variables, KitMemoryAccess memory)
    {
        var frame = FindFrame(context.Eip);
        var local = frame?.Locals.FirstOrDefault(l => l.Name == baseName);
        if (local is null)
            return false;

        var address = (nuint)((long)context.Ebp + local.FrameOffset);
        EmitChildren(local.TypeIndex, address, memory, variables);
        return variables.Count > 0;
    }

    private FrameInfo? FindFrame(uint eip)
    {
        if (_moduleBase == 0 || eip < _moduleBase)
            return null;
        return _pdb.FindFrame(eip - (uint)_moduleBase); // RVA = EIP - module base (image base == PDB base)
    }

    private void EmitValue(string name, uint typeIndex, nuint address, KitMemoryAccess memory, VariableJson variables, string expandBase)
    {
        var type = _pdb.Types.Resolve(typeIndex);
        switch (type.Kind)
        {
            case PdbTypeKind.Array:
            {
                var elem = type.ReferentType != 0 ? _pdb.Types.Resolve(type.ReferentType) : null;
                var label = elem?.Name is { Length: > 0 } en ? $"{en}[{type.ElementCount}]" : $"array[{type.ElementCount}]";
                variables.Append(name, label, expandable: type.ElementCount > 0, expandBase: expandBase);
                break;
            }

            case PdbTypeKind.Struct:
            case PdbTypeKind.Class:
            case PdbTypeKind.Union:
            {
                var label = type.Name is { Length: > 0 } tn ? tn : $"{{{type.ByteSize} bytes}}";
                variables.Append(name, label, expandable: type.Members.Count > 0, expandBase: expandBase);
                break;
            }

            default:
                variables.Append(name, FormatScalar(type, address, memory));
                break;
        }
    }

    private void EmitChildren(uint typeIndex, nuint address, KitMemoryAccess memory, VariableJson variables)
    {
        var type = _pdb.Types.Resolve(typeIndex);

        if (type.Kind == PdbTypeKind.Array && type.ElementCount > 0)
        {
            var elem = _pdb.Types.Resolve(type.ReferentType);
            var elemSize = elem.ByteSize == 0 ? 4u : elem.ByteSize;
            var count = Math.Min(type.ElementCount, 256u);
            for (var i = 0u; i < count && !variables.IsFull; i++)
                EmitValue($"[{i}]", type.ReferentType, address + (nuint)(i * elemSize), memory, variables, expandBase: $"[{i}]");
            return;
        }

        if (type.IsAggregate)
        {
            foreach (var member in type.Members)
            {
                if (variables.IsFull)
                    break;
                EmitValue(member.Name, member.TypeIndex, address + (nuint)member.Offset, memory, variables, expandBase: member.Name);
            }
        }
    }

    private static string FormatScalar(PdbType type, nuint address, KitMemoryAccess memory)
    {
        var low = memory.ReadDword(address);
        if (low is null)
            return "<unreadable>";
        var value = low.Value;

        if (type.IsFloatingPoint && type.ByteSize == 8)
        {
            var high = memory.ReadDword(address + 4) ?? 0;
            var bits = ((ulong)high << 32) | value;
            return $"{BitConverter.Int64BitsToDouble((long)bits):g}";
        }

        if (type.IsFloatingPoint)
            return $"{BitConverter.Int32BitsToSingle((int)value):g} (0x{value:x8})";

        if (type.Kind == PdbTypeKind.Pointer)
            return $"0x{value:x8}";

        if (type.ByteSize == 8)
        {
            var high = memory.ReadDword(address + 4) ?? 0;
            return $"0x{high:x8}{value:x8}";
        }

        value = type.ByteSize switch
        {
            1 => value & 0xFF,
            2 => value & 0xFFFF,
            _ => value,
        };

        var isUnsigned = type.Name is not null && type.Name.Contains("unsigned", StringComparison.Ordinal);
        return isUnsigned ? $"{value} (0x{value:x8})" : $"{(int)value} (0x{value:x8})";
    }

    // Skip compiler-generated / mangled helper locals to match the dbghelp path's filtering.
    private static bool IsHidden(string name) =>
        string.IsNullOrEmpty(name) || name.StartsWith("__", StringComparison.Ordinal);
}
