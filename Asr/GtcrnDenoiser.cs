using Microsoft.ML.OnnxRuntime;
using Microsoft.ML.OnnxRuntime.Tensors;

namespace VoiceTableAssist.Asr;

/// <summary>
/// GTCRN (Grouped Temporal Convolutional Recurrent Network) 进程内 PCM 流式降噪器。
/// 模型：sherpa-onnx 官方发布的 <c>gtcrn_simple.onnx</c>（48.2K 参数，33.0 MMACs/s，ICASSP 2024）。
///
/// <para>
/// 移植自 sherpa-onnx <c>online-speech-denoiser-gtcrn-impl.h</c> + <c>online-speech-denoiser-stft-impl.h</c>：
///   - STFT：窗 √hann（hann_sqrt），n_fft=512，hop=256，window=512（16kHz）；
///   - 每 hop 进分析窗 → 前向 DFT → ONNX → 逆向 DFT → √hann 加窗 → overlap-add → 输出 hop 段；
///   - 状态缓存 <c>conv_cache</c>/<c>tra_cache</c>/<c>inter_cache</c> 跨帧携带；
///   - 帧对齐/补零/收尾 flush 逻辑与 sherpa 参考一致（16kHz 输入 → 无需重采样）。
/// 设计目标：与 sherpa-onnx 离线降噪输出数值一致（误差 &lt; 1e-4），可被现场 /selftest 对照验证。
/// </para>
/// </summary>
public sealed class GtcrnDenoiser : IDisposable
{
    private readonly InferenceSession _session;

    private const int Nfft = 512;
    private const int HopLength = 256;
    private const int WindowLength = 512;
    private const int NumBins = Nfft / 2 + 1;   // 257
    private const int SpecSize = NumBins * 2;   // 514

    // ---- STFT 状态（对齐 OnlineSpeechDenoiserStftImpl） ----
    private float[] _analysisBuffer = new float[WindowLength];
    private float[] _overlapAddBuffer = new float[WindowLength];
    private readonly float[] _fftInput = new float[WindowLength];
    private readonly float[] _fftOutput = new float[SpecSize];
    private readonly float[] _enhancedFftOutput = new float[SpecSize];
    private readonly float[] _ifftOutput = new float[WindowLength];
    private readonly float[] _window = MakeHannSqrtWindow();
    private readonly List<float> _pendingInput = new();
    private bool _started;
    private long _totalInputSamples;
    private long _totalOutputSamples;
    private readonly float[] _zeroHop = new float[HopLength];

    // ---- DFT 预计算表（对齐 StreamingDft） ----
    private readonly double[] _cosF;
    private readonly double[] _sinF;
    private readonly double[] _cosI;
    private readonly double[] _sinI;

    // ---- ONNX 状态缓存 ----
    private DenseTensor<float> _convCache = null!;
    private DenseTensor<float> _traCache = null!;
    private DenseTensor<float> _interCache = null!;

    private readonly object _gate = new();

    /// <summary>当前是否已实现 DSP（自 v1.2 起为 true）。</summary>
    public bool DspImplemented => true;

    public GtcrnDenoiser(string modelPath, int numThreads, int sampleRate)
    {
        if (!File.Exists(modelPath))
            throw new FileNotFoundException($"GTCRN 模型文件不存在：{modelPath}");
        if (sampleRate != 16000)
            throw new ArgumentException($"GTCRN 仅支持 16kHz，实际 {sampleRate}", nameof(sampleRate));

        var opts = new Microsoft.ML.OnnxRuntime.SessionOptions
        {
            IntraOpNumThreads = numThreads,
            LogSeverityLevel = OrtLoggingLevel.ORT_LOGGING_LEVEL_WARNING,
        };
        _session = new InferenceSession(modelPath, opts);

        // 预计算 DFT 表（StreamingDft 构造）
        const double pi = Math.PI;
        _cosF = new double[NumBins * Nfft];
        _sinF = new double[NumBins * Nfft];
        _cosI = new double[Nfft * NumBins];
        _sinI = new double[Nfft * NumBins];
        for (int k = 0; k < NumBins; ++k)
        {
            for (int n = 0; n < Nfft; ++n)
            {
                double angle = 2.0 * pi * k * n / Nfft;
                double c = Math.Cos(angle);
                double s = Math.Sin(angle);
                _cosF[k * Nfft + n] = c;
                _sinF[k * Nfft + n] = s;
                _cosI[n * NumBins + k] = c;
                _sinI[n * NumBins + k] = s;
            }
        }
        ResetCaches();
    }

