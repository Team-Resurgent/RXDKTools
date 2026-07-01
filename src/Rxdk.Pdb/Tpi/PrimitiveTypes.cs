namespace Rxdk.Pdb.Tpi;

/// <summary>
/// Resolves CodeView primitive type indices (values below TypeIndexBegin, typically &lt; 0x1000).
/// These are not stored as records; their meaning is encoded in the index bits. A non-zero pointer
/// "mode" (bits 8-10) makes the type a pointer to the underlying primitive.
/// </summary>
internal static class PrimitiveTypes
{
    internal readonly record struct Info(string Name, uint ByteSize, bool IsFloat);

    internal static bool TryResolve(uint index, out Info info)
    {
        info = default;
        if (index == 0)
            return false;

        var mode = (index >> 8) & 0x7;   // CV_TM_* : 0 direct, 4 near32 ptr, 6 near64 ptr
        var baseByte = index & 0xFF;

        if (!TryBase(baseByte, out var baseInfo))
            return false;

        if (mode != 0)
        {
            var size = mode == 6 ? 8u : 4u; // 32-bit Xbox uses near32 pointers (4 bytes)
            info = new Info(baseInfo.Name + "*", size, false);
            return true;
        }

        info = baseInfo;
        return true;
    }

    private static bool TryBase(uint b, out Info info)
    {
        info = b switch
        {
            0x00 => new Info("<notype>", 0, false),
            0x03 => new Info("void", 0, false),
            0x08 => new Info("HRESULT", 4, false),

            0x10 => new Info("signed char", 1, false),
            0x11 => new Info("short", 2, false),
            0x12 => new Info("long", 4, false),
            0x13 => new Info("__int64", 8, false),

            0x20 => new Info("unsigned char", 1, false),
            0x21 => new Info("unsigned short", 2, false),
            0x22 => new Info("unsigned long", 4, false),
            0x23 => new Info("unsigned __int64", 8, false),

            0x30 => new Info("bool", 1, false),

            0x40 => new Info("float", 4, true),
            0x41 => new Info("double", 8, true),
            0x42 => new Info("long double", 10, true),

            0x68 => new Info("__int8", 1, false),
            0x69 => new Info("unsigned __int8", 1, false),
            0x70 => new Info("char", 1, false),
            0x71 => new Info("wchar_t", 2, false),
            0x72 => new Info("__int16", 2, false),
            0x73 => new Info("unsigned __int16", 2, false),
            0x74 => new Info("int", 4, false),
            0x75 => new Info("unsigned int", 4, false),
            0x76 => new Info("__int64", 8, false),
            0x77 => new Info("unsigned __int64", 8, false),

            _ => default,
        };

        return info.Name is not null;
    }
}
