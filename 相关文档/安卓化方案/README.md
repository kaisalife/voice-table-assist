# VTA 安卓化实现（方案 A：纯 C/C++ + Android Native Service + AIDL）

本目录是《安卓化方案.md》的工程落地。C# 代码不复用，算法逻辑逐句翻译为 C++；
模型 ONNX 直接拷贝（量化见方案 §13，由人工完成，本工程只消费产物）。

## 目录结构

```
安卓化方案/
├─ android/
│  ├─ aidl/com/vta/                 # AIDL 接口 + Parcelable（Java/C++ 桩均自动生成）
│  │  ├─ IVtaService.aidl
│  │  ├─ PcmFrame.aidl / SessionState.aidl / TableInfo.aidl / CellHit.aidl
│  ├─ jni/
│  │  ├─ common/                    # 日志 / 极小 JSON / UTF-8 与文件工具
│  │  ├─ asr/                       # sherpa-onnx OnlineRecognizer 包装 + PCM 管道
│  │  ├─ denoise/                   # GTCRN（STFT/ISTFT/状态缓存，逐行对齐 C#）
│  │  ├─ ner/                       # RaNER（BERT+CRF，Viterbi）
│  │  ├─ embed/                     # gte-base-zh 嵌入 + VTX1 向量库（C# BinaryWriter 二进制兼容）
│  │  ├─ homophone/                 # HR 同音纠正 + DomainCorrection 领域纠错
│  │  ├─ text/                      # ChineseNumeral / CellPhraseGenerator / TripleExtractor
│  │  ├─ session/                   # VoiceInteractionSession 编排 + 会话识别泵
│  │  ├─ tables/                    # TableRegistry / 语音资源生成 / TableVectorManager
│  │  ├─ tests/                     # 主机端纯逻辑自测（host_test.cc，89 断言）
│  │  └─ service/                   # AIDL 服务实现 / AAudio 采音 / EngineHost / main / 配置
│  ├─ Android.bp                    # Soong 构建（设备端正式形态）
│  ├─ CMakeLists.txt                # NDK/CMake 构建（开发迭代）
│  ├─ AndroidManifest.xml           # stub APK：service 注册 + signature 权限锁
│  └─ assets/models/                # 模型资源（scripts/copy_models.ps1 生成，大文件不进 git）
└─ scripts/copy_models.ps1|.sh      # 从现仓库 models/ 拷模型
```

## C# → C++ 模块对照

| C#（现网关） | C++（本工程） | 说明 |
|---|---|---|
| `Asr/SherpaAsrBridge.cs` | `jni/asr/recognizer.cc` + `jni/session/voice_session.cc` | 不再跨进程连 sherpa WS server，进程内直调 C++ 库；partial/final 语义不变 |
| `Asr/GtcrnDenoiser.cs` | `jni/denoise/gtcrn_denoiser.cc` | STFT/ISTFT/overlap-add/状态缓存逐行对齐，数值一致目标 <1e-4 |
| `Services/RaNerEngine.cs` | `jni/ner/raner_engine.cc` | 逐字 tokenizer + MAX_LEN=128 + Viterbi(7 标签) |
| `Services/EmbeddingEngine.cs` + `BerTokenizer.cs` | `jni/embed/embedder.cc` | BERT basic+wordpiece + mean 池化 + 归一化 + MinSim 过滤 |
| `Services/VectorIndex.cs` | `jni/embed/vtx1.cc` | VTX1 二进制格式；字符串 = C# BinaryWriter 7-bit LEB128 长度前缀，round-trip 兼容 |
| `Asr/HomophoneReplacer.cs` | `jni/homophone/homophone_replacer.cc` | 拼音表 + 贪心最长匹配 |
| `Asr/DomainCorrection.cs` | `jni/homophone/domain_correction.cc` | 直接替换 + 上下文规则（正则 lookaround 手写展开） |
| `Services/ChineseNumeral.cs` | `jni/text/chinese_numeral.cc` | 中文数字 0~1000 两位小数 |
| `Services/TripleExtractor.cs` | `jni/text/triple_extractor.cc` | BIO → 三元组 |
| `Services/CellPhraseGenerator.cs` | `jni/text/cell_phrase_generator.cc` | 单元格指代短语族 |
| `Asr/TableVoiceResourceGenerator.cs` | `jni/tables/voice_resource.cc` | hotwords.txt / hr_rules.txt 生成 + 聚合 |
| `Services/TableRegistry.cs` | `jni/tables/table_registry.cc` | registry.json 原子读写 + SanitizeTableKey |
| `Services/TableVectorManager.cs` | `jni/tables/table_vector_manager.cc` | 多表激活/导入/卸载 |
| `Asr/VoiceInteractionSession.cs` | `jni/session/interaction_session.cc` | 累计/静默提交/累计精简/MaxChars 溢出 |
| `Services/EngineHost.cs` | `jni/service/engine_host.cc` | 懒加载 + 空闲卸载 + Touch |

