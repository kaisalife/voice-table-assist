// jni/tables/voice_resource.h —— 翻译自 Asr/TableVoiceResourceGenerator.cs（热词部分）
#pragma once

#include <string>
#include <vector>

namespace vta {

// 按导入的表格模板生成热词串（纯文本、表驱动，自动生成、零维护）：
// 行标签 + 按列数枚举的位置词（+ 可选单字数字）。运行时通过
// CreateStream(hotwords) 按流传入，识别器常驻不重建、切表不混用。
// 注：现网 C# 另会生成 hotwords.txt / hr_rules.txt 文件供 hotwords_file /
// HomophoneReplacer 使用——安卓端热词走按流传入，同音纠错由表内读音吸附
// （SoundAligner）承担，这两类文件已不再生成。
class TableVoiceResourceGenerator {
public:
    // 固定列描述符短语族（测量值X / X号 / 第X个 / 第X列 / 序号X / X号列）。
    static std::vector<std::string> ColumnDescriptors(int columnCount);

    // 生成热词文本：行标签 + 位置词（逐字空格分隔；含非汉字的短语跳过）。
    // includeDigits=true（默认）时追加单字数字（零…九/点）。
    // includeTen=true 时额外追加"十"（BBPE 模型上与"点"抢分，默认不写；换模型后可 A/B）。
    static std::string BuildHotWords(const std::vector<std::string>& rows, int columnCount,
                                     bool includeDigits = true, bool includeTen = false);

    // 生成 per-stream 热词串：内容同上，多个短语用 / 分隔（sherpa CreateStream(hotwords) 格式）。
    // 用途：识别器常驻不重建，每张表开会话时把自己的热词按流传入。
    static std::string BuildHotWordsStream(const std::vector<std::string>& rows, int columnCount,
                                           bool includeDigits = true, bool includeTen = false);
};

}  // namespace vta
