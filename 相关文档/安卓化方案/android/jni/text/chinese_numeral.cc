// jni/text/chinese_numeral.cc —— 翻译自 Services/ChineseNumeral.cs
#include "chinese_numeral.h"

#include <cmath>
#include <cstdio>
#include <unordered_map>

#include "../common/strings.h"

namespace vta {

namespace {

const std::unordered_map<CodePoint, int>& DigitMap() {
    static const std::unordered_map<CodePoint, int> dig = {
        {0x96F6, 0},  // 零
        {0x3007, 0},  // 〇
        {0x4E00, 1},  // 一
        {0x4E8C, 2},  // 二
        {0x4E09, 3},  // 三
        {0x56DB, 4},  // 四
        {0x4E94, 5},  // 五
        {0x516D, 6},  // 六
        {0x4E03, 7},  // 七
        {0x516B, 8},  // 八
        {0x4E5D, 9},  // 九
    };
    return dig;
}

inline bool IsNumeralChar(CodePoint cp) {
    switch (cp) {
        case 0x96F6:  // 零
        case 0x3007:  // 〇
        case 0x4E00:  // 一
        case 0x4E8C:  // 二
        case 0x4E09:  // 三
        case 0x56DB:  // 四
        case 0x4E94:  // 五
        case 0x516D:  // 六
        case 0x4E03:  // 七
        case 0x516B:  // 八
        case 0x4E5D:  // 九
        case 0x5341:  // 十
        case 0x767E:  // 百
        case 0x5343:  // 千
        case 0x70B9:  // 点
        case 0x3002:  // 。
            return true;
        default:
            return false;
    }
}

}  // namespace

double ChineseNumeralToDecimal(const std::string& raw) {
    // 正则：负?[零〇一二三四五六七八九十百千点。]+ —— 取最左最长匹配
    auto cps = Utf8ToCodePoints(raw);
    size_t start = std::string::npos, end = 0;
    for (size_t i = 0; i < cps.size(); ++i) {
        bool negHere = cps[i] == 0x8D1F /* 负 */ && i + 1 < cps.size() && IsNumeralChar(cps[i + 1]);
        if (negHere || IsNumeralChar(cps[i])) {
            size_t s = i;
            if (negHere) ++i;
            size_t e = i;
            while (e < cps.size() && IsNumeralChar(cps[e])) ++e;
            start = s;
            end = e;
            break;
        }
    }
    if (start == std::string::npos) return 0;

    bool neg = cps[start] == 0x8D1F;  // 「负九十五」→ -95（巡检负压/真空场景）
    if (neg) ++start;

    const auto& dig = DigitMap();
    // 小数点位置（'点' / '。'）
    size_t dot = std::string::npos;
    for (size_t i = start; i < end; ++i) {
        if (cps[i] == 0x70B9 || cps[i] == 0x3002) { dot = i; break; }
    }
    size_t intEnd = dot == std::string::npos ? end : dot;

    // 中文整数累加解析（支持 千/百/十/个，最大到"一千"=1000）
    int result = 0, hold = 0;
    for (size_t i = start; i < intEnd; ++i) {
        auto it = dig.find(cps[i]);
        if (it != dig.end()) { hold = it->second; continue; }
        int now = hold == 0 ? 1 : hold;  // "十二"→十位补1
        switch (cps[i]) {
            case 0x5341: result += now * 10; hold = 0; break;    // 十
            case 0x767E: result += now * 100; hold = 0; break;   // 百
            case 0x5343: result += now * 1000; hold = 0; break;  // 千
            default: break;
        }
    }
    result += hold;
    if (result < 0 || result > 1000) return 0;

    double v = result;
    if (dot != std::string::npos && dot + 1 < end) {
        double f = 0;
        size_t fracLen = end - (dot + 1);
        size_t take = fracLen < 2 ? fracLen : 2;
        for (size_t k = 0; k < take; ++k) {
            auto it = dig.find(cps[dot + 1 + k]);
            int d = it != dig.end() ? it->second : 0;
            f += d * (k == 0 ? 0.1 : 0.01);
        }
        v += f;
    }
    if (neg) v = -v;
    return std::round(v * 100.0) / 100.0;
}

std::string FormatCellValue(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

}  // namespace vta
