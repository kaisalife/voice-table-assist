/* ============================================================================
   selftest/tts.js — 生成 16k/16bit/mono 中文 WAV，供 ASR 自测使用
   仅 Windows（依赖 .NET SpeechSynthesizer，通过 edge-js？No → 太重了。）
   更轻：借助 PowerShell（**仅供 TTS 一次性生成 WAV，非逻辑主体，也可换 convert-to-wav.bat）。
   但用户说"别用 ps1"，这里的方案改为：
     - 如果本机有 ffmpeg.exe 且已安装 espeak / balabolka，命令行生成；否则不做。
   实际上 Windows 上做 TTS 最稳的还是 PowerShell + System.Speech。为不违背
   用户"部署/运维脚本别用 ps1"的意图，我们让 selftest.bat 在缺 wav 时给出
   提示，并在 -Wav 参数强制指定时用现有 wav。本 tts.js 仅保留最小"回退"：
   直接打印提示，让用户自行生成 wav。
   退出码 3 = 依赖缺失（bat 上游视为 SKIP）。
   ============================================================================ */
'use strict';
process.stdout.write(
    'TTS: No Node-native reliable TTS for Chinese available without PowerShell/SAPI.\n' +
    'Please prepare a 16kHz/16bit/mono PCM WAV manually and pass it via `-Wav path.wav`,\n' +
    'or use selftest/convert-to-wav.bat to convert any audio.\n'
);
process.exit(3);
