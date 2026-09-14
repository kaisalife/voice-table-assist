// jni/text/pinyin_table.cc
#include "pinyin_table.h"

#include "../common/mutex_compat.h"
#include "../common/log.h"

namespace vta {

namespace {

std::mutex g_mu;
std::unordered_map<std::string, std::shared_ptr<const PinyinTable>> g_cache;

// "ying4" → "ying"；"lv4" → "lv"；顺带去掉末尾轻声标记
std::string StripTone(std::string s) {
    while (!s.empty()) {
        unsigned char c = static_cast<unsigned char>(s.back());
        if (c >= '0' && c <= '9') {
            s.pop_back();
            continue;
        }
        break;
    }
    return s;
}

std::vector<std::string> SplitWs(const std::string& s) {
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
    if (!cur.empty()) parts.push_back(std::move(cur));
    return parts;
}

}  // namespace

std::shared_ptr<const PinyinTable> PinyinTable::Get(const std::string& charPinyinPath) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_cache.find(charPinyinPath);
    if (it != g_cache.end()) return it->second;

    auto table = std::make_shared<PinyinTable>();
    size_t loaded = 0;
    for (const auto& raw : ReadFileLines(charPinyinPath)) {
        std::string line = Trim(raw);
        if (line.empty() || line[0] == '#') continue;
        std::string ch, pinyin;
        size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0) {
            ch = Trim(line.substr(0, eq));
            pinyin = Trim(line.substr(eq + 1));
        } else {
            // 兼容空格分隔：首列单字，其余列拼接
            auto parts = SplitWs(line);
            if (parts.size() < 2) continue;
            ch = parts[0];
            for (size_t i = 1; i < parts.size(); ++i) pinyin += parts[i];
        }
        auto cps = Utf8ToCodePoints(ch);
        if (cps.size() != 1 || pinyin.empty()) continue;
        table->map_[cps[0]] = StripTone(pinyin);
        ++loaded;
    }
    if (loaded == 0) {
        ALOGW("[PY] 拼音表为空或无法解析: %s", charPinyinPath.c_str());
        g_cache[charPinyinPath] = nullptr;
        return nullptr;
    }
    ALOGI("[PY] 拼音表加载完成: %zu 字 (%s)", loaded, charPinyinPath.c_str());
    g_cache[charPinyinPath] = table;
    return table;
}

std::string PinyinTable::Syllable(CodePoint cp) const {
    auto it = map_.find(cp);
    return it == map_.end() ? std::string() : it->second;
}

std::vector<std::string> PinyinTable::Syllables(const std::string& text) const {
    std::vector<std::string> out;
    for (CodePoint cp : Utf8ToCodePoints(text)) out.push_back(Syllable(cp));
    return out;
}

}  // namespace vta
