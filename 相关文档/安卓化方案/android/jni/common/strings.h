// jni/common/strings.h —— UTF-8/码点工具（C# char 语义在 CJK/BMP 范围内等价于码点）
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace vta {

using CodePoint = uint32_t;

// UTF-8 → 码点序列（非法字节替换为 U+FFFD）
std::vector<CodePoint> Utf8ToCodePoints(const std::string& s);
// 码点序列 → UTF-8
std::string CodePointsToUtf8(const std::vector<CodePoint>& cps);
// 单码点 → UTF-8 追加到 out
void AppendUtf8(CodePoint cp, std::string* out);

// C# char.IsHan 等价判断（本仓库用到的基础区间）
inline bool IsHan(CodePoint cp) { return cp >= 0x4E00 && cp <= 0x9FFF; }

// TableRegistry.IsCjk：CJK 统一表意 + 扩展 A + 兼容表意
inline bool IsCjk(CodePoint cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0xF900 && cp <= 0xFAFF);
}

// ASCII 数字
inline bool IsAsciiDigit(CodePoint cp) { return cp >= '0' && cp <= '9'; }

// 读整个文件（二进制）
bool ReadFileBytes(const std::string& path, std::string* out);
// 写文件（二进制，覆盖）；成功返回 true
bool WriteFileBytes(const std::string& path, const void* data, size_t size);
// 写文本文件（UTF-8 无 BOM，等价 C# new UTF8Encoding(false)）
bool WriteFileText(const std::string& path, const std::string& text);
// 逐行读文本（按 \n 切分，去 \r；空行保留空串语义由调用方过滤）
std::vector<std::string> ReadFileLines(const std::string& path);
// 原子写：先写 tmp 再 rename 覆盖（等价 C# WriteJsonAtomic / VectorIndex.Save）
bool WriteFileAtomic(const std::string& path, const void* data, size_t size);
// 目录创建（递归，等价 Directory.CreateDirectory）
bool MakeDirs(const std::string& path);
// 文件存在
bool FileExists(const std::string& path);
// 列出目录下的子目录名（不含 "." ".."；目录不存在返回空）
std::vector<std::string> ListSubDirs(const std::string& path);

// 去除首尾空白（ASCII 空白 + 全角空格）
std::string Trim(const std::string& s);
// 去除首尾下划线（SanitizeTableKey 收尾用）
std::string TrimUnderscore(const std::string& s);
// string 替换全部（等价 string.Replace）
std::string ReplaceAll(std::string s, const std::string& from, const std::string& to);
// 逐码点小写化（ASCII 范围；等价 ToLowerInvariant 对本仓库语料的覆盖面）
std::string ToLowerAscii(const std::string& s);
// 当前时间 epoch 毫秒
int64_t NowMs();
// ISO8601 UTC（"2026-09-11T08:30:00Z"）→ epoch ms；解析失败返回 0
int64_t ParseIso8601ToMs(const std::string& iso);
// epoch ms → ISO8601 UTC
std::string MsToIso8601(int64_t ms);

}  // namespace vta
