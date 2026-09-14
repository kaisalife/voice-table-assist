// jni/text/triple_extractor.h —— 翻译自 Services/TripleExtractor.cs
#pragma once

#include <string>
#include <vector>

namespace vta {

struct Triple {
    std::string sub;
    std::string obj;
    std::string val;
};

// 从 RaNER 输出的 BIO 序列提取三元组 (SUB, OBJ, VAL)。
// bio[i] = (字符, 标签)，标签 ∈ {O, B-SUB, I-SUB, B-OBJ, I-OBJ, B-VAL, I-VAL}
std::vector<Triple> ExtractTriples(const std::vector<std::pair<std::string, std::string>>& bio);

}  // namespace vta
