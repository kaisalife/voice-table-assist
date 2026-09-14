// jni/tables/voice_resource.cc —— 翻译自 Asr/TableVoiceResourceGenerator.cs（热词部分）
#include "voice_resource.h"

#include <cstdio>

#include "../common/log.h"
#include "../common/strings.h"
#include "../text/cell_phrase_generator.h"

namespace vta {

namespace {

// 热词行：**逐字空格分隔**（与现网 C# 一致：sherpa 的 modeling_unit=cjkchar 会按字查表）。
// 词表只有汉字，含非汉字（ASCII 数字/字母）的短语模型输出不了，跳过。
void AppendPhrase(std::string* sb, const std::string& phrase) {
    if (phrase.empty()) return;
    auto cps = Utf8ToCodePoints(phrase);
    for (CodePoint cp : cps)
        if (!IsHan(cp)) return;
    for (size_t i = 0; i < cps.size(); ++i) {
        if (i > 0) sb->push_back(' ');
        AppendUtf8(cps[i], sb);
    }
    sb->push_back('\n');
}

}  // namespace

std::vector<std::string> TableVoiceResourceGenerator::ColumnDescriptors(int columnCount) {
    std::vector<std::string> list;
    for (int c = 1; c <= columnCount; ++c) {
        std::string zh = ToChineseNum(c);
        char ar[16];
        std::snprintf(ar, sizeof(ar), "%d", c);
        list.push_back(zh + "号");       // 一号..六号
        list.push_back(std::string(ar) + "号");   // 1号..6号
        list.push_back("第" + zh + "个");  // 第一个..第六个
        list.push_back("第" + std::string(ar) + "个");
        list.push_back("第" + zh + "列");
        list.push_back("第" + std::string(ar) + "列");
        list.push_back("测量值" + zh);    // 测量值一..测量值六
        list.push_back("测量值" + std::string(ar));
        list.push_back("序号" + zh);
        list.push_back("序号" + std::string(ar));
        list.push_back(zh + "号列");     // 一号列..六号列
        list.push_back(std::string(ar) + "号列");
    }
    return list;
}

std::string TableVoiceResourceGenerator::BuildHotWordsStream(const std::vector<std::string>& rows,
                                                             int columnCount, bool includeDigits,
                                                             bool includeTen) {
    // 内容与 BuildHotWords 一致，只是把多行用 / 连成一条串，
    // 供 sherpa CreateStream(hotwords) 按流传入（识别器常驻不重建）。
    std::string text = BuildHotWords(rows, columnCount, includeDigits, includeTen);
    std::string out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        std::string line = Trim(text.substr(start, nl - start));
        if (!line.empty()) {
            if (!out.empty()) out.push_back('/');
            out += line;
        }
        if (nl >= text.size()) break;
        start = nl + 1;
    }
    ALOGI("[HOTWORD] 本表热词(现网同格式)：%zu 字符", out.size());
    return out;
}

std::string TableVoiceResourceGenerator::BuildHotWords(const std::vector<std::string>& rows,
                                                       int columnCount, bool includeDigits,
                                                       bool includeTen) {
    // 单字数字加权：**刻意不含"十"**。
    // 现网 C# 写入"零…十"，但实测"十"与"点"在同一段声学上是竞争候选（如「二号一点二」可被听成
    // 「二号十二」）：给"十"同样 +hotwords_score 的 bonus 会把"一+点"的路径压掉，"点"就丢了。
    // 18 条小数用例 A/B（官方 sherpa-onnx，同一模型同一音频，仅改数字集）：
    //   含十(现网) 14/18（一点二/一点五/二点五/一点二 四处变合法数词） → 去十 18/18，且
    //   五十 / 十五点二 / 五十点零 / 二十 / 一百 等含"十"读法**无回归**（它们靠声学即可读对）。
    // "十"的同音（实/石/时）由 DomainCorrect 在文本层归一，不依赖热词。
    // 字符级模型上"十/点"是否抢分因模型而异 → 用 includeTen 开关 A/B 后再定默认值。
    static const char* kChDigits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    std::string sb;
    for (const auto& r : rows) AppendPhrase(&sb, Trim(r));
    for (const auto& desc : ColumnDescriptors(columnCount)) AppendPhrase(&sb, desc);

    if (includeDigits) {
        for (const char* ch : kChDigits) sb += std::string(ch) + "\n";
        sb += "点\n";
        if (includeTen) sb += "十\n";
    }
    return sb;
}

}  // namespace vta
