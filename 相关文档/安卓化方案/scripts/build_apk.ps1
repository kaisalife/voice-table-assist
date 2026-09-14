# 打包 VTA 安卓测试 APK（aapt2 + javac + d8 + apksigner，无需 gradle）
#
# 产物：相关文档\安卓化方案\dist\VtaTest.apk（自包含：内置 361MB 模型 + 测试页）
# 用法：powershell -ExecutionPolicy Bypass -File 相关文档\安卓化方案\scripts\build_apk.ps1 [-Abi arm64-v8a|x86_64]
param(
    [string]$Abi = "arm64-v8a",
    [string]$OutApk = ""
)
$ErrorActionPreference = "Stop"
$root = Resolve-Path "相关文档\安卓化方案"
$app = Join-Path $root "android\app"
$sdk = "D:\Android\Sdk"
$buildTools = Join-Path $sdk "build-tools\36.0.0"
$androidJar = Join-Path $sdk "platforms\android-34\android.jar"
$work = Join-Path $root "build-apk"
$dist = Join-Path $root "dist"
if (-not $OutApk) { $OutApk = Join-Path $dist "VtaTest-$Abi.apk" }

foreach ($p in @($buildTools, $androidJar)) { if (-not (Test-Path $p)) { throw "缺少: $p" } }
New-Item -ItemType Directory -Force -Path $work, $dist | Out-Null

# ---- 1. 准备 assets（模型 + 测试 PCM）----
$assets = Join-Path $app "assets"
if (Test-Path $assets) { Remove-Item $assets -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $assets "models"), (Join-Path $assets "test") | Out-Null
Write-Host "[1/7] 拷贝模型资产（~361MB，稍候）..."
Copy-Item (Join-Path $root "android\assets\models\*") (Join-Path $assets "models") -Recurse -Force
Copy-Item (Join-Path $root "dist\selftest_16k.f32") (Join-Path $assets "test\selftest_16k.f32") -Force
Copy-Item (Join-Path $root "dist\homophone_16k.f32") (Join-Path $assets "test\homophone_16k.f32") -Force

