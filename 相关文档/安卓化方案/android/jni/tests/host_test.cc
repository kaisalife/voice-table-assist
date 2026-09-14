// jni/tests/host_test.cc —— 主机端纯逻辑自测（无需 sherpa-onnx / ORT / Android）
//
// 验证 C++ 翻译层与 C# 行为一致（方案 §8.1 "字级别一致"）：
//   1. ChineseNumeral / MergeText / TripleExtractor / CellPhraseGenerator
//   2. DomainCorrection / HomophoneReplacer（用生成的 hr_char_pinyin.txt）
//   3. VTX1 round-trip：C++ 读 C# 写的 cell_index.bin（汽机巡检/锅炉巡检）+ C++ 写回再读
//   4. TableRegistry：读 C# 写的 registry.json + SanitizeTableKey
//
// 用法（在 VoiceTableAssist 目录）：
//   cmake -S 相关文档/安卓化方案/android -B build-host -DVTA_HOST_TEST=ON
//   cmake --build build-host --target vta-host-test
//   build-host\vta-host-test.exe 相关文档/安卓化方案/android/assets/models
// 退出码 0 = 全过。
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../common/json.h"
#include "../common/strings.h"
#include "../embed/vtx1.h"
#include "../homophone/domain_correction.h"
#include "../homophone/homophone_replacer.h"
#include "../tables/table_registry.h"
#include "../tables/voice_resource.h"
#include "../text/cell_phrase_generator.h"
#include "../text/chinese_numeral.h"
#include "../text/merge_text.h"
#include "../text/pinyin_table.h"
#include "../text/sound_aligner.h"
#include "../text/triple_extractor.h"

using namespace vta;

namespace {
int gTotal = 0, gPass = 0;
std::FILE* gOut = nullptr;  // argv[2] 给定时写 UTF-8 文件（绕开控制台转码）

void Out(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (gOut) { std::fputs(buf, gOut); std::fputc('\n', gOut); }
    std::fputs(buf, stdout);
    std::fputc('\n', stdout);
}

void Check(bool pass, const std::string& name, const std::string& detail = "") {
    ++gTotal;
    if (pass) {
        ++gPass;
        Out("  PASS  %s", name.c_str());
    } else {
        Out("  FAIL  %s   %s", name.c_str(), detail.c_str());
    }
}

void CheckNear(double got, double want, const char* name) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "got=%.4f want=%.4f", got, want);
    Check(std::fabs(got - want) < 1e-9, name, buf);
}

void CheckNear2(double got, double want, const std::string& name) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "got=%.4f want=%.4f", got, want);
    Check(std::fabs(got - want) < 1e-6, name, buf);
}

void Section(const char* name) { Out("\n===== [%s] =====", name); }

std::string Num(double v) { return FormatCellValue(v); }
}  // namespace

