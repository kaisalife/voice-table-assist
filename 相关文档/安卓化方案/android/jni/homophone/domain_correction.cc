// jni/homophone/domain_correction.cc
//
// 纠错分层（对齐《新语音输入同音解决方案.md》）：
//   ① 表内读音吸附（SoundAligner）：主体/位置 按读音吸附到**本表词表**——覆盖全部"表相关同音"
//      （水卫→水位、印度→硬度、外景→外径、二好→二号…），零人工维护、换表自动生效。
//   ② 本文件只保留**与表无关的通用数字同音归一**：值里的"十/零/负"等被听错时纠正。
//      ——吸附管不到值部分（值不是表内词），而这些字是中文数字的通用近音，与表无关。
//
// 已删除：历史上写死的"表相关同音清单"（印度→硬度、外景→外径、员工→圆度、测量直→测量值…）
// 与列描述符相关的手写规则（好→号）——这些现在由 ① 自动覆盖。
#include "domain_correction.h"

#include "../common/strings.h"

namespace vta {

namespace {

// 数字字符全集（用于判断"是否处于数字上下文"）
inline bool IsNumeralFamily(CodePoint cp) {
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
        case 0x8D1F:  // 负
            return true;
        default:
            return IsAsciiDigit(cp);
    }
}

// 通用数字同音：只收**声调也相同**的真同音字（保守，避免误纠）
CodePoint CanonicalNumeral(CodePoint cp) {
    switch (cp) {
        case 0x5B9E:  // 实 shí
        case 0x77F3:  // 石 shí
        case 0x65F6:  // 时 shí
            return 0x5341;  // 十
        case 0x7075:  // 灵 líng
            return 0x96F6;  // 零
        case 0x4ED8:  // 付 fù
            return 0x8D1F;  // 负
        case 0x5BFA:  // 寺 sì
            return 0x56DB;  // 四
        case 0x5DF4:  // 巴 bā
            return 0x516B;  // 八
        case 0x4E45:  // 久 jiǔ
            return 0x4E5D;  // 九
        case 0x5348:  // 午 wǔ
        case 0x6B66:  // 武 wǔ
            return 0x4E94;  // 五
        case 0x8FC1:  // 迁 qiān
            return 0x5343;  // 千
        default:
            return 0;
    }
}

}  // namespace

std::string DomainCorrect(const std::string& text) {
    if (Trim(text).empty()) return text;
    auto cps = Utf8ToCodePoints(text);
    const size_t n = cps.size();
    for (size_t i = 0; i < n; ++i) {
        CodePoint canon = CanonicalNumeral(cps[i]);
        if (canon == 0) continue;
        // 仅在数字上下文（前或后一个字符属于数字字符集）才归一，防止"实在/时间"被误改
        bool prevNum = (i > 0) && IsNumeralFamily(cps[i - 1]);
        bool nextNum = (i + 1 < n) && IsNumeralFamily(cps[i + 1]);
        if (prevNum || nextNum) cps[i] = canon;
    }
    return CodePointsToUtf8(cps);
}

}  // namespace vta