# ---- 1b. 表目录改 ASCII key ----
# aapt2 在 Windows 上会把非 ASCII 的资源路径双重编码（汽机巡检 → mojibake），
# 导致 AssetManager 打不开。改名后同步重写 registry.json 的 key 与 vta.json 的 defaultTable，
# 表名(name) 保持中文不变 —— 客户端仍按中文表名调用（ResolveExistingKey 会查到 ASCII key）。
$tablesDir = Join-Path $assets "models\embedding\tables"
$nameMap = [ordered]@{ "汽机巡检" = "qiji"; "锅炉巡检" = "guolu" }
foreach ($cn in $nameMap.Keys) {
    $src = Join-Path $tablesDir $cn
    if (Test-Path $src) { Rename-Item -LiteralPath $src -NewName $nameMap[$cn] }
}
$regPath = Join-Path $tablesDir "registry.json"
if (Test-Path $regPath) {
    $reg = Get-Content $regPath -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($t in $reg.tables) { if ($nameMap.Contains($t.key)) { $t.key = $nameMap[$t.key] } }
    ($reg | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath $regPath -Encoding UTF8
}
$vjPath = Join-Path $assets "models\vta.json"
if (Test-Path $vjPath) {
    $vj = Get-Content $vjPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $vj.defaultTable = "guolu"
    ($vj | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath $vjPath -Encoding UTF8
}
Write-Host "      表目录 → ASCII key（$($nameMap.Values -join ', ')）"

# ---- 2. 原生共享库 ----
$soSrc = Join-Path $root "build-$Abi\libvta-service.so"
if (-not (Test-Path $soSrc)) { throw "缺少 $soSrc（先 cmake --build ... --target vta-jni）" }
# 防呆：源码比 .so 新 → 说明 .so 是旧的（曾因此把带旧配置的库打进了 APK）
# 只比参与 .so 构建的源码；jni/tests 是主机自测，不影响 .so
$newestSrc = Get-ChildItem (Join-Path $root "android\jni") -Recurse -Include *.cc,*.h |
    Where-Object { $_.FullName -notmatch "\\tests\\" } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($newestSrc -and (Get-Item $soSrc).LastWriteTime -lt $newestSrc.LastWriteTime) {
    throw "libvta-service.so 比源码旧（$($newestSrc.Name) 更新）。请先重建：`n" +
          "  cmake --build D:\vb\$Abi --target vta-jni -j 8`n" +
          "  再把 D:\vb\$Abi\libvta-service.so 拷到 $soSrc"
}
$ortSo = Join-Path $root "android\third_party\onnxruntime\jni\$Abi\libonnxruntime.so"
if (-not (Test-Path $ortSo)) { throw "缺少 $ortSo" }
$libDir = Join-Path $work "lib\$Abi"
New-Item -ItemType Directory -Force -Path $libDir | Out-Null
Copy-Item $soSrc (Join-Path $libDir "libvta-service.so") -Force
Copy-Item $ortSo (Join-Path $libDir "libonnxruntime.so") -Force
Write-Host "[2/7] 原生库: $Abi"

# ---- 3. aapt2 link（manifest + assets → base.apk）----
Write-Host "[3/7] aapt2 link（打包 assets，较慢）..."
$base = Join-Path $work "base.apk"
if (Test-Path $base) { Remove-Item $base -Force }
& (Join-Path $buildTools "aapt2.exe") link `
    -I $androidJar `
    --manifest (Join-Path $app "AndroidManifest.xml") `
    -A $assets `
    --min-sdk-version 29 --target-sdk-version 34 `
    --version-code 1 --version-name 1.0 `
    -o $base
if ($LASTEXITCODE -ne 0) { throw "aapt2 link 失败" }

# ---- 4. javac ----
Write-Host "[4/7] javac..."
$classes = Join-Path $work "classes"
if (Test-Path $classes) { Remove-Item $classes -Recurse -Force }
New-Item -ItemType Directory -Force -Path $classes | Out-Null
$srcs = Get-ChildItem (Join-Path $app "java") -Recurse -Filter "*.java" | ForEach-Object { $_.FullName }
& javac -encoding UTF-8 --release 8 -nowarn -classpath $androidJar -d $classes @srcs 2>$null
if ($LASTEXITCODE -ne 0) { throw "javac 失败" }

# ---- 5. d8 → classes.dex ----
Write-Host "[5/7] d8..."
$dexOut = Join-Path $work "dex"
if (Test-Path $dexOut) { Remove-Item $dexOut -Recurse -Force }
New-Item -ItemType Directory -Force -Path $dexOut | Out-Null
$cls = Get-ChildItem $classes -Recurse -Filter "*.class" | ForEach-Object { $_.FullName }
& (Join-Path $buildTools "d8.bat") --release --min-api 29 --lib $androidJar --output $dexOut @cls
if ($LASTEXITCODE -ne 0) { throw "d8 失败" }

# ---- 6. 组包（base.apk + classes.dex + lib/，Python 可控写入）----
Write-Host "[6/7] 组包..."
$unsigned = Join-Path $work "unsigned.apk"
if (Test-Path $unsigned) { Remove-Item $unsigned -Force }
& python (Join-Path $root "scripts\pack_apk.py") $base (Join-Path $dexOut "classes.dex") $libDir $Abi $unsigned
if ($LASTEXITCODE -ne 0) { throw "组包失败" }

# ---- 7. zipalign + 签名 ----
Write-Host "[7/7] zipalign + apksigner..."
$aligned = Join-Path $work "aligned.apk"
if (Test-Path $aligned) { Remove-Item $aligned -Force }
& (Join-Path $buildTools "zipalign.exe") -f -p 4 $unsigned $aligned
if ($LASTEXITCODE -ne 0) { throw "zipalign 失败" }
$ks = Join-Path $work "debug.keystore"
if (-not (Test-Path $ks)) {
    & keytool -genkeypair -v -keystore $ks -storepass android -keypass android -alias vtadebug `
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=VTA Test, OU=Dev, O=VTA, L=, S=, C=CN" | Out-Null
}
if (Test-Path $OutApk) { Remove-Item $OutApk -Force }
& (Join-Path $buildTools "apksigner.bat") sign --ks $ks --ks-pass pass:android --key-pass pass:android `
    --ks-key-alias vtadebug --v1-signing-enabled true --v2-signing-enabled true `
    --out $OutApk $aligned
if ($LASTEXITCODE -ne 0) { throw "apksigner 失败" }

$sz = [math]::Round((Get-Item $OutApk).Length / 1MB, 1)
Write-Host "`n完成: $OutApk  ($sz MB)"
Write-Host "安装: adb install -r `"$OutApk`"   或把 APK 拷到平板点击安装"
