# VoiceTableAssist — 语音表格辅助（单一自包含部署包）

把三部分合并为一个**单进程、单端口(15232)** 的自包含服务包：
`sherpa-onnx`(本地离线流式 ASR) + `text_to_json`(进程内 RaNER + gte-base-zh) + `backend`(WebSocket/语音资源热切网关)。
对外只暴露 HTTP/WS 接口与一个验证页，**不再依赖外部 Node NER(15233)、不做跨服务 HTTP 桥**。

支持**多表**：一个实例可承载多张检验表，按表名持久化各自的 embedding 向量库与语音特化(HR)资源，
查询/导入/语音输入按表名切换，空闲/显式关闭可卸载。维度一律以导入请求 `rows[]` 的真实长度与 `columnCount` 为准。

> **语音单连接**：同一时刻只允许一路语音连接存活，其余连接握手直接回 **HTTP 409**；连接时**同步激活**对应表，
> 去掉了原多连接的并发编排，从根上规避切表 / 抢占问题。多表间的切换（导入 / HTTP 查询）不受影响，仍可自由换表。

## 相关文档（导航）

| 文档 | 内容 |
|---|---|
| [api文档.md](相关文档/api文档.md) | HTTP/WS 接口完整参考（请求/响应/错误码/多表语义/服务端交互编排） |
| [部署文档.md](相关文档/部署文档.md) | 构建、打包、安装、配置、日志、升级回滚、踩坑实录 |
| [用户使用指南.md](相关文档/用户使用指南.md) | 语音输入规则与标准样例 |
| [实现思路.md](相关文档/实现思路.md) | 架构与实现设计 |
| [模型训练方法.md](相关文档/模型训练方法.md) | RaNER 相关模型训练 |
| [模型训练相关文件示例/](相关文档/模型训练相关文件示例/) | 训练相关文件示例 |

> 部署包内自带 `相关文档/`（部署/api/用户指南三份），目标机解压即可查阅，无需回仓库。

## 能力与接口（单端口 15232）

| 方法 | 路径 | 说明 |
|---|---|---|
| HTTP | `GET  /api/health`、`GET /healthz` | 健康检查（别名兼容，含 `activeTable`） |
| HTTP | `POST /text_to_json` | 文本→表格单元格；body 可选 `table`（缺省=当前活动表→`default`）；返回顶层数组 `[{row,column,values}]` |
| HTTP | `POST /import_table` | 导入表：body `{tableName?, rows[], columnCount}` → 按表构建向量库 + 更新 registry + 激活 + 进程内重建该表语音资源 |
| HTTP | `POST /api/speech/ner` | NER 别名，body 可选 `table`，进程内 RaNER，保持既有调用方兼容 |
| HTTP | `GET  /tables` | 列出 registry 中的全部表 + 当前活动表 |
| HTTP | `POST /api/table/unload` | 卸载活动表（只清内存，不删盘）；body 可选 `tableName`（空=当前） |
| HTTP | `POST /api/table/voice` | 重建语音资源（缺省目标=当前活动表，可用 `tableKey` 指定） |
| WS   | `/api/speech/asr/stream?table=` | 语音识别流（16kHz float32 PCM）。**同一时刻仅一路连接**，二路回 409；**服务端自动编排**：静默 2.5 秒判定说完 → 解析 → 直发 `cells` 事件（`Interaction:SilenceMs` 可调）；前端接入见 [api文档.md](相关文档/api文档.md) 极简三行示例（`/voice-mic.js`） |
| HTTP | `/` | 验证页（wwwroot 麦克风识别） |

> `tableName`/`table` 为空或省略时一律回退：当前活动表 → `default`，旧客户端零改动。

## 目录结构

