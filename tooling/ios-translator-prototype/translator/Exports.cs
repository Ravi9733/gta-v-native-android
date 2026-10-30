using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

static class MemoryFiles
{
    static readonly Dictionary<string, byte[]> Files = new(StringComparer.Ordinal);
    internal static void Reset() => Files.Clear();
    internal static void CreateDirectory(string path) { }
    internal static byte[] ReadAllBytes(string path) => Files.TryGetValue(path, out byte[] data) ? data : throw new InvalidOperationException("unavailable in-memory path");
    internal static string ReadAllText(string path) => Encoding.UTF8.GetString(ReadAllBytes(path));
    internal static void WriteAllBytes(string path, byte[] data) => Files[path] = data;
    internal static void WriteAllText(string path, string text) => Files[path] = Encoding.UTF8.GetBytes(text);
    internal static void Move(string source, string destination, bool overwrite)
    {
        if (!overwrite && Files.ContainsKey(destination)) throw new InvalidOperationException("in-memory destination exists");
        Files[destination] = ReadAllBytes(source); Files.Remove(source);
    }
}

static unsafe class NativeCross
{
    internal static delegate* unmanaged[Cdecl]<uint*, nuint, int, byte**, byte**, int> Cross;
    internal static delegate* unmanaged[Cdecl]<void*, void> Free;
    internal static string ToMsl(byte[] spirv)
    {
        if (Cross == null || Free == null) throw new InvalidOperationException("SPIRV-Cross backend not registered");
        if (spirv.Length < 20 || (spirv.Length & 3) != 0) throw new InvalidOperationException("invalid SPIR-V size");
        byte* result = null; byte* error = null;
        try
        {
            int status; fixed (byte* input = spirv) status = Cross((uint*)input, (nuint)(spirv.Length / 4), 0, &result, &error);
            if (status != 0 || result == null) throw new InvalidOperationException(error == null ? "SPIRV-Cross failed" : Marshal.PtrToStringUTF8((nint)error));
            return Marshal.PtrToStringUTF8((nint)result);
        }
        finally { if (result != null) Free(result); if (error != null) Free(error); }
    }
}

static partial class ShaderService
{
    internal static JsonObject TranslateInMemory(JsonObject request, byte[] code, int debug)
    {
        string variant = (string)request["variant"];
        if (variant == null || !Regex.IsMatch(variant, @"\A[0-9a-f]{64}-[0-9a-f]{12}\z")) throw new ArgumentException("invalid GTA variant identity");
        request["code"] = "/input/code.bin";
        MemoryFiles.WriteAllBytes("/input/code.bin", code);
        outDir = "/output"; chain = "totk-native-v4-unverified"; debugMode = debug;
        Handle(request);
        return new JsonObject {
            ["ok"] = true, ["chain"] = chain, ["variant"] = variant,
            ["msl"] = MemoryFiles.ReadAllText(Path.Combine(outDir, variant + "_b.metal")),
            ["reflection_json"] = MemoryFiles.ReadAllText(Path.Combine(outDir, variant + "_b.json")),
            ["spirv_base64"] = Convert.ToBase64String(MemoryFiles.ReadAllBytes(Path.Combine(outDir, variant + ".spv"))),
            ["resource_json"] = MemoryFiles.ReadAllText(Path.Combine(outDir, variant + ".res.json"))
        };
    }
}

public static unsafe class GtaShaderExports
{
    static readonly object Gate = new();
    const int RequestLimit = 1 << 20, CodeLimit = 1 << 20, OutputLimit = 16 << 20;

    [UnmanagedCallersOnly(EntryPoint = "gta_shader_set_cross_backend", CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int SetCross(delegate* unmanaged[Cdecl]<uint*, nuint, int, byte**, byte**, int> cross,
                               delegate* unmanaged[Cdecl]<void*, void> free)
    {
        if (cross == null || free == null) return -1;
        lock (Gate) { NativeCross.Cross = cross; NativeCross.Free = free; }
        return 0;
    }

    // 0 = successful translation; 1 = JSON error response; negative = invalid
    // ABI/serialization failure. Every non-null result belongs to this library.
    // The input request's code path is ignored: only caller-supplied code bytes
    // are available through the isolated in-memory filesystem for this call.
    [UnmanagedCallersOnly(EntryPoint = "gta_shader_translate_json", CallConvs = new[] { typeof(CallConvCdecl) })]
    public static int Translate(byte* request, nuint requestLength, byte* code, nuint codeLength, int debugMode,
                                byte** output, nuint* outputLength)
    {
        if (output == null || outputLength == null) return -1;
        *output = null; *outputLength = 0;
        if (request == null || requestLength == 0 || requestLength > RequestLimit ||
            code == null || codeLength == 0 || codeLength > CodeLimit || (debugMode != 0 && debugMode != 1)) return -2;
        lock (Gate)
        {
            int status = 0; JsonObject reply;
            MemoryFiles.Reset();
            try
            {
                var rq = JsonNode.Parse(new ReadOnlySpan<byte>(request, (int)requestLength)).AsObject();
                reply = ShaderService.TranslateInMemory(rq, new ReadOnlySpan<byte>(code, (int)codeLength).ToArray(), debugMode);
            }
            catch (Exception e) { status = 1; reply = new JsonObject { ["ok"] = false, ["error"] = e.GetType().Name + ": " + e.Message }; }
            finally { MemoryFiles.Reset(); }
            try
            {
                byte[] bytes = Encoding.UTF8.GetBytes(reply.ToJsonString());
                if (bytes.Length > OutputLimit) return -3;
                byte* owned = (byte*)NativeMemory.Alloc((nuint)bytes.Length + 1);
                bytes.AsSpan().CopyTo(new Span<byte>(owned, bytes.Length)); owned[bytes.Length] = 0;
                *output = owned; *outputLength = (nuint)bytes.Length; return status;
            }
            catch { return -4; }
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "gta_shader_free", CallConvs = new[] { typeof(CallConvCdecl) })]
    public static void Free(void* pointer) => NativeMemory.Free(pointer);
}
