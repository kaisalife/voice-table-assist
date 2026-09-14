// jni/homophone/homophone_replacer.cc —— 翻译自 Asr/HomophoneReplacer.cs
#include "homophone_replacer.h"

#include <sstream>

#include "../common/log.h"
#include "../common/strings.h"

namespace vta {

HomophoneReplacer::HomophoneReplacer(const std::string& charPinyinPath,
                                     const std::vector<std::string>& ruleLines) {
    if (!charPinyinPath.empty()) {
        for (const auto& raw : ReadFileLines(charPinyinPath)) {
            std::string s = Trim(raw);
            if (s.empty() || s[0] == '#') continue;
            size_t eq = s.find('=');
            if (eq != std::string::npos && eq > 0) {
                std::string ch = Trim(s.substr(0, eq));
                std::string pinyin = Trim(s.substr(eq + 1));
                auto cps = Utf8ToCodePoints(ch);
                if (cps.size() == 1 && !pinyin.empty()) _charPinyin[cps[0]] = pinyin;
            } else {
                // 空格/制表符分隔：首列单字，其余列拼接为拼音
                std::vector<std::string> parts;
                std::string cur;
                for (char c : s) {
                    if (c == ' ' || c == '\t') {
                        if (!cur.empty()) parts.push_back(std::move(cur));
                        cur.clear();
                    } else {
                        cur.push_back(c);
                    }
                }
                if (!cur.empty()) parts.push_back(cur);
                if (parts.size() >= 2) {
                    auto cps = Utf8ToCodePoints(parts[0]);
                    if (cps.size() == 1) {
                        std::string pinyin;
                        for (size_t i = 1; i < parts.size(); ++i) pinyin += parts[i];
                        _charPinyin[cps[0]] = pinyin;
                    }
                }
            }
        }
    }

    for (const auto& raw : ruleLines) {
        std::string s = Trim(raw);
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string key = Trim(s.substr(0, eq));
        std::string val = Trim(s.substr(eq + 1));
        if (!key.empty() && !val.empty()) _rules[key] = val;
    }
}

std::string HomophoneReplacer::ToTone3Pinyin(const std::string& chinese) const {
    std::string sb;
    for (CodePoint cp : Utf8ToCodePoints(chinese)) {
        auto it = _charPinyin.find(cp);
        if (it != _charPinyin.end()) sb += it->second;
        else AppendUtf8(cp, &sb);
    }
    return sb;
}

std::string HomophoneReplacer::Apply(const std::string& text) const {
    if (Trim(text).empty() || _rules.empty()) return text;

    auto cps = Utf8ToCodePoints(text);
    size_t n = cps.size();

    // 分词：每个汉字为独立 token；连续非汉字为一段 token（作匹配边界）
    struct Tok {
        size_t start;   // 码点下标
        size_t len;     // 码点数
        bool isHan;
        std::string pinyin;  // 汉字 token 的拼音
    };
    std::vector<Tok> toks;
    size_t i = 0;
    while (i < n) {
        if (!IsHan(cps[i])) {
            size_t j = i;
            while (j < n && !IsHan(cps[j])) ++j;
            toks.push_back({i, j - i, false, {}});
            i = j;
        } else {
            Tok t{i, 1, true, {}};
            auto it = _charPinyin.find(cps[i]);
            // 逐字转拼音；无表回退原字（等价 C# ToPinyinChar）
            t.pinyin = it != _charPinyin.end() ? it->second : CodePointsToUtf8({cps[i]});
            toks.push_back(std::move(t));
            i += 1;
        }
    }

    std::string output;
    i = 0;
    while (i < toks.size()) {
        if (!toks[i].isHan) {
            // 非汉字段整段保留（等价 C# text.Substring(starts[i], lens[i])）
            for (size_t k = 0; k < toks[i].len; ++k)
                AppendUtf8(cps[toks[i].start + k], &output);
            ++i;
            continue;
        }

        // 从 i 开始跨连续的汉字 token 累积拼音，找到最长命中的规则键
        std::string acc;
        size_t j = i;
        std::string target;
        size_t endTok = std::string::npos;
        while (j < toks.size() && toks[j].isHan) {
            acc += toks[j].pinyin;
            auto it = _rules.find(acc);
            if (it != _rules.end()) {
                target = it->second;
                endTok = j;
                // 是否存在以当前 key 为前缀的更长规则键（贪心最长匹配）
                bool hasLonger = false;
                for (const auto& [k, v] : _rules) {
                    if (k.size() > acc.size() && k.compare(0, acc.size(), acc) == 0) {
                        hasLonger = true;
                        break;
                    }
                }
                if (!hasLonger) break;
            }
            ++j;
        }

        if (!target.empty() && endTok != std::string::npos && endTok >= i) {
            output += target;
            i = endTok + 1;
        } else {
            for (size_t k = 0; k < toks[i].len; ++k) AppendUtf8(cps[toks[i].start + k], &output);
            ++i;
        }
    }

    return output;
}

}  // namespace vta