```
app/VoiceTableAssist/
├─ VoiceTableAssist.csproj      # Web SDK net8.0，引用 onnxruntime 1.20.1 + WindowsServices
├─ Program.cs                   # 入口：装配 + 路由
├─ appsettings.json             # 统一配置（端口/ASR/交互编排/sherpa/hr/tables）
├─ deploy-check.ps1             # 部署效果检查：14 项完整性自检 + 临时拉起服务 + 就绪自检（-Selftest 自测后自动还原干净；关掉脚本即停）
├─ publish.ps1                  # 打包发布脚本（Windows win-x64，含模型/sherpa/selftest/cordova/文档）
├─ publish-linux.ps1            # 打包发布脚本（Linux self-contained，需自备 sherpa-linux/）
├─ selftest/selftest.ps1        # 一体化自测：HTTP 多表热切换 + WS 就绪延迟 + ASR 语音端到端
├─ cordova/                     # 安卓 Cordova 壳模板（config.xml + build.ps1，APK 打包见部署文档第五节）
├─ Asr/                         # sherpa 进程托管、语音交互编排(VoiceInteractionSession)、同音纠正
├─ Infrastructure/              # 配置/DotEnv/WebSocket/文件日志
├─ Ner/                         # NER 端点（进程内 RaNER）
├─ Services/                    # RaNerEngine/EmbeddingEngine/表管理与向量库/三元组/tokenizer/数字转换
├─ Endpoints/                   # HTTP 路由分组
├─ Dtos/
├─ 相关文档/                     # 部署/api/用户指南等（随部署包分发）
└─ wwwroot/                     # 两张巡检表测试前端（锅炉巡检/汽机巡检）+ voice-mic.js 采集库
```

> 模型与 sherpa 二进制不编入编译产物：`models/`、`sherpa-onnx/` 由 `publish.ps1` 从仓库源拷贝到发布目录。

## 多表数据布局（运行期自动生成）

按 `Tables.BaseDir`（默认 `models/embedding/tables`）与 `Tables.HrBaseDir`（默认 `sherpa-onnx/hr/tables`）：

```
models/embedding/tables/
├─ registry.json               # 表名(name)↔文件系统安全键(key) + 行列/维度/导入时间 的注册表
├─ default/cell_index.bin      # default 表向量库（旧单文件 models/embedding/cell_index.json 启动时自动登记为 default）
└─ <key>/cell_index.bin        # 每表一目录的向量库（二进制 VTX1；key = SanitizeTableKey(表名)）
sherpa-onnx/hr/tables/
├─ current/                    # default 表的 hotwords.txt + hr_rules.txt（向后兼容旧路径）
└─ <key>/hotwords.txt, hr_rules.txt
```

- 表目录一律按 registry 里的 `key` 查找，**不直接拼表名进路径**，规避路径穿越。
- 向量库为**二进制 `cell_index.bin`（VTX1）**，写索引采用"写时拷贝"（先写临时文件再 `File.Move` 覆盖），导入期间查询仍用旧索引，绝不读到半成品。
- 卸载只释放内存；同表重复导入为 last-write-wins 覆盖。

## 本地构建

```powershell
cd app/VoiceTableAssist
dotnet build -c Release
```

## 打包发布

```powershell
cd app/VoiceTableAssist
powershell -ExecutionPolicy Bypass -File .\publish.ps1            # ASR 模型仅 float32（识别精度更高）
```

产出：`app\publish\voice-table-assist-win-x64.zip`（不在仓库根），解压后目录布局：

```
VoiceTableAssist.exe
appsettings.json
deploy-check.ps1                 # 临时拉起验证，关掉即停
selftest/selftest.ps1           # 一体化自测（http/ws/asr 三节，-Only 可选节）
models/{raner, embedding, asr}   # ASR 模型已统一并入 models/asr
sherpa-onnx/{exe, hr/}           # 原生 server + 语音资源（ASR 模型不在其中）
相关文档/{部署文档, api文档, 用户使用指南}.md
cordova/                         # 安卓 Cordova 壳模板（config.xml + build.ps1 + www/）
wwwroot/
```

> 注意：含中文的 .ps1 必须是 UTF-8 with BOM，否则 PowerShell 5.1 按 GBK 解析会报语法错误——
> 详见部署文档"踩坑实录"第 7 条。

## 目标机运行（无需 .NET 运行时）

1. 解压到任意目录（例如 `D:\app\voice-table-assist`）。
2. 推荐 `.\deploy-check.ps1`（临时拉起 + 就绪自检 + 可选 `-Selftest`，关掉脚本即停）；调试可直接 `.\VoiceTableAssist.exe`。
3. 验证：
   - `GET http://127.0.0.1:15232/api/health` → `configured:true`、`provider:sherpa`、`activeTable`
   - 日志显示 RaNER/embedding 装载完成、sherpa-onnx 就绪（端口 6006）
   - 浏览器打开 `http://127.0.0.1:15232/`：两张巡检表测试前端（自动导入、选表语音录入回填）
   - 平板/安卓接入：见部署文档「平板前端接入」节（浏览器直连限制 + Cordova 壳打包）

> 探活用 `127.0.0.1` 不用 `localhost`（IPv6 解析坑，详见部署文档"踩坑实录"）。