    private void ResetCaches()
    {
        // 与 sherpa-onnx 导出 ONNX 的形状严格一致
        _convCache = new DenseTensor<float>(new[] { 2, 1, 16, 16, 33 });
        _traCache = new DenseTensor<float>(new[] { 2, 3, 1, 1, 16 });
        _interCache = new DenseTensor<float>(new[] { 2, 1, 33, 16 });
    }

    /// <summary>复位所有状态缓存（GRU/DFT/overlap-add）。在新一轮对话开始时调用。</summary>
    public void Reset()
    {
        Array.Clear(_analysisBuffer);
        Array.Clear(_overlapAddBuffer);
        _pendingInput.Clear();
        _started = false;
        _totalInputSamples = 0;
        _totalOutputSamples = 0;
        ResetCaches();
    }

    /// <summary>
    /// 对一段 16kHz float32 PCM 样本（[-1,1]）做流式降噪，返回降噪后的样本段。
    /// 采用 hop=256 的流式 STFT → GTCRN → ISTFT → overlap-add，逐 hop 输出。
    /// <paramref name="flush"/> 为 true 时做收尾（补零 + 尾帧 + 复位），用于音频段结束。
    /// </summary>
    public float[] Denoise(ReadOnlySpan<float> samples, bool flush = false)
    {
        lock (_gate)
        {
            _totalInputSamples += samples.Length;
            _pendingInput.AddRange(samples.ToArray());

            var output = new List<float>(samples.Length);

            // ProcessPending：每满一个 hop 处理一帧
            while (_pendingInput.Count >= HopLength)
            {
                ProcessHop(output);
            }

            if (flush)
            {
                // Flush：补零到 hop 处理剩余，再处理一个 zero-hop 收尾
                if (_pendingInput.Count > 0)
                {
                    var padded = new float[HopLength];
                    _pendingInput.CopyTo(padded, 0);
                    ProcessHopBuffer(padded, output);
                    _pendingInput.Clear();
                }

                if (_started)
                    ProcessHopBuffer(_zeroHop, output);

                // 只输出实际输入量（对齐 sherpa Flush 的 remaining 裁剪）
                long remaining = _totalInputSamples - _totalOutputSamples;
                if (remaining < 0) remaining = 0;
                if (output.Count > remaining)
                    output.RemoveRange((int)remaining, output.Count - (int)remaining);
                _totalOutputSamples += output.Count;

                Reset();
            }

            return output.ToArray();
        }
    }

    private void ProcessHop(List<float> output)
    {
        // 从 pendingInput 消费一个 hop
        Span<float> hop = stackalloc float[HopLength];
        for (int i = 0; i < HopLength; i++) hop[i] = _pendingInput[i];
        _pendingInput.RemoveRange(0, HopLength);
        ProcessHopBuffer(hop, output);
    }

