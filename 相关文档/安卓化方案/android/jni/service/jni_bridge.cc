// jni/service/jni_bridge.cc —— APK 内测试用 JNI 桥（进程内直调 VtaServiceImpl，不走 binder）。
//
// 背景：普通 APK 无权 AServiceManager_addService（需系统签名/SELinux），故测试 APK 采用
// 进程内 JNI 直调 —— ASR/降噪/RaNER/HR/向量检索/cells 全链路真实执行，仅省去 binder 层。
// 正式集成（系统应用/预装）仍走 IVtaService AIDL，逻辑同一套 VtaServiceImpl。
#include <jni.h>

#include <memory>
#include <string>
#include <vector>

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

#include "../common/log.h"
#include "../common/strings.h"
#include "selftest_runner.h"
#include "vta_config.h"
#include "vta_service_impl.h"

using namespace vta;

namespace {

std::shared_ptr<VtaServiceImpl> g_service;
std::string g_dataDir;

std::string JStr(JNIEnv* env, jstring s) {
    if (s == nullptr) return {};
    const char* p = env->GetStringUTFChars(s, nullptr);
    std::string out = p ? p : "";
    env->ReleaseStringUTFChars(s, p);
    return out;
}

jstring NewStr(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }

VtaServiceImpl* Svc() { return g_service.get(); }

// 显式文件清单（AAssetDir 枚举在部分设备/打包方式下不可靠，改用确定性清单）
const char* kAssetFiles[] = {
    "models/vta.json",
    "models/asr/gtcrn_simple.onnx",
    "models/asr/silero_vad.onnx",
    "models/asr/sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30/encoder.int8.onnx",
    "models/asr/sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30/decoder.onnx",
    "models/asr/sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30/joiner.int8.onnx",
    "models/asr/sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30/tokens.txt",
    "models/raner/model.onnx",
    "models/raner/vocab.txt",
    "models/raner/config.json",
    "models/raner/crf_transitions.npy",
    "models/raner/crf_start_transitions.npy",
    "models/raner/crf_end_transitions.npy",
    "models/embedding/model_quantized.onnx",
    "models/embedding/tokenizer.json",
    "models/embedding/tables/registry.json",
    "models/embedding/tables/qiji/cell_index.bin",
    "models/embedding/tables/guolu/cell_index.bin",
    "models/sherpa-onnx/hr/hr_char_pinyin.txt",
    "test/selftest_16k.f32",
    "test/homophone_16k.f32",
};

bool ExtractAssets(AAssetManager* mgr, const std::string& outDir) {
    if (mgr == nullptr) {
        ALOGE("[ASSETS] AssetManager 为空");
        return false;
    }
    MakeDirs(outDir);
    int copied = 0, skipped = 0, failed = 0;
    for (const char* rel : kAssetFiles) {
        std::string outPath = outDir + "/" + rel;
        if (FileExists(outPath)) {
            ++skipped;
            continue;
        }
        AAsset* a = AAssetManager_open(mgr, rel, AASSET_MODE_STREAMING);
        if (a == nullptr) {
            ALOGW("[ASSETS] 打开失败: %s", rel);
            ++failed;
            continue;
        }
        off64_t len = AAsset_getLength64(a);
        std::string blob;
        blob.resize(static_cast<size_t>(len));
        if (len > 0) AAsset_read(a, blob.data(), static_cast<size_t>(len));
        AAsset_close(a);
        size_t slash = outPath.find_last_of('/');
        if (slash != std::string::npos) MakeDirs(outPath.substr(0, slash));
        if (WriteFileBytes(outPath, blob.data(), blob.size())) {
            ++copied;
        } else {
            ALOGW("[ASSETS] 写入失败: %s", outPath.c_str());
            ++failed;
        }
    }
    ALOGI("[ASSETS] 解包完成: 新写入 %d，已存在 %d，失败 %d → %s", copied, skipped, failed,
          outDir.c_str());
    return failed == 0;
}

}  // namespace

