// jni/text/sound_aligner.cc —— 表内读音对齐（详见 相关文档/新语音输入同音解决方案.md）
#include "sound_aligner.h"

#include <algorithm>
#include <cstdlib>

#include "../common/log.h"
#include "../tables/voice_resource.h"
#include "pinyin_table.h"

namespace vta {

namespace {

// 声母集合（按长度优先匹配）
const char* kInitials2[] = {"zh", "ch", "sh"};
const char* kInitials1[] = {"b", "p", "m", "f", "d", "t", "n", "l", "g",
                            "k", "h", "j", "q", "x", "r", "z", "c", "s", "y", "w"};

// 声母拆解：返回 {initial, final}
std::pair<std::string, std::string> SplitSyllable(const std::string& s) {
    for (const char* ini : kInitials2) {
        if (s.size() > 2 && s.compare(0, 2, ini) == 0) return {s.substr(0, 2), s.substr(2)};
    }
    for (const char* ini : kInitials1) {
        if (s.size() > 1 && s[0] == ini[0]) return {s.substr(0, 1), s.substr(1)};
    }
    return {"", s};  // 零声母（a/o/e 开头）
}

// 通用音近：声母等价类（平翘舌、n-l、f-h）
std::string NormalizeInitial(const std::string& ini) {
    if (ini == "zh") return "z";
    if (ini == "ch") return "c";
    if (ini == "sh") return "s";
    if (ini == "n") return "l";
    if (ini == "f") return "h";
    return ini;
}

// 通用音近：前鼻音/后鼻音（末尾 ng ↔ n）
std::string NormalizeFinal(const std::string& fin) {
    if (fin.size() >= 2 && fin.compare(fin.size() - 2, 2, "ng") == 0) return fin.substr(0, fin.size() - 1);
    return fin;
}

int EditDistance(const std::string& a, const std::string& b) {
    const size_t n = a.size(), m = b.size();
    if (n == 0) return static_cast<int>(m);
    if (m == 0) return static_cast<int>(n);
    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= m; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev = cur;
    }
    return prev[m];
}

inline bool IsNumeral(CodePoint cp) {
    switch (cp) {
        case 0x96F6: case 0x3007: case 0x4E00: case 0x4E8C: case 0x4E09: case 0x56DB:
        case 0x4E94: case 0x516D: case 0x4E03: case 0x516B: case 0x4E5D: case 0x5341:
        case 0x767E: case 0x5343: case 0x70B9: case 0x3002: case 0x8D1F:  // 负
            return true;
        default:
            return false;
    }
}

// 全汉字才算候选片段（值/标点段不参与替换）
bool AllHan(const std::vector<CodePoint>& cps, int start, int len) {
    for (int i = start; i < start + len; ++i) {
        if (!IsHan(cps[i])) return false;
    }
    return true;
}

bool AllNumeral(const std::vector<CodePoint>& cps, int start, int len) {
    for (int i = start; i < start + len; ++i) {
        if (!IsNumeral(cps[i])) return false;
    }
    return true;
}

}  // namespace

double SoundAligner::SyllableSim(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return 0.0;
    if (a == b) return 1.0;

    auto [ai, af] = SplitSyllable(a);
    auto [bi, bf] = SplitSyllable(b);
    // 通用音近：声母等价 + 韵母（去鼻音）相同 → 0.9
    if (NormalizeInitial(ai) == NormalizeInitial(bi) && NormalizeFinal(af) == NormalizeFinal(bf))
        return 0.9;
    // 音节编辑距离 1 → 0.6（如 yin/y i、ying/ying 的细微差异）
    if (EditDistance(a, b) <= 1) return 0.6;
    return 0.0;
}

double SoundAligner::SequenceSim(const std::vector<std::string>& a,
                                 const std::vector<std::string>& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        double s = SyllableSim(a[i], b[i]);
        if (s <= 0.0) return 0.0;  // 有一个音节完全不像 → 整体不认
        sum += s;
    }
    return sum / static_cast<double>(a.size());
}

