// jni/text/cell_phrase_generator.cc —— 翻译自 Services/CellPhraseGenerator.cs
#include "cell_phrase_generator.h"

#include <cstdio>
#include <unordered_set>

#include "../common/strings.h"

namespace vta {

namespace {
const char* kChDigits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十"};
}  // namespace

std::string ToChineseNum(int n) {
    char buf[16];
    if (n <= 0) {
        std::snprintf(buf, sizeof(buf), "%d", n);
        return buf;
    }
    if (n <= 10) return kChDigits[n];
    if (n < 20) return std::string("十") + (n % 10 == 0 ? "" : kChDigits[n - 10]);
    int tens = n / 10, ones = n % 10;
    return std::string(kChDigits[tens]) + "十" + (ones > 0 ? kChDigits[ones] : "");
}

std::vector<std::string> GenerateCellPhrases(int rowIdx, const std::string& rowLabel, int colIdx) {
    std::string cnRow = ToChineseNum(rowIdx);
    std::string cnCol = ToChineseNum(colIdx);

    // 行描述符：行标签 + 序号几 + 第几行
    std::vector<std::string> rowDescs = {rowLabel, "序号" + cnRow, "第" + cnRow + "行"};

    // 列描述符：几号 + 第几个 + 第几列 + 测量值几
    std::vector<std::string> colDescs = {cnCol + "号", "第" + cnCol + "个", "第" + cnCol + "列",
                                         "测量值" + cnCol};

    // 全部排列：行描述符 + 列描述符，以及反序（「测量值几」不反序——反序条目只会稀释检索）
    std::unordered_set<std::string> phrases;
    std::vector<std::string> out;
    for (const auto& rd : rowDescs) {
        for (const auto& cd : colDescs) {
            std::string a = rd + cd;
            if (phrases.insert(a).second) out.push_back(a);
            if (cd.rfind("测量值", 0) != 0) {
                std::string b = cd + rd;
                if (phrases.insert(b).second) out.push_back(b);
            }
        }
    }
    return out;
}

}  // namespace vta
