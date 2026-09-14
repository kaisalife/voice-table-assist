// jni/service/engine_host.cc —— 翻译自 Services/EngineHost.cs
#include "engine_host.h"

#include <functional>

#include "../asr/recognizer.h"
#include "../common/log.h"
#include "../common/strings.h"
#include "../denoise/gtcrn_denoiser.h"
#include "../embed/embedder.h"
#include "../ner/raner_engine.h"
#include "vta_config.h"

namespace vta {

EngineHost::EngineHost(const VtaConfig& config) : config_(config) {
    lastTouchMs_ = NowMs();
    denoiseEnabled_ = config.denoiseEnabled;
    if (config.lazyLoad) {
        ALOGI("[MODELS] 语义引擎（RaNER/嵌入）懒加载：首次使用约 3~8s，空闲 %ds 自动卸载；ASR 识别器常驻",
              config.idleUnloadSeconds);
    }
    idleThread_ = std::thread([this] { IdleLoop(); });
}

EngineHost::~EngineHost() {
    {
        std::lock_guard<std::mutex> lk(idleMu_);
        quit_ = true;
    }
    idleCv_.notify_all();
    // 必须 join：空闲线程会访问本对象的 gate_/raner_/config_，detach 会在析构后访问已销毁成员
    if (idleThread_.joinable()) idleThread_.join();
    std::lock_guard<std::mutex> lk(gate_);
    raner_.reset();
    embed_.reset();
    {
        std::lock_guard<std::mutex> alk(asrMu_);
        asr_.reset();
    }
}

bool EngineHost::EnsureEngines(const std::function<void(const std::string&)>& progress,
                               std::string* err) {
    std::unique_lock<std::mutex> lk(gate_);
    if (loaded_) {
        Touch();
        return true;
    }
    if (loading_) {  // 已有在途加载，搭车等待
        lk.unlock();
        while (!loaded_ && !quit_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (loaded_) { Touch(); return true; }
        if (err) *err = "引擎加载中/失败";
        return false;
    }
    loading_ = true;
    lk.unlock();

    auto swMs = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    int64_t t0 = swMs();
    bool ok = false;
    std::string localErr;
    try {
        if (progress) progress("正在加载语义解析模型（首次使用需数秒）...");
        auto raner = std::make_unique<RaNerEngine>(config_.ranerDir);
        if (progress) progress("正在加载嵌入模型...");
        auto embed = std::make_unique<EmbeddingEngine>(config_.embedDir);
        embed->minSim = config_.minSim;  // 检索低置信过滤：宁缺勿错填；0=不过滤
        lk.lock();
        raner_ = std::move(raner);
        embed_ = std::move(embed);
        loaded_ = true;
        loading_ = false;
        lk.unlock();
        ALOGI("[MODELS] 引擎按需加载完成 %lldms (RaNER 7类标签)", static_cast<long long>(swMs() - t0));
        if (progress) progress("模型加载完成");
        Touch();
        ok = true;
    } catch (const std::exception& ex) {
        ALOGE("[MODELS] 引擎加载失败: %s", ex.what());
        localErr = ex.what();
        lk.lock();
        loading_ = false;
        lk.unlock();
    }
    if (err && !ok) *err = localErr;
    return ok;
}

bool EngineHost::BuildAsrLocked(std::string* err) {
    try {
        AsrRecognizer::Config c;
        c.modelDir = config_.asrModelDir;
        c.encoder = config_.asrEncoder;
        c.decoder = config_.asrDecoder;
        c.joiner = config_.asrJoiner;
        c.tokens = config_.asrTokens;
        c.decodingMethod = config_.decodingMethod;
        c.numThreads = config_.asrNumThreads;
        c.modelingUnit = config_.asrModelingUnit;
        c.bpeVocab = config_.asrBpeVocab;
        c.enableEndpoint = config_.enableEndpoint;
        c.rule1TrailingSilence = config_.rule1TrailingSilence;
        c.rule2TrailingSilence = config_.rule2TrailingSilence;
        c.rule3TrailingSilence = config_.rule3TrailingSilence;
        c.hotwordsFile = "";  // 不用热词（同音纠错交给表内读音吸附）
        c.hotwordsScore = config_.hotwordsScore;
        auto asr = std::make_shared<AsrRecognizer>(c);
        {
            std::lock_guard<std::mutex> alk(asrMu_);
            asr_ = std::move(asr);
        }
        ALOGI("[ASR] 识别器就绪并常驻（热词按会话流传入，切表/导入不重建）");
        Touch();
        return true;
    } catch (const std::exception& ex) {
        ALOGE("[ASR] 识别器构建失败: %s", ex.what());
        if (err) *err = ex.what();
        return false;
    }
}

bool EngineHost::EnsureAsr(const std::string& /*hotwordsFile*/, std::string* err) {
    std::lock_guard<std::mutex> lk(gate_);
    {
        std::lock_guard<std::mutex> alk(asrMu_);
        if (asr_) {
            Touch();
            return true;
        }
    }
    return BuildAsrLocked(err);
}

bool EngineHost::RebuildAsr(std::string* err) {
    std::lock_guard<std::mutex> lk(gate_);
    ALOGI("[ASR] 强制重建识别器（旧识别器由在跑会话保活）");
    return BuildAsrLocked(err);
}

std::shared_ptr<AsrRecognizer> EngineHost::Asr() const {
    std::lock_guard<std::mutex> lk(asrMu_);
    return asr_;
}

void EngineHost::Touch() { lastTouchMs_ = NowMs(); }

GtcrnDenoiser* EngineHost::NewDenoiser(std::string* err) const {
    if (!denoiseEnabled_.load()) return nullptr;
    try {
        // 每会话一个实例（GRU/STFT 状态隔离），ONNX 模型由 ORT 内部共享只读
        return new GtcrnDenoiser(config_.denoiseModelPath, config_.denoiseNumThreads, 16000);
    } catch (const std::exception& ex) {
        ALOGE("[DENOISE] 构建失败: %s", ex.what());
        if (err) *err = ex.what();
        return nullptr;
    }
}

void EngineHost::OnSessionOpened() { ++activeSessions_; }

void EngineHost::OnSessionClosed() {
    if (activeSessions_.load() > 0) --activeSessions_;
    Touch();
}

void EngineHost::IdleLoop() {
    std::unique_lock<std::mutex> lk(idleMu_);
    while (!quit_.load()) {
        // 可被析构立即唤醒（不用裸 sleep，避免析构时还要等 5s）
        idleCv_.wait_for(lk, std::chrono::seconds(5), [&] { return quit_.load(); });
        if (quit_.load()) break;
        if (!loaded_.load()) continue;
        if (activeSessions_.load() > 0) continue;
        if (NowMs() - lastTouchMs_.load() <= static_cast<int64_t>(config_.idleUnloadSeconds) * 1000)
            continue;
        {
            std::lock_guard<std::mutex> gate(gate_);
            if (!loaded_.load()) continue;
            loaded_ = false;
            raner_.reset();
            embed_.reset();
            ALOGI("[MODELS] 空闲 %ds，已卸载语义引擎（ASR 识别器常驻不卸载）",
                  config_.idleUnloadSeconds);
        }
    }
}

}  // namespace vta
