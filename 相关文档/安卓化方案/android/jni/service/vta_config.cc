// jni/service/vta_config.cc
#include "vta_config.h"

#include "../common/json.h"
#include "../common/log.h"
#include "../common/strings.h"

namespace vta {

namespace {
std::string JoinPath(const std::string& base, const std::string& rel) {
    if (rel.empty()) return base;
    if (rel[0] == '/') return rel;
    if (!base.empty() && base.back() == '/') return base + rel;
    return base + "/" + rel;
}
}  // namespace

VtaConfig VtaConfig::Load(const std::string& baseDir) {
    VtaConfig c;
    // vta.json 位置：<dataDir>/vta.json（adb push 部署）或 <dataDir>/models/vta.json（assets 展开）
    std::string cfgPath = JoinPath(baseDir, "vta.json");
    if (!FileExists(cfgPath)) {
        std::string alt = JoinPath(JoinPath(baseDir, "models"), "vta.json");
        if (FileExists(alt)) cfgPath = alt;
    }
    auto doc = json::ParseFile(cfgPath);
    if (!doc) {
        std::string perr;
        std::string blob;
        if (ReadFileBytes(cfgPath, &blob)) {
            perr = "size=" + std::to_string(blob.size());
            auto direct = json::Parse(blob, &perr);
            if (direct) perr += " (direct parse ok?)";
        } else {
            perr = "cannot read file";
        }
        ALOGI("[CFG] vta.json 加载失败（%s：%s），使用默认配置（base=%s）", cfgPath.c_str(),
              perr.c_str(), baseDir.c_str());
        return c;
    }
    const auto& r = *doc;
    auto getStr = [&](const char* key, std::string* dst) {
        if (!r.at(key).isNull() && r.at(key).isString()) *dst = r.at(key).asString();
    };
    auto getInt = [&](const char* key, int* dst) {
        if (!r.at(key).isNull() && r.at(key).isNumber()) *dst = r.at(key).asInt();
    };
    auto getDouble = [&](const char* key, double* dst) {
        if (!r.at(key).isNull() && r.at(key).isNumber()) *dst = r.at(key).asDouble();
    };
    auto getFloat = [&](const char* key, float* dst) {
        if (!r.at(key).isNull() && r.at(key).isNumber()) *dst = static_cast<float>(r.at(key).asDouble());
    };
    auto getBool = [&](const char* key, bool* dst) {
        if (!r.at(key).isNull() && r.at(key).isBool()) *dst = r.at(key).asBool();
    };

    // ASR
    getStr("asrModelDir", &c.asrModelDir);
    getStr("asrEncoder", &c.asrEncoder);
    getStr("asrDecoder", &c.asrDecoder);
    getStr("asrJoiner", &c.asrJoiner);
    getStr("asrTokens", &c.asrTokens);
    getStr("decodingMethod", &c.decodingMethod);
    getInt("asrNumThreads", &c.asrNumThreads);
    getStr("asrModelingUnit", &c.asrModelingUnit);
    getStr("asrBpeVocab", &c.asrBpeVocab);
    getBool("hotwordDigits", &c.hotwordDigits);
    getBool("enableEndpoint", &c.enableEndpoint);
    getDouble("rule1TrailingSilence", &c.rule1TrailingSilence);
    getDouble("rule2TrailingSilence", &c.rule2TrailingSilence);
    getDouble("rule3TrailingSilence", &c.rule3TrailingSilence);
    getDouble("hotwordsScore", &c.hotwordsScore);

    // 交互
    getInt("silenceMs", &c.silenceMs);
    getInt("maxChars", &c.maxChars);
    getBool("lazyLoad", &c.lazyLoad);
    getInt("idleUnloadSeconds", &c.idleUnloadSeconds);
    getFloat("minSim", &c.minSim);

    // 降噪
    getBool("denoiseEnabled", &c.denoiseEnabled);
    getStr("denoiseModelPath", &c.denoiseModelPath);
    getInt("denoiseNumThreads", &c.denoiseNumThreads);

    // 路径
    getStr("ranerDir", &c.ranerDir);
    getStr("embedDir", &c.embedDir);
    getStr("tablesBaseDir", &c.tablesBaseDir);
    getStr("hrTablesRoot", &c.hrTablesRoot);
    getStr("charPinyinPath", &c.charPinyinPath);
    getStr("commonRulesPath", &c.commonRulesPath);
    getStr("defaultTable", &c.defaultTable);
    getInt("maxSessions", &c.maxSessions);
    getBool("broadcastCapture", &c.broadcastCapture);

    // 相对路径按 baseDir 解析
    c.asrModelDir = JoinPath(baseDir, c.asrModelDir);
    c.denoiseModelPath = JoinPath(baseDir, c.denoiseModelPath);
    c.ranerDir = JoinPath(baseDir, c.ranerDir);
    c.embedDir = JoinPath(baseDir, c.embedDir);
    c.tablesBaseDir = JoinPath(baseDir, c.tablesBaseDir);
    c.hrTablesRoot = JoinPath(baseDir, c.hrTablesRoot);
    c.charPinyinPath = JoinPath(baseDir, c.charPinyinPath);
    c.commonRulesPath = JoinPath(baseDir, c.commonRulesPath);
    return c;
}

}  // namespace vta