extern "C" {

// 解包 assets/models 到 filesDir（幂等；已有文件跳过）
JNIEXPORT jint JNICALL Java_com_vta_service_NativeBridge_nativeExtractAssets(JNIEnv* env, jclass,
                                                                            jobject assetManager,
                                                                            jstring outDir) {
    AAssetManager* mgr = AAssetManager_fromJava(env, assetManager);
    std::string dir = JStr(env, outDir);
    return ExtractAssets(mgr, dir) ? 0 : -1;
}

// 初始化服务（构造 VtaServiceImpl + Init），dataDir 为解包后的模型根目录
JNIEXPORT jint JNICALL Java_com_vta_service_NativeBridge_nativeStart(JNIEnv* env, jclass,
                                                                     jstring dataDir) {
    if (g_service) return 0;
    g_dataDir = JStr(env, dataDir);
    VtaConfig config = VtaConfig::Load(g_dataDir);
    try {
        g_service = ndk::SharedRefBase::make<VtaServiceImpl>(config);
        std::string err;
        if (!g_service->Init(&err)) {
            ALOGE("[VTA-JNI] init failed: %s", err.c_str());
            g_service.reset();
            return -2;
        }
    } catch (const std::exception& ex) {
        ALOGE("[VTA-JNI] start exception: %s", ex.what());
        g_service.reset();
        return -3;
    }
    ALOGI("[VTA-JNI] service ready, dataDir=%s", g_dataDir.c_str());
    return 0;
}

JNIEXPORT jstring JNICALL Java_com_vta_service_NativeBridge_nativeHealth(JNIEnv* env, jclass) {
    if (!Svc()) return NewStr(env, "not_started");
    std::string out;
    ndk::ScopedAStatus st = Svc()->health(&out);
    return NewStr(env, st.isOk() ? out : std::string("error:health"));
}

// 表清单：每项 "name|key|rows|cols"
JNIEXPORT jobjectArray JNICALL Java_com_vta_service_NativeBridge_nativeListTables(JNIEnv* env,
                                                                                  jclass) {
    std::vector<std::string> items;
    if (Svc()) {
        std::vector<aidl::com::vta::TableInfo> tables;
        (void)Svc()->listTables(&tables);
        for (const auto& t : tables) {
            items.push_back(t.name + "|" + t.key + "|" + std::to_string(t.rows) + "|" +
                            std::to_string(t.columns));
        }
    }
    jclass strCls = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray(static_cast<jsize>(items.size()), strCls, nullptr);
    for (size_t i = 0; i < items.size(); ++i)
        env->SetObjectArrayElement(arr, static_cast<jsize>(i), NewStr(env, items[i]));
    return arr;
}

// 离线自检：跑打包的 16k f32 PCM 全链路（无需麦克风）
// 返回多行文本：ok/final/partials/cells
JNIEXPORT jstring JNICALL Java_com_vta_service_NativeBridge_nativeSelftest(JNIEnv* env, jclass,
                                                                          jstring pcmPath,
                                                                          jstring dataDir) {
    std::string pcm = JStr(env, pcmPath);
    std::string dir = JStr(env, dataDir);
    SelftestResult r = RunSelftestOnPcm(pcm, dir);
    std::string out;
    out += std::string("ok=") + (r.ok ? "1" : "0") + "\n";
    if (!r.ok) out += "error=" + r.error + "\n";
    out += "final=" + r.finalText + "\n";
    out += "partials=" + std::to_string(r.partials.size()) + "\n";
    for (const auto& p : r.partials) out += "partial:" + p + "\n";
    out += "cells=" + std::to_string(r.cells.size()) + "\n";
    for (const auto& c : r.cells) out += "cell:" + c + "\n";
    return NewStr(env, out);
}

// 导入表（多表共存）：name + 行标签数组 + 列数 → 建索引/落盘/注册/重建该表语音资源
JNIEXPORT jint JNICALL Java_com_vta_service_NativeBridge_nativeImportTable(
    JNIEnv* env, jclass, jstring name, jobjectArray rowLabels, jint columnCount) {
    if (!Svc()) return -3;
    std::vector<std::string> rows;
    jsize n = rowLabels != nullptr ? env->GetArrayLength(rowLabels) : 0;
    for (jsize i = 0; i < n; ++i) {
        jstring s = static_cast<jstring>(env->GetObjectArrayElement(rowLabels, i));
        rows.push_back(JStr(env, s));
        if (s != nullptr) env->DeleteLocalRef(s);
    }
    int rc = 0;
    ndk::ScopedAStatus st = Svc()->importTable(JStr(env, name), rows, columnCount, &rc);
    return st.isOk() ? rc : -4;
}

// 表结构（测试页渲染表格）：首行 "name|rows|cols"，随后每行一个行标签
JNIEXPORT jstring JNICALL Java_com_vta_service_NativeBridge_nativeTableMeta(JNIEnv* env, jclass,
                                                                           jstring tableName) {
    if (!Svc()) return NewStr(env, "");
    return NewStr(env, Svc()->DescribeTable(JStr(env, tableName)));
}

JNIEXPORT jint JNICALL Java_com_vta_service_NativeBridge_nativeOpenSession(JNIEnv* env, jclass,
                                                                           jstring tableName,
                                                                           jint silenceMs,
                                                                           jint captureMode) {
    if (!Svc()) return -3;
    ::ndk::SpAIBinder nullClient;  // 进程内调用，不需要 binder 死亡通知
    int sid = 0;
    ndk::ScopedAStatus st = Svc()->openSession(JStr(env, tableName), silenceMs, captureMode,
                                               nullClient, &sid);
    return st.isOk() ? sid : -4;
}

JNIEXPORT jstring JNICALL Java_com_vta_service_NativeBridge_nativeGetState(JNIEnv* env, jclass,
                                                                           jint sessionId) {
    if (!Svc()) return NewStr(env, "not_started");
    aidl::com::vta::SessionState st{};
    (void)Svc()->getState(sessionId, &st);
    return NewStr(env, st.phase + "|" + st.partial);
}

// 返回数组，每项 "row|col|value|raw"。
// 坐标统一为 1-based（第一个行标签=第1行，一号=第1列），与测试页表格/前端 data-r|data-c 一致；
// AIDL 契约（正式客户端）仍是 0-based，本桥在此做 +1 归一，便于测试页直接回填。
JNIEXPORT jobjectArray JNICALL Java_com_vta_service_NativeBridge_nativePollCells(JNIEnv* env,
                                                                                jclass,
                                                                                jint sessionId,
                                                                                jint maxN) {
    std::vector<std::string> items;
    if (Svc()) {
        std::vector<aidl::com::vta::CellHit> cells;
        (void)Svc()->pollCells(sessionId, maxN, &cells);
        for (const auto& c : cells) {
            items.push_back(std::to_string(c.row + 1) + "|" + std::to_string(c.column + 1) + "|" +
                            c.value + "|" + c.raw);
        }
    }
    jclass strCls = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray(static_cast<jsize>(items.size()), strCls, nullptr);
    for (size_t i = 0; i < items.size(); ++i)
        env->SetObjectArrayElement(arr, static_cast<jsize>(i), NewStr(env, items[i]));
    return arr;
}

JNIEXPORT jint JNICALL Java_com_vta_service_NativeBridge_nativeCloseSession(JNIEnv* env, jclass,
                                                                           jint sessionId) {
    if (!Svc()) return -3;
    int rc = 0;
    ndk::ScopedAStatus st = Svc()->closeSession(sessionId, &rc);
    return st.isOk() ? rc : -4;
}

JNIEXPORT jint JNICALL Java_com_vta_service_NativeBridge_nativeSetDenoise(JNIEnv* env, jclass,
                                                                         jboolean enabled) {
    if (!Svc()) return -3;
    int rc = 0;
    (void)Svc()->setDenoise(enabled == JNI_TRUE, &rc);
    return rc;
}

// 日志开关：把 native 日志同时写到文件，便于无 adb 环境取证
JNIEXPORT jint JNICALL Java_com_vta_service_NativeBridge_nativeVersion(JNIEnv*, jclass) {
    return 1;
}

}  // extern "C"
