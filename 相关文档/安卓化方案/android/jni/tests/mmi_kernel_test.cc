// jni/tests/mmi_kernel_test.cc —— MatMulInteger(u8×s8) kernel 正确性验证（自包含最小图）。
// 背景：Windows x64 ORT 1.27/1.28 的 MatMulInteger(u8,s8) 对随机数据算错（numpy 手算 diff 1e4）。
// 此测试在目标设备跑同一用例：diff=0 → 设备 kernel 正确（量化模型没问题）；
// diff≠0 → 该平台 ORT u8s8 kernel 也坏。
// 入口：RunMmiKernelTest()（main.cc --mmi-test 调用）。
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include <onnxruntime_cxx_api.h>

namespace {

// 在内存里构造最小 MatMulInteger 图（opset 13）
Ort::Session BuildMmiSession(Ort::Env& env, const std::vector<uint8_t>& A,
                             const std::vector<int8_t>& B, uint8_t azp) {
    // 图：
    //   a (u8 [M,K]) ─┐
    //   b (i8 [K,N]) ─┼─ MatMulInteger(azp,bzp) → out (i32 [M,N])
    //   azp(u8 [1])  ─┘
    // 直接用 ORT C++ 拼图很繁琐 → 用 ONNX 序列化：手写 protobuf 太重。
    // 简化：让 A/B/azp 作为模型 initializer，由 C++ 端用 ONNX protobuf 手拼。
    // 这里采用更简单的方案：把模型文件从 assets 读入 —— 由 copy_models.ps1 生成
    // assets/models/test/mmi_min.onnx。若无文件返回空 session。
    return Ort::Session(nullptr);
}

}  // namespace

// 独立实现：不依赖模型文件 —— 直接在 C++ 里验证 MatMulInteger kernel 语义等价性
// 是不可能的（没有图就没有 kernel）。因此本测试采用"端到端数字对照"：
// PC 侧 raner_check.py 已证明 int8 模型 emissions 与 fp32 偏差巨大；
// 本函数在设备上跑同一 int8 模型的 emissions，与设备上 fp32 模型的 emissions 比 cos。
// cos≈1 → 设备 kernel 正确（PC 侧问题）；cos<0.9 → 模型/kernel 都坏。
extern "C" int RunMmiKernelTest(const char* int8ModelPath, const char* fp32ModelPath) {
    Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "mmi-test");
    try {
        Ort::SessionOptions opts;
        opts.SetGraphOptimizationLevel(ORT_DISABLE_ALL);
        opts.SetIntraOpNumThreads(1);

        Ort::Session s8(env, int8ModelPath, opts);
        Ort::Session s32(env, fp32ModelPath, opts);

        // 构造 input_ids： "硬度一号是十七点八四" 逐字 —— 但没有 vocab。
        // 用固定 token 序列（从 PC 侧 raner_check.py 的输出抄来）：
        // [CLS]=101, 硬=3764, 度=2506, 一=671, 号=1046, 是=2213, 十=685, 七=1246,
        // 点=1934, 八=663, 四=1154, [SEP]=102
        const int64_t idsArr[] = {101, 3764, 2506, 671, 1046, 2213, 685, 1246,
                                  1934, 663, 1154, 102};
        const int seq = 128;
        std::vector<int64_t> ids(seq, 0), mask(seq, 0);
        int n = sizeof(idsArr) / sizeof(idsArr[0]);
        for (int i = 0; i < n && i < seq; ++i) {
            ids[i] = idsArr[i];
            mask[i] = 1;
        }

        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<int64_t> shape{1, seq};
        Ort::Value tIds = Ort::Value::CreateTensor<int64_t>(mem, ids.data(), ids.size(), shape.data(), 2);
        Ort::Value tMask = Ort::Value::CreateTensor<int64_t>(mem, mask.data(), mask.size(), shape.data(), 2);
        const char* inNames[] = {"input_ids", "attention_mask"};
        const char* outNames[] = {"emissions"};
        std::vector<Ort::Value> ins;
        ins.push_back(std::move(tIds));
        ins.push_back(std::move(tMask));
        auto o8 = s8.Run(Ort::RunOptions{nullptr}, inNames, ins.data(), 2, outNames, 1);
        auto o32 = s32.Run(Ort::RunOptions{nullptr}, inNames, ins.data(), 2, outNames, 1);

        auto info8 = o8[0].GetTensorTypeAndShapeInfo();
        auto info32 = o32[0].GetTensorTypeAndShapeInfo();
        size_t cnt = info8.GetElementCount();
        const float* e8 = o8[0].GetTensorData<float>();
        const float* e32 = o32[0].GetTensorData<float>();

        double dot = 0, n8 = 0, n32 = 0;
        int argmaxMatch = 0;
        for (size_t i = 0; i < cnt; ++i) {
            dot += static_cast<double>(e8[i]) * e32[i];
            n8 += static_cast<double>(e8[i]) * e8[i];
            n32 += static_cast<double>(e32[i]) * e32[i];
        }
        for (size_t t = 0; t < cnt / 7; ++t) {
            int b8 = 0, b32 = 0;
            for (int j = 1; j < 7; ++j) {
                if (e8[t * 7 + j] > e8[t * 7 + b8]) b8 = j;
                if (e32[t * 7 + j] > e32[t * 7 + b32]) b32 = j;
            }
            if (b8 == b32) ++argmaxMatch;
        }
        double cosv = dot / (std::sqrt(n8) * std::sqrt(n32) + 1e-9);
        std::printf("[MMI-TEST] emissions cos(int8,fp32)=%.5f argmaxMatch=%zu/%zu\n",
                    cosv, argmaxMatch, cnt / 7);
        std::printf("[MMI-TEST] absmax int8=%.4f fp32=%.4f\n",
                    [] (const float* p, size_t n) {
                        float m = 0;
                        for (size_t i = 0; i < n; ++i) m = std::fabs(p[i]) > m ? std::fabs(p[i]) : m;
                        return m;
                    }(e8, cnt),
                    [] (const float* p, size_t n) {
                        float m = 0;
                        for (size_t i = 0; i < n; ++i) m = std::fabs(p[i]) > m ? std::fabs(p[i]) : m;
                        return m;
                    }(e32, cnt));
        return cosv > 0.99 ? 0 : 1;
    } catch (const std::exception& ex) {
        std::printf("[MMI-TEST] error: %s\n", ex.what());
        return 2;
    }
}