    private void ProcessHopBuffer(ReadOnlySpan<float> hop, List<float> output)
    {
        // 1) 滑动分析窗：左移 hop，右侧填入新 hop
        Array.Copy(_analysisBuffer, HopLength, _analysisBuffer, 0, WindowLength - HopLength);
        for (int i = 0; i < HopLength; ++i)
            _analysisBuffer[WindowLength - HopLength + i] = hop[i];

        // 2) 窗乘
        for (int i = 0; i < WindowLength; ++i)
            _fftInput[i] = _analysisBuffer[i] * _window[i];

        // 3) 前向 DFT
        ForwardDft(_fftInput, _fftOutput);

        // 4) ONNX 推理（mix + 3 caches）→ enh + 3 cache_out
        ProcessFrame();

        // 5) 逆向 DFT
        InverseDft(_enhancedFftOutput, _ifftOutput);

        // 6) overlap-add：左移 hop，原位清空尾部，叠加 ifft*window
        Array.Copy(_overlapAddBuffer, HopLength, _overlapAddBuffer, 0, WindowLength - HopLength);
        Array.Clear(_overlapAddBuffer, WindowLength - HopLength, HopLength);
        for (int i = 0; i < WindowLength; ++i)
            _overlapAddBuffer[i] += _ifftOutput[i] * _window[i];

        // 7) 第一次调用不输出（started_），之后输出前 hop 段
        if (!_started)
        {
            _started = true;
            return;
        }

        for (int i = 0; i < HopLength; ++i)
            output.Add(_overlapAddBuffer[i]);
    }

    private void ProcessFrame()
    {
        // mix = [1, 257, 1, 2]，从 _fftOutput 拷贝
        var mix = new DenseTensor<float>(_fftOutput, new[] { 1, NumBins, 1, 2 });

        using var results = _session.Run(new[]
        {
            NamedOnnxValue.CreateFromTensor("mix", mix),
            NamedOnnxValue.CreateFromTensor("conv_cache", _convCache),
            NamedOnnxValue.CreateFromTensor("tra_cache", _traCache),
            NamedOnnxValue.CreateFromTensor("inter_cache", _interCache),
        });

        var enhanced = results.First(o => o.Name == "enh").AsTensor<float>();
        enhanced.ToArray().CopyTo(_enhancedFftOutput, 0);

        _convCache = new DenseTensor<float>(results.First(o => o.Name == "conv_cache_out").AsTensor<float>().ToArray(), new[] { 2, 1, 16, 16, 33 });
        _traCache = new DenseTensor<float>(results.First(o => o.Name == "tra_cache_out").AsTensor<float>().ToArray(), new[] { 2, 3, 1, 1, 16 });
        _interCache = new DenseTensor<float>(results.First(o => o.Name == "inter_cache_out").AsTensor<float>().ToArray(), new[] { 2, 1, 33, 16 });
    }

    // ---- DFT（对齐 StreamingDft） ----

    private void ForwardDft(float[] input, float[] output)
    {
        for (int k = 0; k < NumBins; ++k)
        {
            double real = 0, imag = 0;
            int off = k * Nfft;
            for (int n = 0; n < Nfft; ++n)
            {
                double v = input[n];
                real += v * _cosF[off + n];
                imag -= v * _sinF[off + n];
            }
            output[2 * k] = (float)real;
            output[2 * k + 1] = (float)imag;
        }
    }

    private void InverseDft(float[] input, float[] output)
    {
        for (int n = 0; n < Nfft; ++n)
        {
            double sum = input[0];
            if (Nfft % 2 == 0)
                sum += input[2 * (NumBins - 1)] * ((n & 1) != 0 ? -1.0 : 1.0);

            int off = n * NumBins;
            for (int k = 1; k < NumBins - 1; ++k)
            {
                double real = input[2 * k];
                double imag = input[2 * k + 1];
                sum += 2.0 * (real * _cosI[off + k] - imag * _sinI[off + k]);
            }
            output[n] = (float)(sum / Nfft);
        }
    }

    private static float[] MakeHannSqrtWindow()
    {
        var w = new float[WindowLength];
        for (int i = 0; i < WindowLength; i++)
        {
            double h = 0.5 * (1.0 - Math.Cos(2.0 * Math.PI * i / (WindowLength - 1)));
            w[i] = (float)Math.Sqrt(h);
        }
        return w;
    }

    public void Dispose() => _session.Dispose();
}