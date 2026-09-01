# VoiceTableAssist API 文档——接口说明

> 依据现有实现整理，供前端、后端调用方及部署验收使用。
>
> **源码地址**：<https://github.com/kaisalife/voice-table-assist>
>
> GitHub 仓库**不含 models**；网关源码构建前请先在 `app/VoiceTableAssist/` 中执行：
> `pip install modelscope && modelscope download --model yanxiashuiyun/VoiceTableAssist --local_dir .`

## 一、概述

**VoiceTableAssist** 为单进程、单端口（`15232`）语音/表格服务。HTTP 与 WebSocket 协议均基于 JSON；WebSocket 地址为 `/api/speech/asr/stream`。

**核心能力**：

1. **多表支持**：按 `table` / `tableName` 切换向量库与语音特化资源。
2. **表名回退**：省略或为空 → 当前活动表 → 回退 `default`（旧客户端零改动）。
3. **未导入保护**：查询未导入的非 `default` 表 → `404` 或 `422`，提示先调用 `POST /import_table`。

---

## 二、通用约定

| 项 | 约定 |
|---|---|
| Base URL | `http://<host>:15232` |
| 编码 | 请求/响应体均为 UTF-8 JSON，`Content-Type: application/json` |
| 数值 | `values` / `value` 为数字；中文数字已由后端转为阿拉伯数字（`double`） |
| 坐标 | `row` / `column` 从 **1** 开始，与导入时 `index` 一致 |
| 跨域 | CORS 全开（`AllowAnyOrigin/AllowAnyMethod/AllowAnyHeader`） |

---

## 三、健康检查

### 1. `GET /api/health`

**200 响应示例**

```json
{
  "status": "ok",
  "service": "voice-table-assist",
  "provider": "sherpa",
  "configured": true,
  "ranerModelDir": "D:\\app\\voice-table-assist\\models",
  "activeTable": "default",
  "provider_version": "1"
}
```

| 字段 | 说明 |
|---|---|
| `provider` | ASR 类型，固定为 `sherpa` |
| `configured` | `true`=可用 |
| `activeTable` | 当前活动表名；`null`=已卸载（再查询自动重载） |
| `modelsLoaded` | 懒加载状态：`false`=模型未驻内存；`true`=已加载 |

### 2. `GET /healthz`（兼容别名）

**200 响应示例**

```json
{ "status": "ok", "modelDir": "...", "mode": "RaNER+gte-base-zh", "provider": "sherpa", "configured": true, "activeTable": "default" }
```

---

## 四、文本→表格单元格

### 1. `POST /text_to_json`

解析一句话，返回命中的单元格数组。**顶层为纯数组**。

**请求示例**

