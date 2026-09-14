// jni/tables/voice_resource.cc —— 翻译自 Asr/TableVoiceResourceGenerator.cs
#include "voice_resource.h"

#include <cstdio>
#include <set>

#include "../common/log.h"
#include "../common/strings.h"
#include "../homophone/homophone_replacer.h"
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
                                                             int columnCount, bool includeDigits) {
    // 内容与 BuildHotWords 一致，只是把多行用 / 连成一条串，
    // 供 sherpa CreateStream(hotwords) 按流传入（识别器常驻不重建）。
    std::string text = BuildHotWords(rows, columnCount, includeDigits);
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
                                                       int columnCount, bool includeDigits) {
    // 单字数字加权：**刻意不含"十"**。
    // 现网 C# 写入"零…十"，但实测"十"与"点"在同一段声学上是竞争候选（如「二号一点二」可被听成
    // 「二号十二」）：给"十"同样 +hotwords_score 的 bonus 会把"一+点"的路径压掉，"点"就丢了。
    // 18 条小数用例 A/B（官方 sherpa-onnx，同一模型同一音频，仅改数字集）：
    //   含十(现网) 14/18（一点二/一点五/二点五/一点二 四处变合法数词） → 去十 18/18，且
    //   五十 / 十五点二 / 五十点零 / 二十 / 一百 等含"十"读法**无回归**（它们靠声学即可读对）。
    // "十"的同音（实/石/时）由 DomainCorrect 在文本层归一，不依赖热词。
    static const char* kChDigits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    std::string sb;
    for (const auto& r : rows) AppendPhrase(&sb, Trim(r));
    for (const auto& desc : ColumnDescriptors(columnCount)) AppendPhrase(&sb, desc);

    if (includeDigits) {
        for (const char* ch : kChDigits) sb += std::string(ch) + "\n";
        sb += "点\n";
    }
    return sb;
}

std::string TableVoiceResourceGenerator::BuildRules(const HomophoneReplacer& lexicon,
                                                    const std::vector<std::string>& rows,
                                                    const std::string& commonRulesPath,
                                                    int columnCount) {
    std::set<std::string> seen;
    std::string sb;
    for (const auto& r : rows) {
        std::string label = Trim(r);
        if (label.empty()) continue;
        std::string key = lexicon.ToTone3Pinyin(label);
        if (key.empty() || !seen.insert(key).second) continue;
        sb += key + "=" + label + "\n";
    }

    // 固定列描述符恒等规则：拼音=目标写法
    for (const auto& desc : ColumnDescriptors(columnCount)) {
        std::string key = lexicon.ToTone3Pinyin(desc);
        if (key.empty() || !seen.insert(key).second) continue;
        sb += key + "=" + desc + "\n";
    }

    if (!commonRulesPath.empty() && FileExists(commonRulesPath)) {
        for (const auto& line : ReadFileLines(commonRulesPath))
            if (!Trim(line).empty()) sb += Trim(line) + "\n";
    }
    return sb;
}

std::string TableVoiceResourceGenerator::Rebuild(const std::string& charPinyinPath,
                                                 const std::string& commonRulesPath,
                                                 const std::string& hrTablesRoot,
                                                 const std::string& tableDir,
                                                 const std::vector<std::string>& rows,
                                                 int columnCount) {
    MakeDirs(tableDir);

    // 热词始终生成；hr_rules（同音纠正）仅在拼音表可用时生成，缺失时只走热词加权
    std::string hotWords = BuildHotWords(rows, columnCount);
    if (FileExists(charPinyinPath)) {
        HomophoneReplacer lexicon(charPinyinPath);
        std::string rules = BuildRules(lexicon, rows, commonRulesPath, columnCount);
        WriteFileText(tableDir + "/hotwords.txt", hotWords);
        WriteFileText(tableDir + "/hr_rules.txt", rules);
    } else {
        WriteFileText(tableDir + "/hotwords.txt", hotWords);
        ALOGW("[HR] 拼音表缺失: %s（仅生成热词）", charPinyinPath.c_str());
    }

    // recognizer 只在构建时读一次热词文件，且只加载 current/hotwords.txt 一个文件——
    // 把所有已导入表的热词聚合写入该文件（去重），配合热词热加载，切表时解码偏置覆盖全部行标签。
    AggregateHotwords(hrTablesRoot, hrTablesRoot + "/current");

    return hrTablesRoot + "/current/hotwords.txt";
}

void TableVoiceResourceGenerator::AggregateHotwords(const std::string& tablesRoot,
                                                    const std::string& currentDir) {
    // 遍历 tablesRoot 下两层：tables/<key>/hotwords.txt（default → tables/current 自身）
    std::set<std::string> merged;
    auto collect = [&](const std::string& path) {
        if (!FileExists(path)) return;
        for (const auto& line : ReadFileLines(path)) {
            std::string s = Trim(line);
            if (!s.empty() && s[0] != '#') merged.insert(s);
        }
    };
    collect(tablesRoot + "/current/hotwords.txt");
    for (const auto& sub : ListSubDirs(tablesRoot)) {
        collect(tablesRoot + "/" + sub + "/hotwords.txt");
    }
    MakeDirs(currentDir);
    std::string text;
    for (const auto& line : merged) text += line + "\n";
    WriteFileText(currentDir + "/hotwords.txt", text);
}

}  // namespace vta
