// jni/service/selftest_runner.h —— 文件回放全管线自检（可执行体与 APK 内测试共用）
#pragma once

#include <string>
#include <vector>

namespace vta {

struct SelftestResult {
    bool ok = false;
    std::string error;
    std::string finalText;                 // ASR 最终文本（HR + 领域纠错后）
    std::vector<std::string> partials;     // 过程中 partial（去重后）
    std::vector<std::string> cells;        // "row|col|value|raw"
    double decodeSeconds = 0;              // 解码耗时（不含模型加载）
    double rtf = 0;                        // 解码耗时 / 音频时长
};

// 用 <dataDir> 下的模型跑一段 16kHz float32 单声道 PCM，返回全链路结果。
SelftestResult RunSelftestOnPcm(const std::string& pcmPath, const std::string& dataDir);

// ---- 多表共存 + 单活动表 切换验证 ----
// 模型：导入多张表共存；同一时刻只服务一张；用到哪张表就用哪张表的数据。
// 验证点：
//   1) 逐表切换（Activate + 该表热词/HR 规则 + 该表读音词表），识别结果只落在本表行列范围内；
//   2) 热词为全表聚合 → 装载一次识别器即可服务所有表，切表**不重建**识别器（快）；
//   3) 会话进行中强制重建识别器两次，在跑会话不受影响（旧识别器由会话保活）。
struct TableSwitchResult {
    bool ok = false;
    std::string error;
    struct Step {
        std::string table;
        int rows = 0;
        int cols = 0;
        std::string finalText;
        std::vector<std::string> cells;  // "row|col|value|raw"
        bool cellsWithinTable = false;   // 全部命中是否都落在本表范围内
        bool recognizerRebuilt = false;  // 本步是否触发了识别器重建
    };
    std::vector<Step> steps;
    bool forcedRebuildMidSession = false;  // 是否在会话中途强制重建过识别器
};

TableSwitchResult RunTableSwitchTest(const std::string& pcmPath, const std::string& dataDir,
                                     const std::vector<std::string>& tables);

}  // namespace vta
