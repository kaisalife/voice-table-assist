// jni/text/pinyin_table.h —— 汉字→去声调拼音（通用字典，随包内置，与表无关）
//
// 文件格式：每行 "汉字=ying4"（TONE3，音节末尾数字）；加载时统一剥掉声调数字，
// 得到"去声调"形式（ying），供音近对齐使用。旧格式（空格分隔）也兼容。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/strings.h"

namespace vta {

class PinyinTable {
public:
    // 进程内单例（按路径缓存，避免多会话重复加载 0.4MB 字典）；加载失败返回 nullptr
    static std::shared_ptr<const PinyinTable> Get(const std::string& charPinyinPath);

    // 单字去声调音节（如 硬→ying、一→yi）；非汉字/未收录返回空串
    std::string Syllable(CodePoint cp) const;

    // 整串逐字音节（非汉/未收录 → 空串占位，长度与码点数一致）
    std::vector<std::string> Syllables(const std::string& text) const;

    size_t Size() const { return map_.size(); }

private:
    std::unordered_map<CodePoint, std::string> map_;
};

}  // namespace vta