SoundAligner::SoundAligner(std::shared_ptr<const PinyinTable> pinyin,
                           std::vector<std::string> rowLabels, int columnCount)
    : pinyin_(std::move(pinyin)) {
    auto addWord = [&](const std::string& raw) {
        std::string w = Trim(raw);
        if (w.empty()) return;
        // 词表只收纯汉字（含 ASCII 数字/单位的写法 ASR 不会输出，且无读音可比）
        for (CodePoint cp : Utf8ToCodePoints(w)) {
            if (!IsHan(cp)) return;
        }
        for (const auto& e : words_) {
            if (e.text == w) return;  // 去重
        }
        Entry e;
        e.text = w;
        e.cps = Utf8ToCodePoints(w);
        e.syllables = pinyin_ ? pinyin_->Syllables(w) : std::vector<std::string>{};
        if (e.syllables.empty()) return;
        for (const auto& s : e.syllables) {
            if (s.empty()) return;  // 有字不在字典里 → 该词不可用（避免半截匹配）
        }
        words_.push_back(std::move(e));
    };

    for (const auto& r : rowLabels) addWord(r);
    if (columnCount > 0) {
        for (const auto& d : TableVoiceResourceGenerator::ColumnDescriptors(columnCount)) addWord(d);
    }
    for (const auto& e : words_) vocabText_.push_back(e.text);
    ALOGI("[ALIGN] 词表构建完成：%zu 条（行标签 %zu + 列说法），音近对齐%s", words_.size(),
          rowLabels.size(), Ready() ? "已启用" : "未启用（缺拼音表）");
    // 词表不大时全量打印，便于现场核对"到底吸附了哪些词"（含位置词：几号/第几个/测量值几…）
    if (words_.size() <= 80) {
        std::string all;
        for (size_t i = 0; i < vocabText_.size(); ++i) {
            all += vocabText_[i];
            if (i + 1 < vocabText_.size()) all += "、";
        }
        ALOGI("[ALIGN] 词表内容：%s", all.c_str());
    }
}

void SoundAligner::TopTwo(const std::vector<std::string>& spanSyl, std::string* bestText,
                          double* bestScore, double* secondScore) const {
    *bestText = "";
    *bestScore = 0.0;
    *secondScore = 0.0;
    for (const auto& e : words_) {
        if (e.syllables.size() != spanSyl.size()) continue;
        double sc = SequenceSim(spanSyl, e.syllables);
        if (sc > *bestScore) {
            *secondScore = *bestScore;
            *bestScore = sc;
            *bestText = e.text;
        } else if (sc > *secondScore) {
            *secondScore = sc;
        }
    }
}

SoundAligner::Match SoundAligner::AlignWord(const std::string& word) const {
    Match m;
    if (!Ready()) return m;
    std::string w = Trim(word);
    // 已经就是表内词 → 无需吸附
    for (const auto& e : words_) {
        if (e.text == w) {
            m.canonical = w;
            m.score = 1.0;
            m.margin = 1.0;
            return m;
        }
    }
    auto syl = pinyin_->Syllables(w);
    if (syl.empty()) return m;
    for (const auto& s : syl) {
        if (s.empty()) return m;  // 有字不在字典里 → 不猜
    }
    std::string bestText;
    double best = 0, second = 0;
    TopTwo(syl, &bestText, &best, &second);
    m.score = best;
    m.margin = best - second;
    if (best >= thr_ && m.margin >= margin_ && !bestText.empty()) m.canonical = bestText;
    return m;
}

