# VoiceTableAssist.Tests

xUnit + FluentAssertions 单测项目。覆盖最容易被改动踩到的纯函数层。

## 运行

```bash
# 在仓库根目录
dotnet test tests/VoiceTableAssist.Tests
```

## 覆盖范围

| 文件 | 测什么 | 为什么关键 |
|---|---|---|
| `Asr/MergeTextTests.cs` | 流式文本合并（前缀扩展 / 包含去重 / 重叠拼接）| 累计 / 累计精简 / NER 衔接点；`OnFinal`/`FoldPartial` 调它 |
| `Asr/HomophoneReplacerTests.cs` | 同音字替换（拼音整串匹配）| ASR 误识别 → 正确行标签的核心纠错 |
| `Services/TripleExtractorTests.cs` | BIO 序列 → (Sub, Obj, Val) 三元组 | NER 输出 → 填表的契约 |
| `Services/ChineseNumeralTests.cs` | 中文数字 → 阿拉伯数字（0~1000，2 位小数）| 抽出的 val 转 Excel 数字 |

## 设计原则

- **不依赖磁盘**。`HomophoneReplacer` 临时文件用 `Path.GetTempFileName` + `try/finally Delete`，不污染仓库。
- **不依赖网络**。所有模型/嵌入/WS 都不碰；只测纯函数层。
- **不依赖全局状态**。无 `WebApplicationFactory`，无 DI 容器构建。
- **可重复**。无随机输入，无时间断言。

## 后续可加

- `EngineHost.AcquireSession` / `ReleaseSession` 的单飞 + ReferenceEquals 行为（需要 IConfiguration mock）。
- `OnFinal` 的累计溢出/计时器行为（需要 fake logger + manual timer 注入；工作量大，先看 Tier 2 需求）。
- `HomophoneReplacerProvider.Get` 的多表路由（需要写一个 IConfiguration in-memory provider；Tier 2）。
