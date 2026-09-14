// jni/common/strings.cc
#include "strings.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <windows.h>
#define VTA_MKDIR(p) _mkdir(p)
#define VTA_TIMEGM(t) _mkgmtime(t)
#define VTA_GMTIME_R(t, tm) (gmtime_s(tm, t) == 0 ? tm : nullptr)
#define VTA_UNLINK(p) _unlink(p)
#define VTA_STAT(p, st) _stat((p), (st))
#define VTA_S_ISREG(m) ((m) & _S_IFREG)
#define VTA_S_ISDIR(m) ((m) & _S_IFDIR)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define VTA_MKDIR(p) mkdir((p), 0755)
#define VTA_TIMEGM(t) timegm(t)
#define VTA_GMTIME_R(t, tm) gmtime_r(t, tm)
#define VTA_UNLINK(p) unlink(p)
#define VTA_STAT(p, st) stat((p), (st))
#define VTA_S_ISREG(m) S_ISREG(m)
#define VTA_S_ISDIR(m) S_ISDIR(m)
#endif

namespace vta {

std::vector<CodePoint> Utf8ToCodePoints(const std::string& s) {
    std::vector<CodePoint> cps;
    cps.reserve(s.size());
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            cps.push_back(c);
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
            cps.push_back(((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu));
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
            cps.push_back(((c & 0x0Fu) << 12) |
                          ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                          (static_cast<unsigned char>(s[i + 2]) & 0x3Fu));
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
            cps.push_back(((c & 0x07u) << 18) |
                          ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                          ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) |
                          (static_cast<unsigned char>(s[i + 3]) & 0x3Fu));
            i += 4;
        } else {
            cps.push_back(0xFFFD);
            i += 1;
        }
    }
    return cps;
}

void AppendUtf8(CodePoint cp, std::string* out) {
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string CodePointsToUtf8(const std::vector<CodePoint>& cps) {
    std::string out;
    out.reserve(cps.size() * 3);
    for (CodePoint cp : cps) AppendUtf8(cp, &out);
    return out;
}

bool ReadFileBytes(const std::string& path, std::string* out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) { std::fclose(f); return false; }
    out->resize(static_cast<size_t>(n));
    size_t got = n > 0 ? std::fread(out->data(), 1, static_cast<size_t>(n), f) : 0;
    std::fclose(f);
    return got == static_cast<size_t>(n);
}

bool WriteFileBytes(const std::string& path, const void* data, size_t size) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t put = size > 0 ? std::fwrite(data, 1, size, f) : 0;
    std::fclose(f);
    return put == size;
}

bool WriteFileText(const std::string& path, const std::string& text) {
    return WriteFileBytes(path, text.data(), text.size());
}

std::vector<std::string> ReadFileLines(const std::string& path) {
    std::vector<std::string> lines;
    std::string text;
    if (!ReadFileBytes(path, &text)) return lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            std::string line = text.substr(start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
            break;
        }
        std::string line = text.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        start = nl + 1;
    }
    return lines;
}

bool MakeDirs(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    size_t i = 0;
    if (path[0] == '/') { cur = "/"; i = 1; }
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        if (j > i) {
            if (!cur.empty() && cur.back() != '/') cur += '/';
            cur += path.substr(i, j - i);
            if (VTA_MKDIR(cur.c_str()) != 0 && errno != EEXIST) return false;
        }
        i = j + 1;
    }
    return true;
}

bool FileExists(const std::string& path) {
#ifdef _WIN32
    struct _stat st;
#else
    struct stat st;
#endif
    return VTA_STAT(path.c_str(), &st) == 0 && VTA_S_ISREG(st.st_mode);
}

std::vector<std::string> ListSubDirs(const std::string& path) {
    std::vector<std::string> out;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((path + "/*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.')
            out.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(path.c_str());
    if (!d) return out;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        struct stat st;
        std::string full = path + "/" + ent->d_name;
        if (::stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) out.push_back(ent->d_name);
    }
    closedir(d);
#endif
    return out;
}

bool WriteFileAtomic(const std::string& path, const void* data, size_t size) {
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        std::string dir = path.substr(0, slash);
        if (!dir.empty() && !MakeDirs(dir)) return false;
    }
    char suffix[64];
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::snprintf(suffix, sizeof(suffix), ".tmp-%llx", static_cast<unsigned long long>(now));
    std::string tmp = path + suffix;
    if (!WriteFileBytes(tmp, data, size)) return false;
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        VTA_UNLINK(tmp.c_str());
        return false;
    }
    return true;
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    auto isWs = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
    };
    while (b < e && (isWs(static_cast<unsigned char>(s[b])) || s[b] == static_cast<char>(0xE3))) {
        // 全角空格 U+3000 = E3 80 80
        if (s[b] == static_cast<char>(0xE3) && b + 2 < e &&
            static_cast<unsigned char>(s[b + 1]) == 0x80 &&
            static_cast<unsigned char>(s[b + 2]) == 0x80) {
            b += 3;
        } else if (isWs(static_cast<unsigned char>(s[b]))) {
            ++b;
        } else {
            break;
        }
    }
    while (e > b) {
        unsigned char c = static_cast<unsigned char>(s[e - 1]);
        if (isWs(c)) { --e; continue; }
        if (c == 0x80 && e >= 3 && static_cast<unsigned char>(s[e - 2]) == 0x80 &&
            static_cast<unsigned char>(s[e - 3]) == 0xE3) { e -= 3; continue; }
        break;
    }
    return s.substr(b, e - b);
}

std::string TrimUnderscore(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && s[b] == '_') ++b;
    while (e > b && s[e - 1] == '_') --e;
    return s.substr(b, e - b);
}

std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t ParseIso8601ToMs(const std::string& iso) {
    // "2026-09-11T08:30:00[.fff][Z]" → epoch ms（C# DateTime 序列化兼容）
    if (iso.size() < 19) return 0;
    std::tm tm{};
    int y, mo, d, h, mi, se;
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = se;
    time_t t = VTA_TIMEGM(&tm);
    if (t < 0) return 0;
    int64_t ms = static_cast<int64_t>(t) * 1000;
    // 小数毫秒
    size_t dot = iso.find('.', 19);
    if (dot != std::string::npos && dot + 1 < iso.size()) {
        int frac = 0, digits = 0;
        for (size_t i = dot + 1; i < iso.size() && std::isdigit(static_cast<unsigned char>(iso[i])); ++i) {
            if (digits < 3) { frac = frac * 10 + (iso[i] - '0'); ++digits; }
        }
        while (digits < 3) { frac *= 10; ++digits; }
        ms += frac;
    }
    return ms;
}

std::string MsToIso8601(int64_t ms) {
    time_t t = static_cast<time_t>(ms / 1000);
    std::tm tm{};
    VTA_GMTIME_R(&t, &tm);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms % 1000));
    return buf;
}

}  // namespace vta