可选：`RANER_MODEL_DIR` 环境变量可覆盖 `models/` 目录（其下的 `tables/` 也随该目录定位）。

## Linux 部署（可选）

> C# 逻辑是多平台兼容的；Program.cs 已按平台切换服务托管（Windows=`UseWindowsService`，Linux=`UseSystemd`）。
> 但要跑在 Linux 上，原生依赖需替换为 Linux 版。

```powershell
# 1) 备好 Linux 版 sherpa-onnx：官方 tarball 解包整目录放入 sherpa-linux/
#    （含 bin/sherpa-onnx-online-websocket-server + models/ + hr/）
# 2) 交叉发布打包（产物：app\publish\voice-table-assist-linux-x64.zip，含模型/selftest/文档）
powershell -ExecutionPolicy Bypass -File .\publish-linux.ps1
```

解压到 Linux 后：
- 运行：前台 `./VoiceTableAssist`；服务：注册 `voice-table-assist.service` 用 systemd 托管。
- 按需修改 `appsettings.json` 的 `SherpaServer:ExePath` 为 Linux 版 sherpa 可执行文件名。
- 服务托管分支在 `Program.cs` 中由 `OperatingSystem.IsWindows()/IsLinux()` 决定，无需改代码。

## 注册为 Windows 服务（可选，系统自带 sc.exe，无外部依赖）

```powershell
sc.exe create VoiceTableAssist binPath= "D:\app\voice-table-assist\VoiceTableAssist.exe" start= auto DisplayName= "Voice Table Assist"
sc.exe description VoiceTableAssist "语音巡检网关（sherpa 离线识别 + RaNER 表格填充）"
sc.exe failure  VoiceTableAssist reset= 86400 actions= restart/5000
sc.exe start    VoiceTableAssist
# 停止/卸载：sc.exe stop VoiceTableAssist ; sc.exe delete VoiceTableAssist
```

## 自测

前置：服务已启动（默认端口 15232）。一体化脚本三节按序全跑（`-Only http|ws|asr` 可选节）：

```powershell
powershell -ExecutionPolicy Bypass -File .\selftest\selftest.ps1                # 全部
powershell -ExecutionPolicy Bypass -File .\selftest\selftest.ps1 -Only http     # 只跑 HTTP 节
```

- **http**：导入表A(`default` 6×6)与表B(`力学性能` 3×4)，依次带 `table` 名调用 `/text_to_json`、`/api/speech/ner`，用 `/api/health` 的 `activeTable` 校验热切换。
- **ws**：用非活动表建 WS 连接，测 connect→ready 延迟（握手不等索引加载）。
- **asr**：Windows TTS 生成语音（或 `-Wav` 指定 wav）走真实 WS 链路验证 partial/final/cells。

退出码 0=全部通过。

## 配置（appsettings.json）

- `Urls`：`http://0.0.0.0:15232`（对外监听）。
- `Interaction.SilenceMs`：静默自动提交阈值（默认 2500ms）；`Interaction.MaxChars`：语音累积上限（默认 120 字，超限清空并停止会话）。
- `SherpaServer.*`：sherpa-onnx 可执行/模型/热词，均相对 `sherpa-onnx/`。
- `Homophone.*`：`hr_char_pinyin.txt`、`hr_common_rules.txt`、`hr_rules.txt`、表目录（default 走 `tables/current`）。
- `Tables.*`：见下表。
- `AsrProvider:Endpoint`：sherpa-onnx 流式服务地址（默认 `ws://127.0.0.1:6006`）；`AsrProvider:SampleRate`：采样率（默认 16000，浏览器上行 float32 PCM 透传）。

### `Tables` 段

| 键 | 默认 | 说明 |
|---|---|---|
| `BaseDir` | `embedding/tables` | 多表向量库根目录（相对 models 根：`models/embedding/tables`；`RANER_MODEL_DIR` 覆盖时为其下 `embedding/tables`） |
| `HrBaseDir` | `sherpa-onnx/hr/tables` | 多表语音资源根目录 |
| `DefaultTable` | `default` | 缺省表逻辑名（旧 `cell_index.json` 自动登记为它） |
| `IdleTimeout` | `00:30:00` | 空闲卸载超时（无查询/语音活动则回收活动表内存） |
| `IdleCheckInterval` | `00:01:00` | 空闲巡检间隔 |

优先级：系统环境变量 > `app/.env`（如需）> appsettings.json。