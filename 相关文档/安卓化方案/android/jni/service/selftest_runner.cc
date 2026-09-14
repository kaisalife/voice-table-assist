// jni/service/selftest_runner.cc —— 从 main.cc 抽出的自检实现（可执行体 + JNI 共用）
#include "selftest_runner.h"

#include <chrono>
#include <memory>

#include "../common/log.h"
#include "../common/strings.h"
#include "../homophone/homophone_replacer.h"
#include "../session/voice_session.h"
#include "../tables/table_vector_manager.h"
#include "../text/pinyin_table.h"
#include "../text/sound_aligner.h"
#include "engine_host.h"
#include "vta_config.h"

namespace vta {

namespace {
std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}
}  // namespace

SelftestResult RunSelftestOnPcm(const std::string& pcmPath, const std::string& dataDir) {
    SelftestResult r;
    VtaConfig config = VtaConfig::Load(dataDir);
    config.lazyLoad = false;

    EngineHost host(config);
    TableVectorManager manager(config.tablesBaseDir, config.hrTablesRoot, config.charPinyinPath,
                               config.commonRulesPath, config.defaultTable);

    std::string err;
    if (!host.EnsureEngines(nullptr, &err)) {
        r.error = "engines: " + err;
        ALOGE("[SELFTEST] %s", r.error.c_str());
        return r;
    }
    std::string key = manager.ResolveTargetKey(config.defaultTable);
    if (key.empty()) key = config.defaultTable;
    std::shared_ptr<const VtxIndex> index = manager.Activate(key);
    if (index == nullptr) {
        r.error = "no table index under " + config.tablesBaseDir;
        ALOGE("[SELFTEST] %s", r.error.c_str());
        return r;
    }
    // 表内读音吸附（词表 = 本表行标签 + 列说法）；热词/HR 规则已按设计移除
    if (!host.EnsureAsr("", &err)) {
        r.error = "asr: " + err;
        ALOGE("[SELFTEST] %s", r.error.c_str());
        return r;
    }

    VoiceSessionConfig cfg;
    cfg.tableName = config.defaultTable;
    cfg.tableKey = key;
    cfg.silenceMs = config.silenceMs;
    cfg.maxChars = config.maxChars;
    cfg.minSim = config.minSim;
    cfg.captureMode = 1;  // 文件喂流
    cfg.pinyinPath = config.charPinyinPath;  // 表内读音对齐用
    cfg.hotwordDigits = config.hotwordDigits;
    auto session = VoiceSession::CreateAndStart(1, cfg, &host, index, nullptr, &err);
    if (session == nullptr) {
        r.error = "session: " + err;
        ALOGE("[SELFTEST] %s", r.error.c_str());
        return r;
    }

    std::string blob;
    if (!ReadFileBytes(pcmPath, &blob)) {
        r.error = "cannot read pcm: " + pcmPath;
        ALOGE("[SELFTEST] %s", r.error.c_str());
        return r;
    }
    const float* samples = reinterpret_cast<const float*>(blob.data());
    size_t n = blob.size() / sizeof(float);
    ALOGI("[SELFTEST] pcm=%s samples=%zu (%.1fs)", pcmPath.c_str(), n, n / 16000.0);

    const size_t chunk = 1600;  // 100ms
    std::string lastPartial;
    auto tDecode0 = std::chrono::steady_clock::now();
    for (size_t off = 0; off < n; off += chunk) {
        size_t take = (n - off < chunk) ? n - off : chunk;
        session->PushPcm(samples + off, take);
        auto st = session->GetState();
        if (!st.partial.empty() && st.partial != lastPartial) {
            lastPartial = st.partial;
            r.partials.push_back(st.partial);
        }
    }
    session->Close();
    // 解码性能：RTF = 解码耗时 / 音频时长（>1 表示跟不上实时 → 会卡顿）
    double decodeSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - tDecode0).count();
    double audioSec = static_cast<double>(n) / 16000.0;
    ALOGI("[PERF] 解码 %.2fs / 音频 %.2fs → RTF=%.2f", decodeSec, audioSec,
          audioSec > 0 ? decodeSec / audioSec : 0.0);
    r.decodeSeconds = decodeSec;
    r.rtf = audioSec > 0 ? decodeSec / audioSec : 0.0;
    auto hits = session->PollCells(64, 100);
    for (const auto& h : hits) {
        r.cells.push_back(std::to_string(h.row) + "|" + std::to_string(h.column) + "|" + h.value +
                          "|" + h.raw);
        ALOGI("cell: row=%d col=%d value=%s raw=\"%s\"", h.row, h.column, h.value.c_str(),
              h.raw.c_str());
    }
    if (!r.partials.empty()) r.finalText = r.partials.back();
    std::string lastFinal = session->LastFinalText();
    if (!lastFinal.empty()) r.finalText = lastFinal;  // 收尾 final 优先（partial 为空时也能显示）
    r.ok = true;
    ALOGI("[SELFTEST] done: %zu cells", r.cells.size());
    return r;
}