int main(int argc, char** argv) {
    std::string modelsDir = argc > 1 ? argv[1] : "相关文档/安卓化方案/android/assets/models";
    if (argc > 2) gOut = std::fopen(argv[2], "wb");

    // ---------- 1. ChineseNumeral（对照 C# ChineseNumeral.ToDecimal）----------
    Section("chinese_numeral");
    CheckNear(ChineseNumeralToDecimal("十七点八四"), 17.84, "十七点八四=17.84");
    CheckNear(ChineseNumeralToDecimal("负九十五"), -95.0, "负九十五=-95");
    CheckNear(ChineseNumeralToDecimal("五十点零"), 50.0, "五十点零=50");
    CheckNear(ChineseNumeralToDecimal("十二"), 12.0, "十二=12（十位补1）");
    CheckNear(ChineseNumeralToDecimal("一百"), 100.0, "一百=100");
    CheckNear(ChineseNumeralToDecimal("一千零一"), 0.0, "一千零一=0（超出累加语义，与C#一致）");
    CheckNear(ChineseNumeralToDecimal("零点零一"), 0.01, "零点零一=0.01");
    CheckNear(ChineseNumeralToDecimal("六十"), 60.0, "六十=60");
    Check(Num(ChineseNumeralToDecimal("十七点八四")) == "17.84", "format 17.84");
    Check(Num(ChineseNumeralToDecimal("五十点零零")) == "50", "format 50.00→50");
    Check(Num(ChineseNumeralToDecimal("一点五")) == "1.5", "format 1.50→1.5");

    // ---------- 2. MergeText（对照 C# VoiceInteractionSession.MergeText）----------
    Section("merge_text");
    Check(MergeStreamingText("", "一号是五") == "一号是五", "空前缀");
    Check(MergeStreamingText("一号是五", "一号是五") == "一号是五", "包含去重 prev⊃cur");
    Check(MergeStreamingText("一号是", "一号是五") == "一号是五", "包含去重 cur⊃prev");
    Check(MergeStreamingText("硬度一号十七", "十七点八四") == "硬度一号十七点八四",
          "重叠拼接（2码点重叠）");
    // 重叠 <2 码点不拼接（与 C# 一致：for overlap >= 2）
    Check(MergeStreamingText("硬度一号", "号是五") == "硬度一号 号是五", "单字重叠不拼接");
    Check(MergeStreamingText("硬度一号", "二号六十") == "硬度一号 二号六十", "无重叠空格连接");

    // ---------- 3. TripleExtractor（对照 C#）----------
    Section("triple_extractor");
    {
        // "硬度一号十七点八四" 的理想 BIO
        std::vector<std::pair<std::string, std::string>> bio = {
            {"硬", "B-SUB"}, {"度", "I-SUB"}, {"一", "B-OBJ"},
            {"号", "I-OBJ"}, {"十", "B-VAL"}, {"七", "I-VAL"},
            {"点", "I-VAL"}, {"八", "I-VAL"}, {"四", "I-VAL"},
        };
        auto t = ExtractTriples(bio);
        Check(t.size() == 1 && t[0].sub == "硬度" && t[0].obj == "一号" && t[0].val == "十七点八四",
              "完整三元组");
    }
    {
        // OBJ 无 VAL → "?"
        std::vector<std::pair<std::string, std::string>> bio = {
            {"硬", "B-SUB"}, {"度", "I-SUB"}, {"一", "B-OBJ"}, {"号", "I-OBJ"},
        };
        auto t = ExtractTriples(bio);
        Check(t.size() == 1 && t[0].obj == "一号" && t[0].val == "?", "缺VAL占位?");
    }
    {
        // SUB 后两个 OBJ/VAL 对（如"硬度一号五点零二号六十"）
        std::vector<std::pair<std::string, std::string>> bio = {
            {"硬", "B-SUB"}, {"度", "I-SUB"}, {"一", "B-OBJ"}, {"号", "I-OBJ"},
            {"五", "B-VAL"}, {"二", "B-OBJ"}, {"号", "I-OBJ"}, {"六", "B-VAL"},
        };
        auto t = ExtractTriples(bio);
        Check(t.size() == 2 && t[0].obj == "一号" && t[0].val == "五" && t[1].obj == "二号" &&
                  t[1].val == "六",
              "多三元组");
    }

    // ---------- 4. CellPhraseGenerator（对照 C#）----------
    Section("cell_phrase_generator");
    {
        auto p = GenerateCellPhrases(1, "硬度", 1);
        bool hasRow = false, hasCol = false, hasRev = false;
        for (const auto& s : p) {
            if (s == "硬度一号") hasRow = true;
            if (s == "序号一测量值一") hasCol = true;
            if (s == "一号硬度") hasRev = true;
        }
        Check(hasRow && hasCol && hasRev, "行标签×列描述符×反序");
        // 「测量值几」不反序
        bool noBadRev = true;
        for (const auto& s : p)
            if (s.rfind("测量值", 0) == 0 && s.find("硬度") != std::string::npos &&
                s.find("测量值") == 0)
                noBadRev = false;
        Check(noBadRev, "测量值不反序");
        Check(ToChineseNum(21) == "二十一" && ToChineseNum(10) == "十" && ToChineseNum(99) == "九十九",
              "ToChineseNum 1~99");
    }

    // ---------- 5. 通用数字同音归一（表相关纠错已删，改由读音吸附覆盖）----------
    Section("domain_correction");
    Check(DomainCorrect("付五点零") == "负五点零", "付→负（数字上下文）");
    Check(DomainCorrect("付零点五") == "负零点五", "付→负（零点）");
    Check(DomainCorrect("一灵点五") == "一零点五", "灵→零（数字夹持）");
    Check(DomainCorrect("五实六") == "五十六", "实→十（前后数字夹持）");
    Check(DomainCorrect("实二") == "十二", "实→十（前缀）");
    Check(DomainCorrect("三寺") == "三四", "寺→四");
    Check(DomainCorrect("好天气") == "好天气", "非数字语境不动（好由吸附处理）");
    Check(DomainCorrect("实在") == "实在", "实非数字语境不动");
    Check(DomainCorrect("时间") == "时间", "时非数字语境不动");
    // 表相关的同音（印度→硬度、外景→外径、二好→二号）由 SoundAligner 覆盖，见 sound_aligner 段

    // ---------- 6. HomophoneReplacer（生成的拼音表 + 规则）----------
    Section("homophone_replacer");
    {
        std::string pinyinPath = modelsDir + "/sherpa-onnx/hr/hr_char_pinyin.txt";
        if (FileExists(pinyinPath)) {
            HomophoneReplacer lexicon(pinyinPath);
            Check(lexicon.Enabled(), "拼音表加载");
            Check(lexicon.ToTone3Pinyin("硬度") == "ying4du4", "ToTone3Pinyin(硬度)");
            Check(lexicon.ToTone3Pinyin("外径") == "wai4jing4", "ToTone3Pinyin(外径)");
            Check(lexicon.ToTone3Pinyin("圆度") == "yuan2du4", "ToTone3Pinyin(圆度)");
            // 规则：误识别拼音 → 正确汉字（等价现网 hr_rules 生成逻辑）
            // 独=du2、井=jing3（以生成的拼音表为准）
            HomophoneReplacer r(pinyinPath, {"ying4du2=硬度", "wai4jing3=外径"});
            Check(r.Apply("硬度一号是五十点零") == "硬度一号是五十点零", "无错字原样保留");
            Check(r.Apply(" Ying4du3 ") == " Ying4du3 ", "非汉字串不匹配");
            // 中文同音错字（"硬独"→ying4du2）
            Check(r.Apply("硬独一号") == "硬度一号", "同音错字纠正");
            Check(r.Apply("外井二号") == "外径二号", "跨 token 最长匹配");
        } else {
            std::printf("  SKIP  homophone（缺 %s）\n", pinyinPath.c_str());
        }
    }

    // ---------- 7. VTX1 round-trip（§8.1 核心验收线）----------
    Section("vtx1");
    {
        // 7a. C++ 读 C# 写的 .bin（汽机巡检）
        const char* tables[] = {"汽机巡检", "锅炉巡检"};
        for (const char* t : tables) {
            std::string path = modelsDir + "/embedding/tables/" + t + "/cell_index.bin";
            if (!FileExists(path)) {
                std::printf("  SKIP  vtx1 %s（缺 %s）\n", t, path.c_str());
                continue;
            }
            VtxIndex idx;
            std::string err;
            bool ok = Vtx1Load(path, &idx, &err);
            Check(ok, std::string("C++ 读 C# 写的 bin: ") + t, err);
            if (ok) {
                // registry.json 里该表 rows/cols/dim 应与 bin 头一致
                auto doc = json::ParseFile(modelsDir + "/embedding/tables/registry.json");
                bool metaOk = false;
                if (doc) {
                    for (const auto& e : doc->at("tables").asArray()) {
                        if (e.at("name").asString() == t) {
                            metaOk = e.at("rowsCount").asInt() == idx.rowsCount &&
                                     e.at("colsCount").asInt() == idx.colsCount &&
                                     e.at("dim").asInt() == idx.dim;
                        }
                    }
                }
                Check(metaOk, std::string("bin 头与 registry 一致: ") + t);
                Check(idx.dim == 768, "dim=768 (gte-base-zh)");
                // 向量已归一化 → 自身内积≈1
                if (!idx.entries.empty()) {
                    float self = Vtx1Dot(idx.entries[0].vec.data(), idx.entries[0].vec.data(), idx.dim);
                    CheckNear(self, 1.0, "向量已归一化（自内积≈1）");
                }
            }
        }
        // 7b. C++ 写 → C++ 读 round-trip（字节级等价 C# BinaryWriter 格式）
        VtxIndex w;
        w.dim = 4;
        w.rowsCount = 2;
        w.colsCount = 2;
        w.rows = {"行一", "行二"};
        for (int r = 1; r <= 2; ++r)
            for (int c = 1; c <= 2; ++c) {
                VtxCell cell;
                cell.row = r;
                cell.col = c;
                cell.phrase = "行" + std::to_string(r) + "列" + std::to_string(c);
                cell.vec = {0.1f * r, 0.2f * c, 0.3f, 0.4f};
                w.entries.push_back(std::move(cell));
            }
        std::string tmp = modelsDir + "/../vtx1_roundtrip_test.bin";
        std::string err2;
        Check(Vtx1Save(tmp, w, &err2), "C++ 写 VTX1", err2);
        VtxIndex r2;
        Check(Vtx1Load(tmp, &r2, &err2), "C++ 读回", err2);
        Check(r2.dim == 4 && r2.entries.size() == 4 && r2.entries[3].phrase == "行2列2" &&
                  std::fabs(r2.entries[3].vec[1] - 0.4f) < 1e-6,
              "round-trip 数据一致");
        // 头部魔数校验（C# Magic=0x31585456 "VTX1" 小端）
        {
            std::string blob;
            (void)ReadFileBytes(tmp, &blob);
            uint32_t magic = 0;
            std::memcpy(&magic, blob.data(), 4);
            Check(magic == 0x31585456, "魔数 VTX1 与 C# 一致");
        }
        remove(tmp.c_str());
    }

    // ---------- 9b. 表内读音对齐（新语音输入同音解决方案）----------
    Section("sound_aligner");
    {
        std::string pinyinPath = modelsDir + "/sherpa-onnx/hr/hr_char_pinyin.txt";
        auto py = PinyinTable::Get(pinyinPath);
        if (py == nullptr) {
            std::printf("  SKIP  sound_aligner（缺 %s）\n", pinyinPath.c_str());
        } else {
            // 蒸汽巡检表（锅炉巡检）
            SoundAligner al(py, {"水位", "汽压", "汽温", "风压", "流量", "给水温度"}, 4);
            Check(al.Ready(), "对齐器就绪（词表 " + std::to_string(al.Vocabulary().size()) + " 条）");
            Check(al.AlignSentence("水卫一号五米") == "水位一号五米", "同音：水卫→水位",
                  al.AlignSentence("水卫一号五米"));
            Check(al.AlignSentence("气压一号零点八") == "汽压一号零点八", "同音：气压→汽压",
                  al.AlignSentence("气压一号零点八"));
            Check(al.AlignSentence("风雅三号二点五") == "风压三号二点五", "同音：风雅→风压",
                  al.AlignSentence("风雅三号二点五"));
            Check(al.AlignSentence("给水问度四号") == "给水温度四号", "单字错：问→温",
                  al.AlignSentence("给水问度四号"));
            Check(al.AlignSentence("殷号五米") == "一号五米", "位置词表：殷号→一号",
                  al.AlignSentence("殷号五米"));
            Check(al.AlignSentence("二好三米") == "二号三米", "位置词表：二好→二号",
                  al.AlignSentence("二好三米"));
            Check(al.AlignSentence("今天天气不错") == "今天天气不错", "负例：表外语不替换",
                  al.AlignSentence("今天天气不错"));

            // 硬度在另一张表（汽机巡检历史表）
            SoundAligner al2(py, {"外径", "内径", "表面光洁度", "硬度", "直线度", "圆度"}, 6);
            Check(al2.AlignSentence("印度一号十七点八四") == "硬度一号十七点八四",
                  "近音：印度→硬度（ying/yin）", al2.AlignSentence("印度一号十七点八四"));
            Check(al2.AlignSentence("外景二号") == "外径二号", "近音：外景→外径",
                  al2.AlignSentence("外景二号"));

            // 防误纠：表内两行读音完全一样 → 分不清就放弃
            SoundAligner amb(py, {"汽压", "气压"}, 4);
            auto m = amb.AlignWord("气亚");
            Check(m.canonical.empty(), "防误纠：两个候选并列 → 放弃（不替换）", m.canonical);
            auto m2 = amb.AlignWord("汽压");
            Check(m2.canonical == "汽压", "本来就对 → 直接返回");

            // 音节相似度（通用音近规则）
            CheckNear2(SoundAligner::SyllableSim("ying", "yin"), 0.9, "前/后鼻音：ying≈yin");
            CheckNear2(SoundAligner::SyllableSim("shi", "si"), 0.9, "平/翘舌：shi≈si");
            CheckNear2(SoundAligner::SyllableSim("shui", "shui"), 1.0, "完全相同");
            CheckNear2(SoundAligner::SyllableSim("hao", "wei"), 0.0, "完全不像");
        }
    }

    // ---------- 9c. 按流热词串（逐字空格分隔；数字可选）----------
    Section("hotword_stream");
    {
        std::string hw = TableVoiceResourceGenerator::BuildHotWordsStream({"水位", "汽压"}, 2);
        Check(hw.find("/") != std::string::npos, "多短语用 / 分隔");
        Check(hw.find("水 位") != std::string::npos, "行标签逐字空格分隔", hw);
        Check(hw.find("测 量 值 一") != std::string::npos, "位置词入热词");
        Check(hw.find("1") == std::string::npos, "含阿拉伯数字的写法被跳过");
        // 单字数字默认写入，但**不含"十"**（实测"十"与"点"竞争同一段声学，
        // 加权"十"会让 一点二→十二；去十后 18/18 小数用例正确）
        Check(hw.find("点") != std::string::npos && hw.find("五") != std::string::npos,
              "默认含单字数字与小数点");
        Check(hw.find("十") == std::string::npos, "默认不含单字数字十（避免与点竞争）");
        std::string hw2 = TableVoiceResourceGenerator::BuildHotWordsStream({"水位"}, 2, false);
        Check(hw2.find("点") == std::string::npos && hw2.find("十") == std::string::npos,
              "hotwordDigits=false 时不写单字数字");
    }

    // ---------- 8. TableRegistry（读 C# registry.json + SanitizeTableKey）----------    Section("table_registry");
    {
        TableRegistry reg(modelsDir + "/embedding/tables", "default");
        auto snap = reg.Snapshot();
        Check(snap.size() == 2, "读 C# registry.json 得 2 张表",
              "size=" + std::to_string(snap.size()));
        bool namesOk = false;
        for (const auto& e : snap)
            if (e.name == "汽机巡检" || e.name == "锅炉巡检") namesOk = true;
        Check(namesOk, "表名含 汽机巡检/锅炉巡检");
        Check(reg.ResolveExistingKey("汽机巡检") == "汽机巡检", "ResolveExistingKey 中文名");
        Check(reg.ResolveExistingKey("不存在") .empty(), "未注册返回空");
        Check(TableRegistry::SanitizeTableKey("锅炉/巡检*1") == "锅炉_巡检_1", "SanitizeTableKey 非法字符");
        Check(TableRegistry::SanitizeTableKey("___") == "table", "全下划线→table");
        // EnsureRegistered 撞名后缀
        TableRegistry reg2(modelsDir + "/../reg_test_tmp", "default");
        std::string k1 = reg2.EnsureRegistered("新表", 3, 4, 768);
        std::string k2 = reg2.EnsureRegistered("新表", 5, 4, 768);
        Check(k1 == k2 && k1 == "新表", "同名更新返回同 key");
        std::string k3 = reg2.EnsureRegistered("新表/", 1, 1, 768);
        Check(k3 == "新表_1", "撞 key 追加后缀（'/'被替换后尾'_'被裁剪）", k3);
    }

    // ---------- 9. 语音资源生成（hotwords/rule 生成器）----------
    Section("voice_resource");
    {
        std::string pinyinPath = modelsDir + "/sherpa-onnx/hr/hr_char_pinyin.txt";
        auto cols = TableVoiceResourceGenerator::ColumnDescriptors(6);
        Check(cols.size() == 72, "6 列 → 72 个列描述符", std::to_string(cols.size()));
        std::string hw = TableVoiceResourceGenerator::BuildHotWords({"硬度", "振动"}, 2);
        Check(hw.find("硬 度\n") != std::string::npos, "热词逐字空格分隔（与现网 C# 一致）", hw);
        Check(hw.find("一 号\n") != std::string::npos, "列描述符入热词");
        Check(hw.find("1号") == std::string::npos, "含 ASCII 数字短语被跳过");
        if (FileExists(pinyinPath)) {
            HomophoneReplacer lexicon(pinyinPath);
            std::string rules =
                TableVoiceResourceGenerator::BuildRules(lexicon, {"硬度", "振动"}, "", 2);
            Check(rules.find("ying4du4=硬度\n") != std::string::npos, "行标签规则 拼音=汉字");
            Check(rules.find("ce4liang4zhi2yi1=测量值一\n") != std::string::npos,
                  "列描述符恒等规则（值=zhi2）");
        }
    }

    // ---------- 10. JSON 解析器（crf_transitions.json / tokenizer.json 可解析）----------
    Section("json");
    {
        auto crf = json::ParseFile(modelsDir + "/raner/crf_transitions.json");
        if (crf) {
            size_t n = crf->isArray() && !crf->asArray().empty() && crf->asArray()[0].isNumber()
                           ? crf->asArray().size()
                           : (crf->isArray() ? crf->asArray().size() * crf->asArray()[0].asArray().size()
                                             : crf->at("data").asArray().size());
            Check(n == 49, "crf_transitions.json 49 个元素", std::to_string(n));
        } else {
            std::printf("  SKIP  crf json（int8 产物用 npy）\n");
        }
        auto tok = json::ParseFile(modelsDir + "/embedding/tokenizer.json");
        Check(tok && !tok->at("model").at("vocab").asObject().empty(), "tokenizer.json vocab 可解析");
    }

    std::printf("\n===== 结果: %d/%d 通过 =====\n", gPass, gTotal);
    return gPass == gTotal ? 0 : 1;
}