## 与现网 WS 协议的映射（方案 §3.5）

| 现网 WS | AIDL |
|---|---|
| WS 连接 `?table=` | `openSession(tableName, silenceMs, captureMode, client)` |
| 二进制帧 float32 PCM | `pushPcm(sessionId, PcmFrame)`（captureMode=1） |
| 下行 `partial` | 50~100ms 轮询 `getState` → `SessionState.partial` |
| 下行 `final` + `cells` | `pollCells(sessionId)` → `CellHit[]` |
| `{"type":"stop"}` | `closeSession(sessionId)`（flush 降噪/识别 + fold partial + 提交） |
| 重复 `openSession` | 幂等：自动关闭旧会话（含静默提交）再开新的（单会话模型：一个麦克风 = 一路） |
| 连接断开（AIDL 路线） | `linkToDeath` → 自动 close 活动会话；进程内路线（C ABI/`NativeBridge`）共进程，无此问题 |

差异修订（对方案草稿的两处落定）：
1. `openSession` 增加第 4 参 `@nullable IBinder client`——binder 死亡通知需要客户端 binder 才能注册，
   否则 §8.4"binder 断开自动回收"无法实现。企业 App 传自身任意 binder 即可。
2. `importTable` 从【待定】落地为正式接口——端侧无 HTTP 后台，没有它表永远进不来。
3. `setDenoise(bool)` 按方案 §5.2 建议落地（运行时开关，对新会话生效）。
4. `CellHit.row/column` 按方案 §3.2 契约输出 **0-indexed**（内部向量库 1-based，服务端已换算）。

## 构建（CMake 路线，开发迭代）