TableSwitchResult RunTableSwitchTest(const std::string& pcmPath, const std::string& dataDir,
                                     const std::vector<std::string>& tables) {
    TableSwitchResult r;
    VtaConfig config = VtaConfig::Load(dataDir);
    config.lazyLoad = false;
    ALOGI("[SWITCH] 开始多表切换验证：%zu 张表，pcm=%s", tables.size(), pcmPath.c_str());

    EngineHost host(config);
    TableVectorManager manager(config.tablesBaseDir, config.hrTablesRoot, config.charPinyinPath,
                               config.commonRulesPath, config.defaultTable);

    std::string err;
    if (!host.EnsureEngines(nullptr, &err)) {
        r.error = "engines: " + err;
        ALOGE("[SWITCH] %s", r.error.c_str());
        return r;
    }

    std::string blob;
    if (!ReadFileBytes(pcmPath, &blob)) {
        r.error = "cannot read pcm: " + pcmPath;
        return r;
    }
    const float* samples = reinterpret_cast<const float*>(blob.data());
    size_t n = blob.size() / sizeof(float);
    const size_t chunk = 1600;

    auto py = PinyinTable::Get(config.charPinyinPath);
    bool asrReady = false;

    // 逐表切换：每次只用"这一张表"的数据（该表索引 + 该表读音词表；热词/HR 已移除）
    for (const auto& t : tables) {
        TableSwitchResult::Step step;
        step.table = t;

        std::string key = manager.ResolveTargetKey(t);
        if (key.empty()) {
            r.error = "表未注册: " + t;
            return r;
        }
        std::shared_ptr<const VtxIndex> index = manager.Activate(key);  // 切表（共享所有权）
        if (index == nullptr) {
            r.error = "索引加载失败: " + t;
            return r;
        }
        step.rows = index->rowsCount;
        step.cols = index->colsCount;

        // 识别器只装载一次（无热词 → 切表无需重建）
        step.recognizerRebuilt = !asrReady;
        if (!host.EnsureAsr("", &err)) {
            r.error = "asr: " + err;
            return r;
        }
        asrReady = true;

        // 该表的读音词表（用到哪张表就用哪张表的数据）
        std::shared_ptr<SoundAligner> aligner;
        if (py != nullptr)
            aligner = std::make_shared<SoundAligner>(py, index->rows, index->colsCount);

        VoiceSessionConfig cfg;
        cfg.tableName = t;
        cfg.tableKey = key;
        cfg.silenceMs = config.silenceMs;
        cfg.maxChars = config.maxChars;
        cfg.minSim = config.minSim;
        cfg.captureMode = 1;
        cfg.pinyinPath = config.charPinyinPath;
        cfg.hotwordDigits = config.hotwordDigits;
        auto session = VoiceSession::CreateAndStart(1, cfg, &host, index, nullptr, &err);
        if (session == nullptr) {
            r.error = "session(" + t + "): " + err;
            return r;
        }

        for (size_t off = 0; off < n; off += chunk) {
            size_t take = (n - off < chunk) ? n - off : chunk;
            session->PushPcm(samples + off, take);
            // 会话进行中强制重建识别器（验证：旧识别器由在跑会话共享保活，识别流不悬空）
            if (!r.forcedRebuildMidSession && off >= (n / chunk / 2) * chunk) {
                (void)host.RebuildAsr(&err);
                r.forcedRebuildMidSession = true;
                ALOGI("[SWITCH] 会话进行中强制重建识别器（表 %s）", t.c_str());
            }
        }
        session->Close();

        auto hits = session->PollCells(64, 100);
        step.finalText = session->LastFinalText();
        bool allWithin = true;
        for (const auto& h : hits) {
            step.cells.push_back(std::to_string(h.row) + "|" + std::to_string(h.column) + "|" +
                                 h.value + "|" + h.raw);
            if (h.row < 1 || h.row > step.rows || h.column < 1 || h.column > step.cols)
                allWithin = false;
        }
        step.cellsWithinTable = allWithin;
        ALOGI("[SWITCH] 表=%s(%d行x%d列) 重建=%s final=\"%s\" cells=%zu 落在本表范围=%s",
              t.c_str(), step.rows, step.cols, step.recognizerRebuilt ? "是" : "否",
              step.finalText.c_str(), step.cells.size(), allWithin ? "是" : "否");
        r.steps.push_back(std::move(step));
    }
    r.ok = true;
    return r;
}

}  // namespace vta