std::string SoundAligner::AlignSentence(const std::string& text) const {
    if (!Ready() || text.empty()) return text;
    auto cps = Utf8ToCodePoints(text);
    const int n = static_cast<int>(cps.size());

    struct Cand {
        int start;
        int len;
        double score;
        std::string canonical;
        std::string original;
    };
    std::vector<Cand> cands;

    // 保护区：与词表**精确相等**的片段一律不参与替换。
    // （否则"一号"会被读音相近的"四号"替换——yihao/sihao 编辑距离 1）
    std::vector<int> protectedAt;  // 与 cps 等长：>0 表示该位置已被某个精确命中的词覆盖
    protectedAt.assign(n, 0);
    for (const auto& e : words_) {
        const int len = static_cast<int>(e.cps.size());
        if (len <= 0 || len > n) continue;
        for (int i = 0; i + len <= n; ++i) {
            if (std::equal(e.cps.begin(), e.cps.end(), cps.begin() + i)) {
                for (int k = i; k < i + len; ++k) protectedAt[k] = len;
            }
        }
    }

    for (const auto& e : words_) {
        const int len = static_cast<int>(e.syllables.size());
        if (len <= 0 || len > n) continue;
        for (int i = 0; i + len <= n; ++i) {
            if (protectedAt[i] != 0) continue;  // 该处已是表内词（精确）→ 保护区
            if (!AllHan(cps, i, len)) continue;
            if (AllNumeral(cps, i, len)) continue;  // 值段不动
            std::vector<std::string> spanSyl;
            spanSyl.reserve(len);
            bool anyMissing = false;
            for (int k = 0; k < len; ++k) {
                std::string s = pinyin_->Syllable(cps[i + k]);
                if (s.empty()) { anyMissing = true; break; }
                spanSyl.push_back(std::move(s));
            }
            if (anyMissing) continue;
            double sc = SequenceSim(spanSyl, e.syllables);
            if (sc < thr_) continue;
            std::string span = CodePointsToUtf8(std::vector<CodePoint>(cps.begin() + i,
                                                                       cps.begin() + i + len));
            if (span == e.text) continue;  // 本来就对，不用替换
            cands.push_back({i, len, sc, e.text, span});
        }
    }
    if (cands.empty()) return text;

    // 同一片段（start,len）取前二名做差距守卫
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.len != b.len) return a.len > b.len;
        return a.score > b.score;
    });
    std::vector<Cand> accepted;
    for (size_t i = 0; i < cands.size();) {
        size_t j = i;
        double second = 0.0;
        while (j < cands.size() && cands[j].start == cands[i].start && cands[j].len == cands[i].len) {
            if (j > i) second = std::max(second, cands[j].score);
            ++j;
        }
        const Cand& top = cands[i];
        if (top.score - second >= margin_) accepted.push_back(top);
        i = j;
    }
    if (accepted.empty()) return text;

    // 全局贪心：高分优先、长词优先，避免重叠替换
    std::sort(accepted.begin(), accepted.end(), [](const Cand& a, const Cand& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.len > b.len;
    });
    std::vector<bool> used(n, false);
    std::vector<int> replOf(n, -1);  // 起始位置 → accepted 下标
    std::vector<bool> chosen(accepted.size(), false);
    for (size_t k = 0; k < accepted.size(); ++k) {
        bool overlap = false;
        for (int t = accepted[k].start; t < accepted[k].start + accepted[k].len; ++t) {
            if (used[t] || protectedAt[t] != 0) { overlap = true; break; }
        }
        if (overlap) continue;
        for (int t = accepted[k].start; t < accepted[k].start + accepted[k].len; ++t) used[t] = true;
        replOf[accepted[k].start] = static_cast<int>(k);
        chosen[k] = true;
    }

    // 重建文本
    std::string out;
    for (int i = 0; i < n;) {
        int k = replOf[i];
        if (k >= 0 && chosen[k]) {
            out += accepted[k].canonical;
            ALOGI("[ALIGN] \"%s\" → \"%s\"（相似度 %.2f）", accepted[k].original.c_str(),
                  accepted[k].canonical.c_str(), accepted[k].score);
            i += accepted[k].len;
        } else {
            AppendUtf8(cps[i], &out);
            ++i;
        }
    }
    return out;
}

}  // namespace vta
