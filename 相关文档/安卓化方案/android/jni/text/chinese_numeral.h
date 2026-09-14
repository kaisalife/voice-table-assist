// jni/text/chinese_numeral.h —— 中文数字 → 阿拉伯数字（翻译自 Services/ChineseNumeral.cs）
#pragma once

#include <string>

namespace vta {

// 范围 0~1000，保留两位小数；无法解析返回 0。raw 例："负九十五" → -95。
double ChineseNumeralToDecimal(const std::string& raw);

// CellHit.value 的字符串格式化：%.2f 后去尾零（"17.00"→"17"，"17.80"→"17.8"）。
std::string FormatCellValue(double v);

}  // namespace vta
