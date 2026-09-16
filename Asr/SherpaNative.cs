using System.Reflection;
using System.Runtime.InteropServices;

namespace VoiceTableAssist.Asr;

/// <summary>
/// sherpa-onnx C API 的 P/Invoke 绑定（对齐官方 v1.13.6 <c>sherpa-onnx/c-api/c-api.h</c>）。
///
/// 只声明流式识别所需的最小集合：结构体**必须与头文件字段顺序完全一致**（顺序/类型不符会踩内存），
/// 故改动前先核对同版本头文件。识别器进程内常驻；热词按流传入
/// （<see cref="SherpaOnnxCreateOnlineStreamWithHotwords"/>），切表/导入无需重建。
/// </summary>
internal static class SherpaNative
{
    internal const string Library = "sherpa-onnx-c-api";

    // ---------------- 配置结构体（字段顺序 = c-api.h 顺序）----------------

    [StructLayout(LayoutKind.Sequential)]
    internal struct OnlineTransducerModelConfig
    {
        public IntPtr Encoder;
        public IntPtr Decoder;
        public IntPtr Joiner;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct OnlineParaformerModelConfig
    {
        public IntPtr Encoder;
        public IntPtr Decoder;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct OnlineZipformer2CtcModelConfig
    {
        public IntPtr Model;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct OnlineNemoCtcModelConfig
    {
        public IntPtr Model;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct OnlineToneCtcModelConfig
    {
        public IntPtr Model;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct OnlineModelConfig
    {
        public OnlineTransducerModelConfig Transducer;
        public OnlineParaformerModelConfig Paraformer;
        public OnlineZipformer2CtcModelConfig Zipformer2Ctc;
        public IntPtr Tokens;
        public int NumThreads;
        public IntPtr Provider;
        public int Debug;
        public IntPtr ModelType;
        public IntPtr ModelingUnit;
        public IntPtr BpeVocab;
        public IntPtr TokensBuf;
        public int TokensBufSize;
        public OnlineNemoCtcModelConfig NemoCtc;
        public OnlineToneCtcModelConfig ToneCtc;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct FeatureConfig
    {
        public int SampleRate;
        public int FeatureDim;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct OnlineCtcFstDecoderConfig
    {
        public IntPtr Graph;
        public int MaxActive;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct HomophoneReplacerConfig
    {
        public IntPtr DictDir;    // 历史字段（ABI 占位）
        public IntPtr Lexicon;
        public IntPtr RuleFsts;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct OnlineRecognizerConfig
    {
        public FeatureConfig FeatConfig;
        public OnlineModelConfig ModelConfig;
        public IntPtr DecodingMethod;
        public int MaxActivePaths;
        public int EnableEndpoint;
        public float Rule1MinTrailingSilence;
        public float Rule2MinTrailingSilence;
        public float Rule3MinUtteranceLength;
        public IntPtr HotwordsFile;
        public float HotwordsScore;
        public OnlineCtcFstDecoderConfig CtcFstDecoderConfig;
        public IntPtr RuleFsts;
        public IntPtr RuleFars;
        public float BlankPenalty;
        public IntPtr HotwordsBuf;
        public int HotwordsBufSize;
        public HomophoneReplacerConfig Hr;
    }

    // ---------------- 结果结构体 ----------------

    [StructLayout(LayoutKind.Sequential)]
    internal struct OnlineRecognizerResult
    {
        public IntPtr Text;
        public IntPtr Tokens;
        public IntPtr TokensArr;
        public IntPtr Timestamps;
        public int Count;
        public IntPtr Json;
    }

    // ---------------- 函数 ----------------

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr SherpaOnnxGetVersionStr();

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr SherpaOnnxCreateOnlineRecognizer(ref OnlineRecognizerConfig config);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void SherpaOnnxDestroyOnlineRecognizer(IntPtr recognizer);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr SherpaOnnxCreateOnlineStream(IntPtr recognizer);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr SherpaOnnxCreateOnlineStreamWithHotwords(IntPtr recognizer, IntPtr hotwords);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void SherpaOnnxDestroyOnlineStream(IntPtr stream);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void SherpaOnnxOnlineStreamAcceptWaveform(IntPtr stream, int sampleRate, float[] samples, int n);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SherpaOnnxIsOnlineStreamReady(IntPtr recognizer, IntPtr stream);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void SherpaOnnxDecodeOnlineStream(IntPtr recognizer, IntPtr stream);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr SherpaOnnxGetOnlineStreamResult(IntPtr recognizer, IntPtr stream);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void SherpaOnnxDestroyOnlineRecognizerResult(IntPtr result);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int SherpaOnnxOnlineStreamIsEndpoint(IntPtr recognizer, IntPtr stream);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void SherpaOnnxOnlineStreamInputFinished(IntPtr stream);

    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void SherpaOnnxOnlineStreamReset(IntPtr recognizer, IntPtr stream);

    /// <summary>结果结构体 → 文本（复制出托管字符串后即可销毁原生结果）。</summary>
    internal static string ReadResultText(IntPtr recognizer, IntPtr stream)
    {
        var result = SherpaOnnxGetOnlineStreamResult(recognizer, stream);
        if (result == IntPtr.Zero) return "";
        try
        {
            var structResult = Marshal.PtrToStructure<OnlineRecognizerResult>(result);
            return structResult.Text == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(structResult.Text) ?? "";
        }
        finally
        {
            SherpaOnnxDestroyOnlineRecognizerResult(result);
        }
    }

    // ---------------- 原生库加载 ----------------

    private static int _resolverBearing;
    private static string? _nativeDir;

    /// <summary>
    /// 注册 DLL 解析器并从 <paramref name="nativeDir"/> 加载 sherpa-onnx-c-api.dll
    /// （依赖 onnxruntime.dll 先按同名加载，避免依赖解析走系统 PATH）。
    /// 幂等：重复调用只校验目录一致。
    /// </summary>
    internal static void Load(string nativeDir)
    {
        var dir = Path.GetFullPath(nativeDir);
        lock (typeof(SherpaNative))
        {
            if (Interlocked.Exchange(ref _resolverBearing, 1) == 1)
            {
                if (!string.Equals(_nativeDir, dir, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException($"sherpa 原生库已从 {_nativeDir} 加载，不能改用 {dir}");
                return;
            }

            _nativeDir = dir;
            var onnx = Path.Combine(dir, "onnxruntime.dll");
            if (!File.Exists(onnx)) onnx = Path.Combine(dir, "libonnxruntime.so");
            var providers = Path.Combine(dir, OperatingSystem.IsWindows() ? "onnxruntime_providers_shared.dll" : "libonnxruntime_providers_shared.so");

            NativeLibrary.SetDllImportResolver(typeof(SherpaNative).Assembly, Resolve);
            if (File.Exists(onnx)) NativeLibrary.Load(onnx);
            if (File.Exists(providers)) NativeLibrary.Load(providers);

            var lib = Path.Combine(dir, OperatingSystem.IsWindows() ? "sherpa-onnx-c-api.dll" : "libsherpa-onnx-c-api.so");
            NativeLibrary.Load(lib);
        }
    }

    private static IntPtr Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPaths)
    {
        if (libraryName != Library && libraryName != "sherpa-onnx-c-api.dll" && libraryName != "libsherpa-onnx-c-api.so")
            return IntPtr.Zero;   // 交给默认解析

        var dir = _nativeDir ?? throw new InvalidOperationException("sherpa 原生库目录未初始化（应先调用 SherpaNative.Load）");
        var fileName = OperatingSystem.IsWindows() ? "sherpa-onnx-c-api.dll" : "libsherpa-onnx-c-api.so";
        var path = Path.Combine(dir, fileName);
        if (!File.Exists(path)) throw new FileNotFoundException($"未找到 sherpa 原生库: {path}", path);
        return NativeLibrary.Load(path);
    }
}
