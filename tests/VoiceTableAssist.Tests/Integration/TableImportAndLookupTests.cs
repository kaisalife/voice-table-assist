using FluentAssertions;
using VoiceTableAssist.Services;
using Xunit;

namespace VoiceTableAssist.Tests.Integration;

/// <summary>
/// 端到端集成测试：模拟前端"导入业务表 → 走 NER + embedding → 检查每个单元格能否命中"完整流程。
///
/// 这是"用户语音 → 表格"主链路的唯一集成测试，跑全 6 行 × 4 列验证：
///   1) 整行连读 NER 能正确切分（"外径测量值一11.11测量值二22.22..." → 4 个三元组）；
///   2) 每个三元组的 (sub+obj) 走 embedding 后能命中正确 (row, col)；
///   3) NER 不会跨行/跨列错位。
///
/// 模型依赖：
///   期望 &lt;repo>/models/raner/ 和 &lt;repo>/models/embedding/ 存在（出版目录已自带）。
///   也可设置 RANER_MODEL_DIR 环境变量指向含 raner/、embedding/ 的目录。
///   若模型缺失，整个 fixture 在 InitializeAsync 阶段抛 FileNotFoundException，xUnit 将所有测试标记为失败并给出明确提示。
///
/// 不进部署包：主 csproj 已 <Compile Remove="tests\**" /> + <Content Remove="tests\**" />；publish.bat 不碰 tests/。
/// </summary>
public class TableImportAndLookupTests : IClassFixture<TableLookupFixture>
{
    private readonly TableLookupFixture _fx;

    public TableImportAndLookupTests(TableLookupFixture fx) => _fx = fx;

    /// <summary>模拟前端"导入"业务表：6 行检验项目 × 4 列测量值（典型力学性能表）。</summary>
    public static IEnumerable<object[]> TestRows()
    {
        var labels = new[] { "外径", "内径", "厚度", "抗拉强度", "屈服强度", "延伸率" };
        for (var row = 1; row <= labels.Length; row++)
        {
            var colIndices = Enumerable.Range(1, TableLookupFixture.BusinessColumnCount).ToArray();
            yield return new object[] { row, labels[row - 1], colIndices };
        }
    }

    [Fact]
    public void NerPlusEmbedding_EndToEnd_ByRow()
    {
        // 按行扫：模拟"用户对同一行连续录入多列"的真实路径。
        // 例如 row=1, label=外径, 4 列：句子 = "外径测量值一十一点一一测量值二二十二点二二测量值三三十三点三三测量值四四十四点四四"。
        // NER 抽 4 个三元组（外径, 测量值一, 11.11）…（外径, 测量值四, 44.44），
        // 每个三元组独立走 embedding 查 sub+obj，期望都命中正确 (row, col)。
        // 一次 fixture 加载 = 6 行 × 4 列 = 24 个句子，覆盖整表所有 NER+embedding 串联组合。
        var raner = _fx.Raner;
        var embed = _fx.Embed;
        var index = _fx.Index;
        var failures = new List<string>();

        // 每行 4 列的样本值（中文读法），让 NER 既能看到列指代也能看到数值
        var sampleVals = new[] { "十一点一一", "二十二点二二", "三十三点三三", "四十四点四四" };

        foreach (var data in TestRows())
        {
            var row = (int)data[0];
            var label = (string)data[1];
            var cols = (int[])data[2];

            // 用"测量值 N"作为列指代（这是 CellPhraseGenerator 已枚举的最稳的列描述符）
            // 拼成一句完整口语化输入："{label}测量值一{val1}测量值二{val2}..."
            var sentence = label;
            for (var i = 0; i < cols.Length; i++)
            {
                var colDesc = CellPhraseGenerator.ToChineseNum(cols[i]);
                sentence += $"测量值{colDesc}{sampleVals[i]}";
            }

            var bio = raner.Predict(sentence);
            var triples = TripleExtractor.Extract(bio);

            // 整句应抽到 cols.Length 个完整三元组
            if (triples.Count != cols.Length)
            {
                failures.Add(
                    $"row={row} ({label}) cols=[{string.Join(",", cols)}] sentence=\"{sentence}\" → " +
                    $"抽到 {triples.Count} 个三元组（期望 {cols.Length}）；" +
                    $"BIO={string.Join(",", bio.Select(b => $"{b.Ch}/{b.Tag}"))}");
                continue;
            }

            // 每个三元组独立查 embedding，按 OBJ 中的列指代（"测量值一"）匹配目标 col
            for (var i = 0; i < triples.Count; i++)
            {
                var (sub, obj, val) = triples[i];
                var query = sub + obj;
                var hit = embed.Lookup(query, index);
                var expectedCol = cols[i];
                if (hit.Row != row || hit.Col != expectedCol)
                {
                    failures.Add(
                        $"row={row} ({label}) col={expectedCol} sentence=\"{sentence}\" " +
                        $"triple=({sub},{obj},{val}) query=\"{query}\" → " +
                        $"({hit.Row},{hit.Col}) sim={hit.Sim:F3} 命中=\"{hit.Phrase}\"");
                }
            }
        }

        failures.Should().BeEmpty(
            $"每行整句 NER+embedding 都应正确抽取并命中；{failures.Count} 条失败，示例：\n" +
            string.Join("\n", failures.Take(20)));
    }
}
