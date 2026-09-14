// jni/text/triple_extractor.cc —— 翻译自 Services/TripleExtractor.cs
#include "triple_extractor.h"

namespace vta {

std::vector<Triple> ExtractTriples(const std::vector<std::pair<std::string, std::string>>& bio) {
    std::vector<std::pair<std::string, std::string>> entities;  // (text, type)
    std::string curType;
    std::string curChars;

    auto flushEntity = [&]() {
        if (!curType.empty()) {
            entities.emplace_back(curChars, curType);
            curType.clear();
            curChars.clear();
        }
    };

    for (const auto& [ch, tag] : bio) {
        if (tag.rfind("B-", 0) == 0) {
            flushEntity();
            curType = tag.substr(2);
            curChars = ch;
        } else if (tag.rfind("I-", 0) == 0) {
            std::string t = tag.substr(2);
            if (curType == t) {
                curChars += ch;
            } else {
                flushEntity();
            }
        } else {
            flushEntity();
        }
    }
    flushEntity();

    std::vector<Triple> triples;
    size_t i = 0;
    std::string curSub;  // C# 初始为 null，此处空串等价（IsNullOrEmpty 判断一致）
    while (i < entities.size()) {
        if (entities[i].second == "SUB") {
            curSub = entities[i].first;
            ++i;
            while (i < entities.size() && entities[i].second != "SUB") {
                const auto& type = entities[i].second;
                const auto& text = entities[i].first;
                if (type == "OBJ" && i + 1 < entities.size() && entities[i + 1].second == "VAL") {
                    triples.push_back({curSub, text, entities[i + 1].first});
                    i += 2;
                } else if (type == "OBJ") {
                    triples.push_back({curSub, text, "?"});
                    ++i;
                } else if (type == "VAL") {
                    triples.push_back({curSub, "?", text});
                    ++i;
                } else {
                    ++i;
                }
            }
        } else {
            ++i;
        }
    }
    return triples;
}

}  // namespace vta
