// jni/tables/voice_resource.h —— 翻译自 Asr/TableVoiceResourceGenerator.cs
#pragma once

#include <string>
#include <vector>

namespace vta {

// 按"导入的表格模板"生成两块语音特化资源（纯文本、由表驱动）：
//  1. hotwords.txt —— 送入 sherpa-onnx hotwords_file（解码 bias），含行标签 + 列描述符；
//  2. hr_rules.txt —— 送入 HomophoneReplacer（拼音=汉字），把同音错字纠正为本表行标签。
class TableVoiceResourceGenerator {
public:
    // 固定列描述符短语族（测量值X / X号 / 第X个 / 第X列 / 序号X / X号列）。
    static std::vector<std::string> ColumnDescriptors(int columnCount);

    // 生成热词文本：行标签 + 位置词（逐字空格分隔；含非汉字的短语跳过）。
    // includeDigits=true（默认，= 现网 C# 行为）时追加单字数字（零…十/点/零）。
    // 51 条音频 × 3 配置离线 A/B 实测：含与不含仅 1/51 不同（同音字，吸附可纠），
    // 既非纠错手段、也非"点丢失"的原因（见《新语音输入同音解决方案.md》§数值识别边界）。
    static std::string BuildHotWords(const std::vector<std::string>& rows, int columnCount,
                                     bool includeDigits = true);

    // 生成 per-stream 热词串：内容同上，多个短语用 / 分隔（sherpa CreateStream(hotwords) 格式）。
    // 用途：识别器常驻不重建，每张表开会话时把自己的热词按流传入。
    static std::string BuildHotWordsStream(const std::vector<std::string>& rows, int columnCount,
                                           bool includeDigits = true);

    // 生成同音替换规则文本（每行 拼音=汉字）：行标签 + 固定列描述符恒等规则 + 通用近音规则。
    static std::string BuildRules(const class HomophoneReplacer& lexicon,
                                  const std::vector<std::string>& rows,
                                  const std::string& commonRulesPath, int columnCount);

    // 依据配置重建语音资源到指定表目录，并聚合全部表热词到 current/hotwords.txt。
    // 返回聚合后的热词文件路径（recognizer 热加载用）；失败返回空串。
    static std::string Rebuild(const std::string& charPinyinPath, const std::string& commonRulesPath,
                               const std::string& hrTablesRoot, const std::string& tableDir,
                               const std::vector<std::string>& rows, int columnCount);

private:
    // 合并 tables/ 下所有表的 hotwords.txt（去重）写入 sherpa 加载的 current 目录。
    static void AggregateHotwords(const std::string& tablesRoot, const std::string& currentDir);
};

}  // namespace vta