```bash
# 1. 依赖
#    - Android NDK r26+（clang 17, c++17）
#    - sherpa-onnx v1.12.x 源码 → android/third_party/sherpa-onnx
#    - onnxruntime-android ≥ 1.28.0 解包 → android/third_party/onnxruntime/
#      （jni/arm64-v8a/libonnxruntime.so + headers/）
#      ⚠️ 必须 ≥1.28.0：1.27.0 在骁龙 8 Elite Gen 5 zipformer 静默算错（方案 §14.4）
# 2. 模型
powershell -File 相关文档\安卓化方案\scripts\copy_models.ps1
# 3. 编译
cd 相关文档/安卓化方案/android
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Soong 路线（设备端正式形态）用 `Android.bp`：`mm -B` 于本目录。

## 部署

1. **stub APK**：`AndroidManifest.xml` + 最小 Java 壳（`VtaNativeService` 加载 `libvta-service.so`
   并 `System.loadLibrary` 后由 native 侧 `AServiceManager_addService("vta")` 注册）。
   APK 体积 ~1.0GB（全 fp32）/ ~360MB（全 int8，方案 §13.3），超 200MB 需拆 AAB/OBB（【待定】）。
2. **权限**：`RECORD_AUDIO` 由 stub APK 引导页申请一次，或
   `adb shell pm grant com.vta.service android.permission.RECORD_AUDIO`。
3. **SELinux**：正式部署需允许 `vta` service 注册/被 bind（vendor sepolicy 或 magisk 环境）；
   调试期可在 userdebug 上 `setenforce 0` 验证链路。
4. **模型目录**：`<filesDir>/vta/`（或 `--data-dir` 指定）。assets 随包自动展开；
   也可 `adb push models/* /data/local/tmp/vta/` 后直跑可执行文件。

## 多表模型（导入多张 · 单活动表）

**需求**：可以导入多张表共存；同一时刻只服务一张；用到哪张表就用**哪张表的数据**。

**设计**（与现网 C# 一致，已在设备上验证）：

| 机制 | 说明 |
|---|---|
| 表注册与落盘 | 每表独立目录 `tables/{key}/cell_index.bin`（VTX1）+ `registry.json` 记录 name/key/行列数/导入时间 |
| 单活动表 | `TableVectorManager::Activate(key)` 切换当前表；同一时刻只有一个"活动"表 |
| 用到哪张用哪张 | 会话打开时**快照该表的索引**（`shared_ptr<const VtxIndex>`）+ 按该表行标签构建**读音吸附词表** + 按该表生成**热词串** |
| 识别器常驻不重建 | 识别器**不含任何表相关状态**：热词用 sherpa 的 `CreateStream(本表热词串)` **按流传入**，切表/导入都不重建（实测：第 1 张表装载、第 2 张表 0 重建） |
| 导入即生效 | 导入只做"建索引 + 落盘 + 注册"；新表在下一次 `openSession` 自动生效（热词/吸附词表按表实时生成） |
| 单会话模型 | 一个物理麦克风 = 同一时刻一路识别；重复 `openSession` 幂等（自动关旧开新），换表即换会话 |
| 识别器重建安全 | 即便强制重建识别器，**在跑会话持有旧识别器**（共享所有权），识别流不悬空（已实测） |

**验收命令**（设备端）：

```bash
# 多表切换：逐表用各自的数据识别，检查 cells 是否都落在本表行列范围内
vta-service --switch-test <pcm16k.f32> 汽机巡检 锅炉巡检 --data-dir /data/local/tmp/vta
# 实测输出：
#   表=汽机巡检(6行x4列) 重建=是 cells=2 落在本表范围=是
#   表=锅炉巡检(6行x4列) 重建=否 cells=2 落在本表范围=是   ← 切表零重建
#   会话中途强制重建识别器 ×2 → 在跑会话不受影响
```

APK 测试页：**表下拉框**选择当前表 + **导入表**按钮（表名/行标签每行一个/列数），导入后自动切换。

## 同音字纠错（热词按流传入 + 表内读音吸附）

两层互补，**均零人工、换表自动生效**：

| 层 | 作用 | 来源 |
|---|---|---|
| **热词（按流传入）** | 解码前偏置，让 ASR **少听错**（实测：无热词出「印度殷号」，有热词出「硬度一号/水位」） | 本表行标签 + 位置词 + 数字 → 按模型 `tokens.txt` 逐字 token 化（缺字落字节 token，**无需 bpe.model**）→ `CreateStream(hotwords)` |
| **表内读音吸附** | 解码后纠错，听错也能**纠回来**（水卫→水位、印度→硬度、二好→二号…） | 本表行标签 + 列数枚举的位置词，用通用音近模型比对 |
| 通用数字同音归一 | 值里的数字同音（实/石/时→十、灵→零、付→负…） | 与表无关的通用规则，需声调相同 + 数字上下文 |

> 热词**不再走文件/聚合**，也不再重建识别器：`CreateStream(本表热词串)` 按流传入，
> 每张表只带自己的词（偏置聚焦），切表/导入识别器都不重建。
> HR 规则文件（hr_rules）已移除（只认"拼音完全相同"，被吸附覆盖）。
> 详见 [相关文档/新语音输入同音解决方案.md](../新语音输入同音解决方案.md)。

### 使用约束（重要）

**行名首句不可省略**：「测量值一/第一个」只表示列位置；首句不带行名时报"未匹配到表格项"（不是识别问题——实测 ASR 文本完全正确）。
先说过行名后，同一会话内可省略（累计上下文沿用，如「水位一号是五十」→「测量值二是四十九点九九」）。
完全同音的两行（汽压/气压）同时存在时，说哪个填哪个；只有"说了汽压被听成气压"无解（任何方法都无解）。

## 调试与验收（三层测试）

### 第 1 层：PC 纯逻辑自测（无需 NDK/设备/推理库，已跑通 72/72）

验证 C++ 翻译层与 C# 行为字级一致：中文数字、文本合并、三元组抽取、领域纠错、
同音纠正（含生成的拼音表）、**VTX1 round-trip（C++ 读 C# 写的 cell_index.bin + 反向）**、
registry.json、语音资源生成。

```powershell
# 用本机 MinGW g++（或任意 C++17 编译器；有 CMake 则 -DVTA_HOST_TEST=ON）
& "D:\wingw interator\mingw64\bin\g++.exe" -std=c++17 -O2 `
  -I"相关文档\安卓化方案\android\jni" `
  -o "相关文档\安卓化方案\android\build-host\vta-host-test.exe" `
  (相关文档\安卓化方案\android\jni\common\json.cc,strings.cc,
   text\*.cc, embed\vtx1.cc, homophone\*.cc, tables\table_registry.cc,
   tables\voice_resource.cc, tests\host_test.cc | % { "相关文档\安卓化方案\android\jni\$_" })
相关文档\安卓化方案\android\build-host\vta-host-test.exe 相关文档\安卓化方案\android\assets\models 输出.txt
# 退出码 0 = 全过；输出文件为原始 UTF-8（绕开控制台转码）
```

### 第 2 层：全管线数值对照（同一段 PCM，.NET 网关 vs Native Service）

**MuMu 实测已跑通**（2026-09-13）：x86_64 构建 + `--selftest`，TTS 语句
"硬度一号是十七点八四二号测量值六十" → 2 cells（`row=6 col=1 value=17.84`），
ASR int8 RTF≈0.32。构建/部署/运行步骤：

```powershell
# 工具链（一次性）：NDK r27c → D:\Android\android-ndk-r27c；cmake/ninja → pip install cmake ninja
# 依赖：third_party/sherpa-onnx（v1.12.40 源码）+ third_party/onnxruntime（1.28.0 aar 解包）
cmake -S 相关文档\安卓化方案\android -B D:\vb\x86 -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=D:\Android\android-ndk-r27c\build\cmake\android.toolchain.cmake `
  -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-31 -DCMAKE_BUILD_TYPE=Release
cmake --build D:\vb\x86 --target vta-service -j 8

# 部署到 MuMu（adb connect 127.0.0.1:16384）
adb push D:\vb\x86\vta-service 相关文档\安卓化方案\android\assets\models → /data/local/tmp/vta{,/models}
# 注意 vta.json 放 /data/local/tmp/vta/（assets/models 里那份含 BOM 时引擎会跳过——已修复解析器跳 BOM）

# 运行
adb shell "cd /data/local/tmp/vta && LD_LIBRARY_PATH=/data/local/tmp/vta/lib ./vta-service --selftest /data/local/tmp/xxx.f32 --data-dir /data/local/tmp/vta"
adb logcat -s VTA    # 看 partial/final/cells
```

```powershell
# 1. 准备 f32 PCM（复用现网工具）
node selftest\text2float32.js selftest\denoise-test\inp_16k.wav test.f32

# 2a. .NET 网关基准：起网关后
selftest\selftest.bat -Wav selftest\denoise-test\inp_16k.wav   # 记录 cells 输出

# 2b. C++ 侧：二选一
#    设备端（推荐）：adb push 后跑
adb push 相关文档\安卓化方案\android\assets\models /data/local/tmp/vta/
adb push build-android/vta-service /data/local/tmp/
adb shell "/data/local/tmp/vta-service --selftest /data/local/tmp/test.f32 --data-dir /data/local/tmp/vta"
#    PC 端：用 Windows 版 onnxruntime + sherpa-onnx 按同一 CMakeLists 编 vta-service.exe 再 --selftest
```

验收线（§8.1）：两边 `cell: row/col/value/raw` **字级一致**；
`raw` 是 HR+领域纠错后的原文，`value` 是中文数字转阿拉伯结果。

**RaNER 量化验收**（重要，见 `android/assets/models/README.md` 的缺陷说明）：
x86 + ORT 对 `MatMulInteger(uint8,int8)` 用 VPMADDUBSW，数值大时 int16 饱和 → 未
`reduce_range` 的 dynamic 量化模型在 x86 上会算错（官方 issue #7524/#16472/#19109）。
当前采用 `models/raner/export-int8-reduced/`（reduce_range，98MB，72 句测试集与 fp32 一致 71/72；
MuMu e2e 通过）。验收命令：

```bash
python 相关文档/安卓化方案/scripts/raner_check.py "硬度一号是十七点八四" models/raner/export-int8-reduced
python 相关文档/安卓化方案/scripts/raner_accuracy_compare.py models/raner/export-int8-reduced/model.onnx
python 相关文档/安卓化方案/scripts/quantize_raner_reduced.py      # 重新生成（一行，21s）
# 排查某模型是否受 kernel 缺陷影响：
#   python scripts/mmi_rewrite.py <模型> <out.onnx>  → 重写后正常即证明是 ORT kernel 而非模型
```
设备端：`vta-service --mmi-test <int8模型> <fp32模型>`（要求 cos≥0.99、argmax≥126/128）。

### 第 3 层：设备端验收（方案 §8.2/§8.3/§8.4）

```bash
adb logcat -s VTA                          # native 日志
# 数值自检（防 ORT 版本/SoC 静默算错）：官方 test_wavs 已随 ASR int8 包拷入
adb shell "cd /data/local/tmp/vta && /data/local/tmp/vta-service --selftest models/asr/sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30/test_wavs/0.wav 的f32版"
# P0 链路：bind 服务 → health()=="ok" → getVersion()==1
# 离线稳定性：拔网 30 分钟连续语音；后台锁屏 10 分钟 service 存活
# 异常路径：空 PCM 块不崩；错误采样率返回 -2；binder 断开会话自动回收
# RTF 实测：sherpa 官方 APK 或自跑，目标 ≤0.5（§14）
```

## 配置（vta.json，可选）

放 `<dataDir>/vta.json`，键名与现网 appsettings 对齐（缺省即默认值）：

```json
{
  "asrModelDir": "models/asr/sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30",
  "asrNumThreads": 4,
  "rule1TrailingSilence": 2.0,
  "silenceMs": 300,
  "maxChars": 500,
  "minSim": 0.55,
  "denoiseEnabled": false
}
```

## 未决项（方案 §9.2，代码已按默认值落地）

- 打包哪些表：`assets/models/embedding/tables/` 按需增删。
- 接入形态：主应用（C#+H5）采用进程内 C ABI + DllImport（见《安卓化方案接口适配文档.md》）；
  独立 stub APK + AIDL 的 Service 壳（`VtaNativeService`/`VtaForegroundKeeper`）未实现，
  需要时再补。
- 部署形态：单 APK 暴露 service（signature 权限锁）；内嵌企业 App 待集成方确认。
