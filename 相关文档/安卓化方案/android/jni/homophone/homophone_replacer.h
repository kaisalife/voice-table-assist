// jni/homophone/homophone_replacer.h —— 翻译自 Asr/HomophoneReplacer.cs
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "../common/strings.h"

namespace vta {

// 原生同音字/专名替换器，复刻 sherpa-onnx HomophoneReplacer 语义（免 pynini / 免 replace.fst）：
// 1) 用「汉字→带调拼音」表（hr_char_pinyin.txt）把识别文本逐字转拼音；
// 2) 在整串拼音上做「整串拼音=汉字」的短语替换，未命中部分原样保留。
// 键 = 误识别文本的拼音（TONE3，无声调标记时低声用1声，无空格）；值 = 正确汉字。
class HomophoneReplacer {
public:
    // charPinyinPath：每行 "汉字=拼音"（或空格分隔）的紧凑拼音表。
    // ruleLines：规则（每行 拼音=汉字），可选。
    HomophoneReplacer(const std::string& charPinyinPath,
                      const std::vector<std::string>& ruleLines = {});

    // 是否有可用规则/拼音表（用于判定是否启用纠正）
    bool Enabled() const { return !_charPinyin.empty(); }
    bool HasRules() const { return !_rules.empty(); }

    // 对 ASR 识别文本执行同音字/专名替换，返回纠正后的文本。
    std::string Apply(const std::string& text) const;

    // 把一个汉字串转成无空格带调拼音串（逐字查表）。用于为本表行标签生成规则键。
    std::string ToTone3Pinyin(const std::string& chinese) const;

private:
    std::unordered_map<CodePoint, std::string> _charPinyin;
    std::unordered_map<std::string, std::string> _rules;  // 整串拼音 -> 汉字
};

}  // namespace vta
