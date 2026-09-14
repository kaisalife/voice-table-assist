// jni/text/cell_phrase_generator.h —— 翻译自 Services/CellPhraseGenerator.cs
#pragma once

#include <string>
#include <vector>

namespace vta {

// 阿拉伯数字 → 中文数字（1~99）
std::string ToChineseNum(int n);

// 为单元格 (rowIdx, colIdx) 生成所有指代短语（行描述符 × 列描述符全排列，含反序）。
std::vector<std::string> GenerateCellPhrases(int rowIdx, const std::string& rowLabel, int colIdx);

}  // namespace vta