```json
{ "text": "巡检登记，硬度，一号是五十点零，二号是四十九点九九", "table": "default" }
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `text` | string | 是 | 自然语言输入 |
| `table` | string | 否 | 目标表名；缺省=当前活动表 → `default` |

**200 响应示例**

```json
[
  { "row": 4, "column": 1, "values": 50 },
  { "row": 4, "column": 2, "values": 49.99 }
]
```

**错误响应**

| 状态码 | 场景 | 响应体 |
|---|---|---|
| `404` | 表未导入 | `{ "error": "表 \"xxx\" 尚未导入，请先 POST /import_table" }` |
| `500` | 推理异常 | `{ "error": "<message>" }` |

---

## 五、导入表

### 1. `POST /import_table`

构建一张表的向量库并写入磁盘，更新注册表、激活该表、重建该表语音资源。同表重复导入为**覆盖**（last-write-wins）。

**请求示例**

```json
{
  "tableName": "力学性能",
  "rows": [
    { "label": "抗拉强度", "index": 1 },
    { "label": "屈服强度", "index": 2 },
    { "label": "伸长率", "index": 3 }
  ],
  "columnCount": 4
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `tableName` | string | 否 | 缺省=`default`；trim 后不得为空，否则 `400` |
| `rows[]` | array | 是 | 行；`label`=行标签；`index`=行号（从 1 起） |
| `rows[].label` | string | 是 | 行标签，如「硬度」 |
| `rows[].index` | int | 是 | 行号 |
| `columnCount` | int | 是 | 列数；列号自动从 1 到 `columnCount` 生成 |

> **列无需传列名**：列定位本质是「列号索引」，列描述符（`1号` / `第1列` / `测量值1` 等）由后端按列号自动生成。
> **维度无上限**：向量行/列维度来自 `rows[]` 真实长度与 `columnCount`，无固定上限、无 `6×6` 硬编码。

**200 响应示例**

```json
{ "status": "ok", "tableName": "力学性能", "rowsCount": 3, "colsCount": 4, "entries": 950, "dim": 768 }
```

| 字段 | 说明 |
|---|---|
| `entries` | 向量条数，约 `rows×cols×每单元格短语数` |
| `dim` | 向量维度，固定 `768` |

**错误响应**

| 状态码 | 场景 |
|---|---|
| `400` | 表名为空 / 解析失败等，返回 `{ "error": "<message>" }` |

**落盘路径**

| 资源 | 路径 |
|---|---|
| 向量库 | `models/embedding/tables/<key>/cell_index.bin`（二进制 VTX1 格式） |
| 注册表 | `models/embedding/tables/registry.json` |
| 语音资源 | `sherpa-onnx/hr/tables/<key>/`；`default` 对应 `sherpa-onnx/hr/tables/current/` |

> **格式说明**：向量库统一为二进制 VTX1 格式，体积约为文本格式的 16%，加载无解析成本。若目标机存在旧版 `cell_index.json`（registry 无对应记录），对该表重新 `POST /import_table` 一次即可生成 `cell_index.bin` 并自动清理旧文件。

---

## 六、NER 别名

### 1. `POST /api/speech/ner`

与 `/text_to_json` 同链路，返回结构化三元组（含耗时），兼容既有后端调用方。

**请求示例**

```json
{ "text": "屈服强度，二号是二百", "table": "力学性能" }
```

| 字段 | 类型 | 必填 |
|---|---|---|
| `text` | string | 是 |
| `table` | string | 否 |

**200 响应示例**

```json
{ "triples": [ { "column": 2, "row": 2, "value": 200 } ], "elapsedMs": 12 }
```

| 字段 | 说明 |
|---|---|
| `triples` | 有效三元组数组；缺数值或为 `?`、无法定位单元格的条目被过滤 |
| `triples[].column` / `row` | 列/行号 |
| `triples[].value` | 阿拉伯数值 |
| `elapsedMs` | 推理耗时（ms） |

**错误响应**

| 状态码 | 场景 | 响应体 |
|---|---|---|
| `400` | 缺 `text` | `{ "error": "缺少 text 字段" }` |
| `422` | 未能解析有效三元组 | `{ "error": "未能从文本中解析出有效的三元组", "text": "..." }` |
| `500` | 推理失败 | `{ "error": "NER 推理失败", "detail": "..." }` |

---

## 七、表清单

### 1. `GET /tables`

**200 响应示例**

```json
{
  "tables": [
    { "name": "default", "key": "default", "rowsCount": 6, "colsCount": 6, "dim": 768, "importedAt": "2026-08-27T08:00:00Z" },
    { "name": "力学性能", "key": "力学性能", "rowsCount": 3, "colsCount": 4, "dim": 768, "importedAt": "2026-08-27T08:05:00Z" }
  ],
  "activeTable": "力学性能"
}
```

| 字段 | 说明 |
|---|---|
| `tables[]` | 已导入表清单 |
| `activeTable` | 当前活动表；`null`=已卸载 |

---

## 八、卸载表

### 1. `POST /api/table/unload`

只释放活动表内存，**不删除磁盘文件**，下次查询自动重载。

**请求示例（可选）**

```json
{ "tableName": "" }
```

| 字段 | 说明 |
|---|---|
| `tableName` | 空/省略 → 卸载当前活动表 |

**200 响应示例**

```json
{ "status": "ok", "activeTable": null }
```

> 后台另有空闲自动卸载兜底（`Tables:IdleTimeout`，默认 30 分钟）。

---

## 九、重建语音资源

### 1. `POST /api/table/voice`

依据表结构重建 `hotwords.txt` + `hr_rules.txt`。`/import_table` 已在进程内直调同一逻辑。

**请求示例**

```json
{ "rows": ["外径", "内径", "表面光洁度", "硬度", "直线度", "圆度"], "columnCount": 6, "tableKey": "default" }
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `rows` | string[] | 是 | 行标签列表，空串会被剔除 |
| `columnCount` | int | 否 | 列数，默认 `0` |
| `tableKey` | string | 否 | 目标表 key；缺省=当前活动表；`default` → `tables/current/` |

**200 响应示例**

```json
{ "status": "ok", "rowsCount": 6, "columnCount": 6, "tableKey": "default" }
```

**错误响应**

| 状态码 | 场景 |
|---|---|
| `400` | 字符拼音表缺失，返回 `{ "error": "char-pinyin 表缺失: ..." }` |

---

## 十、WebSocket 流式识别

### 1. `WS /api/speech/asr/stream?table=<表名>`

连接即激活对应表，并使用该表同音纠正（HR）规则。

**连接规则**

| 场景 | 行为 |
|---|---|
| 表未导入 | 网关在握手前返回 `404` JSON |
| 已有语音会话 | 新连接在握手前返回 **HTTP 409** |

**上行帧（浏览器 → 服务）**

| 帧类型 | 内容 |
|---|---|
| 二进制帧 | float32 PCM，`16kHz`，服务端原样透传给 sherpa |
| 文本帧 | `{ "type": "stop" }`，结束本次识别 |

**下行帧（服务 → 浏览器）**

```json
{ "type": "loading", "message": "正在加载语义解析模型（首次使用需数秒）..." }
{ "type": "ready" }
{ "type": "partial", "text": "硬度", "isFinal": false, "accumulated": "" }
{ "type": "final", "text": "硬度，一号是五十点零", "isFinal": true, "accumulated": "硬度，一号是五十点零" }
{ "type": "cells", "text": "硬度，一号是五十点零", "isFinal": true, "cells": [ { "row": 4, "column": 1, "values": 50 } ] }
```

| 字段 | 说明 |
|---|---|
| `type` | `loading` / `ready` / `partial` / `final` / `cells` |
| `message` | 仅 `type=loading`：模型按需加载的进度提示 |
| `text` | 识别文本（已做同音纠正 + 数字/列号上下文替换） |
| `isFinal` | 是否最终结果 |
| `accumulated` | 仅 `partial`/`final`：服务端交互会话的当前累计文本（**提交后保留**，供省略行标签的后续语句沿用上下文）；仅溢出清空后为空串 |
| `cells` | 仅 `type=cells`：解析好的单元格，结构与 `/text_to_json` 一致 |

### 2. 懒加载与常驻策略

| 配置项 | 默认值 | 说明 |
|---|---|---|
| `Models:LazyLoad` | `true` | 语义引擎（RaNER/gte）按需加载；`false`=启动即加载 |
| `Models:IdleUnloadSeconds` | 代码 30s，随包 `180` | 语义引擎空闲多少秒自动卸载。**sherpa 语音模型常驻不卸载**（随服务启动后台拉起） |

**行为**：

1. 语义引擎不随服务启动常驻（待机内存约 300MB），首次 WS 连接或首次文本查询时自动加载（RaNER/gte 约 1.5~2s），期间下发 `loading` 帧；空闲达到阈值且无活跃语音会话时自动卸载。
2. **sherpa 语音模型常驻**：随服务启动后台拉起（约 8s，期间下发 `loading` 帧），空闲不卸载（待机即 600~700MB），保证语音随时可用。

### 3. 错误帧

```json
{ "type": "error", "code": "ASR_CONNECTION" / "PARSE_FAILED" / "ACCUM_OVERFLOW", "message": "..." }
```

| 错误码 | 含义 |
|---|---|
| `ASR_CONNECTION` | sherpa 连接异常 |
| `PARSE_FAILED` | 解析失败 |
| `ACCUM_OVERFLOW` | 累积文本超过 `Interaction:MaxChars` 上限，服务端清空；`voice-mic.js` 收到后立即断开会话并释放麦克风 |

### 4. 服务端交互编排

服务端自动完成：流式文本合并 → **sherpa 端点检测（说完停顿约 2 秒切句输出 final）** → 300ms 后 RaNER 解析 → 下发 `cells`。客户端也可发 `{"type":"stop"}` 立即提交。

**配置项**

| 配置项 | 默认值 | 说明 |
|---|---|---|
| `SherpaServer:EnableEndpoint` | `true` | 端点检测：说完停顿后 sherpa 主动输出 final（关闭则只能靠 `stop` 提交） |
| `SherpaServer:Rule1TrailingSilence` | `2.0` | 停顿多少秒切一句（Rule2/3 同值，保证每句都能断句） |
| `Interaction:SilenceMs` | `300` | final 到达后的提交收尾延时 |
| `Interaction:MaxChars` | `500` | 累积上限，超限自动清空并发 `ACCUM_OVERFLOW` |

> **累计文本提交后保留**（不清空）：后续语句常省略行标签（说完「水位一号是五十」再说「二号是六十」），基于完整累计上下文解析才能正确归属；重复格子以相同/更新值覆盖，无害。

**前端接入（推荐）**

服务托管 `/voice-mic.js`，跨源可直接引用（静态资源已带 CORS 头）：

```html
<script src="http://192.168.1.100:15232/voice-mic.js"></script>
<button onclick="mic.toggle()">语音录入</button>
<script>
  const mic = VoiceMic.create({
    base: 'http://192.168.1.100:15232',
    table: '力学性能',
    onResult: (cells) => cells.forEach(c => fill(c.row, c.column, c.values)),
    onError: (msg) => alert(msg),
  })
</script>
```

> 浏览器麦克风要求 **HTTPS 或 localhost** 安全上下文。跨源采集脚本 `audio-capture-worklet.js` 默认从网关地址加载，如需自托管可通过 `workletUrl` 指定。

---

## 十一、验证页与静态资源

### 1. `GET /`

返回 `wwwroot` 下的浏览器验证页（麦克风识别到表格单元格）。

### 2. 静态资源

| 路径 | 用途 |
|---|---|
| `/voice-mic.js` | 无 UI 极简语音录入客户端（推荐） |
| `/audio-capture-worklet.js` | 麦克风采集 Worklet |

> 旧版 `voice-client.js`、`speech-command.js` 已删除：功能由 `voice-mic.js` + 服务端编排取代。

---

## 十二、错误码速查

| 状态码 | 场景 |
|---|---|
| `200` | 成功 |
| `400` | 请求体缺关键字段 / 表名为空 / 导入失败 / 语音重建失败 |
| `404` | 查询未导入的表（`/text_to_json` 或 WS 指定 `?table=`） |
| `422` | `/api/speech/ner` 无法解析出有效三元组 |
| `500` | 后端推理/处理异常 |
| `503` | sherpa-onnx 子进程未就绪 / 启动失败 |

---

## 十三、多表语义

| 概念 | 说明 |
|---|---|
| 活动表 | 最后一次成功 `import_table` 或带 `table` / `?table=` 的请求所激活的表 |
| 省略表名 | 用当前活动表；无活动表用 `default` |
| 观察切换 | `GET /api/health`、`GET /tables` 的 `activeTable` 字段 |

---

## 十四、更新记录

### 1. 2026-09-01 语音链路修复与部署固化

| 项 | 说明 |
|---|---|
| 挂载策略 | sherpa 语音模型**常驻**（随启动后台拉起，空闲不卸载）；语义引擎保持懒加载 + 空闲 180s 卸载 |
| 端点检测 | `--enable-endpoint=true --rule1-min-trailing-silence=2`：说完停顿约 2s 自动切句输出 final；`Interaction:SilenceMs` 2500→300 收尾提交 |
| 累计保留 | 提交后不清空累计文本（省略行标签的后续语句沿用前文主体）；`Interaction:MaxChars` 120→500 |
| 负数解析 | 中文数字支持「负」前缀（`负九十五`→ -95，巡检负压/真空场景） |
| 热词聚合 | 导入/刷新表后聚合**所有表**热词并自动重启 sherpa（2s 防抖合并）；测试页打开自动刷新已存在表的语音资源 |
| 向量短语精简 | 每格 80→21 条：剔除阿拉伯数字变体（模型词表无数字）、「测量值X」不反序 |
| stop 时序修复 | stop 先等 sherpa 最终 final 再提交，修复 cells 因连接释放（SemaphoreSlim disposed）而丢失 |
| 前端诊断 | 麦克风权限状态、设备枚举、采集链路诊断（AudioContext 状态/数据块数）、输入电平条 |
| 部署固化 | publish 修 sherpaSrc 路径 / 剔除模型 zip 原件（1.6GB→1.05GB）/ 剥离用户数据；deploy-check 14 项自检 + 自测后自动还原干净；热词文件启动自愈；静态文件 no-cache |

### 2. 2026-08-28 单连接模式

语音链路收敛为**同时只允许一个连接存活**。

| 项 | 说明 |
|---|---|
| 新门卫 | `EngineHost.TryAcquireSession()/ReleaseSession()`，`Interlocked` 0/1 会话位 |
| 握手行为 | WS 握手在 `Program.cs` 中抢位；抢不到直接回 **HTTP 409**「已有语音会话在进行中」 |
| 删掉的并发机制 | 每连接索引快照、`TableVectorManager` 每表锁双检、`LoadAndActivate` |
| 统一机制 | 单把 `SemaphoreSlim` + 握手同步 `Activate(tableKey)`，`SubmitAsync` 直接取 `_manager.ActiveIndex` |
| 行为变化 | 握手不再立即 `ready`，先同步激活大表索引（秒级）；`SherpaAsrBridge` 不再自管活跃会话计数 |
| 改动文件 | `Program.cs`、`Services/EngineHost.cs`、`Services/TableVectorManager.cs`、`Asr/SherpaAsrBridge.cs`、`Asr/VoiceInteractionSession.cs`、`wwwroot/voice-mic.js`、`wwwroot/voice-client.js` |

### 3. 2026-08-28 WS 事件携带服务端累计文本

`partial`/`final` 事件新增 `accumulated` 字段：服务端交互会话的当前累计文本（合并去重后）。提交或溢出清空后自动归零。`voice-mic.js` 的 `onTranscript` 回调新增第三个参数 `accumulated`。

### 4. 2026-08-28 模型懒加载 + 空闲自动卸载

解决常驻约 1.7GB 内存问题。

| 项 | 说明 |
|---|---|
| 新组件 | `Services/EngineHost.cs`：按需加载、30 秒空闲自动卸载、会话计数、卸载时停 sherpa 子进程 |
| 内存实测 | 启动待机约 120MB（原约 1.4GB + sherpa 670MB）；加载后约 1.0GB + sherpa 683MB；空闲 30s 回落约 120MB |
| 冷启动 | 文本查询 +1.5~2s，WS 语音连接 +8~10s，期间下发 `loading` 帧 |
| 新事件 | `type=loading`；`/api/health` 新增 `modelsLoaded` |
| 配置 | `Models:LazyLoad`（默认 `true`）、`Models:IdleUnloadSeconds`（默认 `30`） |
| 顺带修复 | `TableRegistry` 读写属性名大小写不一致导致重启后注册表被误判损坏；`/text_to_json` body 非法 JSON/缺 `text` 从裸 `500` 改为 `400` |

### 5. 2026-08-27 向量库二进制格式 VTX1

二进制 VTX1（`cell_index.bin`）：768 维浮点直接以 float32 落盘，体积约为原来的 16%，加载从「读盘+逐字符解析」变为「读盘+内存拷贝」。写路径仍原子替换（tmp→File.Move）。服务只读写二进制格式：存量旧文件重新 `POST /import_table` 即生成新格式，导入时自动清理旧文件。

### 6. 2026-08-27 WS 握手不阻塞向量库加载

`?table=` 连接时向量库改为后台加载，握手毫秒级完成（实测 connect→ready 约 100ms）。提交解析双条件：静默 2.5s 且本连接表索引就绪；每条会话绑定连接时刻的表索引快照，用户忘关语音直接切表也不会串表。

### 7. 2026-08-27 服务端交互编排

WS 流式识别从「裸 ASR 通道」升级为交互即服务：文本合并、静默判定、RaNER 解析全部下沉到服务端（`Asr/VoiceInteractionSession.cs`），每条连接独立会话，下行已串行化。

---

## 末、存疑点

1. `Models:IdleUnloadSeconds` 随包配置为 `180`，代码默认 `30`，是否需要在文档中统一为单一默认值？
2. WebSocket 单连接模式下，握手同步激活大表索引的耗时上限是否需要给出明确阈值或超时配置？
3. `accumulated` 字段主要面向调试，是否应在前端生产代码中依赖该字段做业务逻辑？
4. 懒加载模式下首次语音连接 8~10s 的冷启动耗时，是否需在前端给出更明确的 loading 提示文案？
5. 错误码 `503` 的具体触发条件（sherpa 子进程启动失败重试策略）是否需要在文档中补充？
