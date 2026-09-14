// jni/service/vta_config.h —— 服务配置（vta.json，键名与现网 appsettings.json 对齐）
#pragma once

#include <string>

namespace vta {

struct VtaConfig {
    // ASR（sherpa-onnx 进程内直调；默认即官方 int8 模型，见方案 §13.1）
    std::string asrModelDir = "models/asr/sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30";
    std::string asrEncoder = "encoder.int8.onnx";
    std::string asrDecoder = "decoder.onnx";
    std::string asrJoiner = "joiner.int8.onnx";
    std::string asrTokens = "tokens.txt";
    std::string decodingMethod = "modified_beam_search";
    int asrNumThreads = 4;
    // 建模单元：与 ASR 模型 tokens.txt 一致。
    // ⚠️ 本仓库模型（multi-zh-hans zipformer，BBPE 2000）**发布包不含 bpe.model**，
    //    若设 "bbpe"，sherpa 的 EncodeHotwords 会调用空的 bpe_encoder_ → SIGSEGV。
    //    故默认 "cjkchar"（逐字查表，安全）：热词仅对 tokens.txt 中存在的字生效；
    //    同音兜底由表内读音吸附（SoundAligner）负责。
    //    若拿到配套 bpe.model，设 asrModelingUnit="bbpe" + asrBpeVocab=<bpe.model 路径> 即可全量生效。
    std::string asrModelingUnit = "cjkchar";
    std::string asrBpeVocab = "";  // BBPE 编码器（bpe.model）；空 = 不用（现网同默认）

    // 是否把单字数字 + "点"写入热词（默认 true）。
    // ⚠️ 与现网的唯一差异：**刻意不写"十"**。实测"十"与"点"在同一段声学上是竞争候选
    // （「二号一点二」会被听成「二号十二」）：给"十"加同样 bonus 会把"一+点"路径压掉。
    // 18 条小数用例 A/B（官方 sherpa-onnx）：含十 14/18 → 去十 **18/18**；
    // 五十/十五点二/五十点零/二十/一百 等含"十"读法无回归（靠声学读对；
    // "十"的同音实/石/时由 DomainCorrect 在文本层归一）。
    bool hotwordDigits = true;

    // 是否额外把"十"写进热词（默认 false）。
    // BBPE 模型上实测"十"与"点"在同一段声学上抢分（一点二→十二），故默认不写；
    // 换用字符级模型时可用此开关 A/B（不同模型上该竞争关系不同，须实测后定默认值）。
    bool hotwordDigitTen = false;

    // 端点（与现网一致：三条规则同阈值，每句停顿即切句）
    bool enableEndpoint = true;
    double rule1TrailingSilence = 2.0;
    double rule2TrailingSilence = 2.0;
    double rule3TrailingSilence = 2.0;

    // 热词
    double hotwordsScore = 2.0;

    // 交互编排
    int silenceMs = 300;   // Interaction:SilenceMs（openSession 未传时用）
    int maxChars = 500;    // Interaction:MaxChars

    // 语义引擎
    bool lazyLoad = true;
    int idleUnloadSeconds = 180;

    // 嵌入
    float minSim = 0.55f;  // Embedding:MinSim

    // GTCRN 降噪（默认关闭，与现网一致；setDenoise 运行时开关）
    bool denoiseEnabled = false;
    std::string denoiseModelPath = "models/asr/gtcrn_simple.onnx";
    int denoiseNumThreads = 1;

    // 路径（相对 filesDir/vta/）
    std::string ranerDir = "models/raner";
    std::string embedDir = "models/embedding";
    std::string tablesBaseDir = "models/embedding/tables";   // Tables:BaseDir
    std::string charPinyinPath = "models/sherpa-onnx/hr/hr_char_pinyin.txt";  // 拼音字典（表内读音对齐用）
    std::string defaultTable = "default";  // Tables:DefaultTable

    // 从 filesDir/vta/vta.json 读取覆盖项（缺文件/缺键用默认值）
    static VtaConfig Load(const std::string& baseDir);
};

}  // namespace vta
