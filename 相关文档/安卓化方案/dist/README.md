# VTA 安卓端测试 APK

自包含安装包：内置全部模型（ASR int8 + RaNER int8/reduce_range + 嵌入 int8 + 表向量库），
离线可用，不需要网关、不需要 adb、不需要命令行。

| 文件 | 适用设备 | 体积 |
|---|---|---|
| `VtaTest-arm64-v8a.apk` | **绝大多数安卓平板/手机**（ARM64） | 347 MB |
| `VtaTest-x86_64.apk` | 模拟器（MuMu/AVD） | 352 MB |

## 一、安装

1. 把 APK 拷到平板（USB / 微信文件 / 网盘均可）
2. 用文件管理器点击安装（首次需允许「安装未知来源应用」）
3. 桌面出现 **VTA语音测试** 图标

要求：Android 10（API 29）及以上。

## 二、测试步骤

打开 App 后有 4 个按钮：

| 按钮 | 作用 | 说明 |
|---|---|---|
| **1 初始化** | 解包内置模型 + 启动本地引擎 + **渲染巡检表** | 首次约 1~2 分钟（361MB 落盘），之后再点很快 |
| **2 离线自检** | 用内置测试语音跑完整链路并**回填表格** | **免麦克风、免人声**，最快的验收手段 |
| **3 开始录入** | 打开真实麦克风（Service 自主采音） | 首次弹权限，允许即可；识别结果实时回填 |
| **4 停止并出结果** | 收尾并取出剩余结果 | 自动静默 300ms 也会出结果 |

界面从上到下：**实时识别**（partial 文本）→ **最近命中** → **巡检表**（识别值直接填进对应格子并高亮）
→ **运行日志**。

巡检表与网关测试前端 `wwwroot/index.html` 一致：首列是行标签（水位/汽压/汽温/风压/流量/给水温度），
列头为 `1号~4号`，命中后格子变绿显示数值。坐标口径与前端 `data-r/data-c` 相同（第一个行标签=第1行）。

### 验收标准（点「2 离线自检」）

内置测试语音内容是「硬度一号是十七点八四二号测量值六十」，正确结果：

```
ok=1
final=硬度一号是十七点八四二号测量值六十
cells=2
cell:6|1|17.84|十七点八四      ← 第6行(给水温度) 第1列(1号) = 17.84
cell:6|2|0|?
```

表格上应看到：**给水温度** 行的 **1号** 格显示 `17.84`（绿色）、**2号** 格显示 `0`。

- `final` 字级正确 → ASR int8 + 热词生效
- `cell:6|1|17.84|十七点八四` → 三元组抽取 + 向量检索 + 中文数字转换全部正常
- 第二条 `raw=?` 是 NER 模型的已知切分瑕疵（PC 参考实现一致），不影响主流程

### 麦克风验收（点「3 开始录入」）

对着平板说：**「水位一号是五米」**（或表内任意 `行标签 + X号 + 数值`）

预期：「实时识别」逐字出现文本；停顿后对应格子自动填入数值并高亮。

## 三、远程/批量验证（可选，需 adb）

```bash
# 安装
adb install -r VtaTest-arm64-v8a.apk

# 免点击自动跑离线自检（首次需等模型解包）
adb shell am start -n com.vta.service/.VtaTestActivity -e autorun selftest
adb logcat -s VTA

# 自动进入录音模式（需先授予麦克风权限）
adb shell pm grant com.vta.service android.permission.RECORD_AUDIO
adb shell am start -n com.vta.service/.VtaTestActivity -e autorun listen
```

## 四、常见问题

| 现象 | 原因/处理 |
|---|---|
| 安装报「解析包错误」 | APK 拷贝不完整（应 347MB），重新拷贝 |
| 安装报「存储空间不足」 | 需 ~1.2GB（APK 347MB + 解包 361MB + 余量） |
| 点初始化卡很久 | 首次解包 361MB 正常，看日志的百分比进度 |
| 自检 `ok=0` / `cells=0` | 拍下整屏日志或抓 `adb logcat -s VTA` 发回 |
| 麦克风不出字 | 确认已允许麦克风权限；平板上是否被其它应用占用 |
| 识别有偏差 | 说话慢一点、离麦克风近一点；可在 App 内先点「2 离线自检」确认引擎正常 |

## 五、与正式方案的关系

- 本 APK 用**进程内 JNI 直调**（普通应用无权向 servicemanager 注册服务）；
- ASR/降噪/RaNER/同音纠正/向量检索/cells 全链路与正式 `IVtaService` AIDL 服务**同一套 C++ 代码**；
- 正式集成（预装/系统签名 APK 或企业 App）走 AIDL + `openSession/pushPcm/pollCells`，见
  `相关文档/安卓化方案/README.md`。

## 六、重新构建

```powershell
# 1. 编原生库（arm64 / x86_64）
cmake --build D:\vb\arm64 --target vta-jni -j 8      # 产出 libvta-service.so
#    拷到 相关文档\安卓化方案\build-<abi>\libvta-service.so

# 2. 打 APK（自动：拷 assets → 表目录转 ASCII → aapt2 → javac → d8 → 组包 → 签名）
powershell -ExecutionPolicy Bypass -File 相关文档\安卓化方案\scripts\build_apk.ps1 -Abi arm64-v8a
```
