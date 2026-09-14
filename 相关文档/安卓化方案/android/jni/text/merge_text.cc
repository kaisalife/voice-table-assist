// jni/text/merge_text.cc —— 翻译自 VoiceInteractionSession.MergeText
#include "merge_text.h"

#include "../common/strings.h"

namespace vta {

std::string MergeStreamingText(const std::string& prev, const std::string& cur) {
    if (prev.empty()) return cur;
    if (cur.empty() || prev.find(cur) != std::string::npos) return prev;
    if (cur.find(prev) != std::string::npos) return cur;

    // 重叠拼接：prev 尾部与 cur 头部最长 >=2 码点重叠
    auto prevCps = Utf8ToCodePoints(prev);
    auto curCps = Utf8ToCodePoints(cur);
    size_t limit = prevCps.size() < curCps.size() ? prevCps.size() : curCps.size();
    for (size_t overlap = limit; overlap >= 2; --overlap) {
        bool eq = true;
        for (size_t i = 0; i < overlap; ++i) {
            if (prevCps[prevCps.size() - overlap + i] != curCps[i]) { eq = false; break; }
        }
        if (eq) {
            std::string out = prev;
            for (size_t i = overlap; i < curCps.size(); ++i) AppendUtf8(curCps[i], &out);
            return out;
        }
    }
    return prev + " " + cur;
}

}  // namespace vta
