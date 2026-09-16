namespace VoiceTableAssist.Asr;

/// <summary>
/// 领域后处理纠错（对齐《新语音输入同音解决方案.md》与 Android 版 jni/homophone/domain_correction.cc）。
///
/// 纠错分层：
///   ① 表内读音吸附（<see cref="SoundAligner"/>）：主体/位置按读音吸附到**本表词表**——覆盖全部"表相关同音"
///      （水卫→水位、印度→硬度、外景→外径、二好→二号…），零人工维护、换表自动生效。
///   ② 本类只保留**与表无关的通用数字同音归一**：值里的"十/零/负"等被听错时纠正。
///      ——吸附管不到值部分（值不是表内词），而这些字是中文数字的通用近音，与表无关。
///
/// 已删除：历史上写死的"表相关同音清单"（印度→硬度、外景→外径、员工→圆度、测量直→测量值…）
/// 与列描述符相关的手写规则（好→号）——这些现在由 ① 自动覆盖。
/// </summary>
internal static class DomainCorrection
{
    /// <summary>对 ASR 识别文本执行通用数字同音归一，返回修正后的文本。</summary>
    public static string Correct(string text)
    {
        if (string.IsNullOrWhiteSpace(text)) return text;

        var chars = text.ToCharArray();
        for (var i = 0; i < chars.Length; i++)
        {
            var canonical = CanonicalNumeral(chars[i]);
            if (canonical == '\0') continue;
            // 仅在数字上下文（前或后一个字符属于数字字符集）才归一，防止"实在/时间"被误改
            var prevNum = i > 0 && IsNumeralFamily(chars[i - 1]);
            var nextNum = i + 1 < chars.Length && IsNumeralFamily(chars[i + 1]);
            if (prevNum || nextNum) chars[i] = canonical;
        }
        return new string(chars);
    }

    /// <summary>数字字符全集（用于判断"是否处于数字上下文"）。</summary>
    private static bool IsNumeralFamily(char c) => c switch
    {
        '零' or '〇' or '一' or '二' or '三' or '四' or '五' or '六' or '七' or '八' or '九'
            or '十' or '百' or '千' or '点' or '。' or '负' => true,
        _ => c is >= '0' and <= '9',
    };

    /// <summary>通用数字同音：只收**声调也相同**的真同音字（保守，避免误纠）；非目标字返回 '\0'。</summary>
    private static char CanonicalNumeral(char c) => c switch
    {
        '实' or '石' or '时' => '十',   // shí
        '灵' => '零',                   // líng
        '付' => '负',                   // fù
        '寺' => '四',                   // sì
        '巴' => '八',                   // bā
        '久' => '九',                   // jiǔ
        '午' or '武' => '五',           // wǔ
        '迁' => '千',                   // qiān
        _ => '\0',
    };
}
