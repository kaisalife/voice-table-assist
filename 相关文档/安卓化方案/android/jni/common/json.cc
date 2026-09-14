// jni/common/json.cc
#include "json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vta::json {

namespace {

struct Parser {
    const char* p;
    const char* end;
    std::string err;

    bool fail(const char* msg) {
        if (err.empty()) err = msg;
        return false;
    }

    void skipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    bool parseValue(Value* out, int depth) {
        if (depth > 64) return fail("nest too deep");
        skipWs();
        if (p >= end) return fail("unexpected end");
        char c = *p;
        if (c == '{') return parseObject(out, depth);
        if (c == '[') return parseArray(out, depth);
        if (c == '"') {
            std::string s;
            if (!parseString(&s)) return false;
            *out = Value(std::move(s));
            return true;
        }
        if (c == 't') { return parseLit("true", Value(true), out); }
        if (c == 'f') { return parseLit("false", Value(false), out); }
        if (c == 'n') { return parseLit("null", Value(), out); }
        return parseNumber(out);
    }

    bool parseLit(const char* lit, Value v, Value* out) {
        size_t n = std::strlen(lit);
        if (static_cast<size_t>(end - p) < n || std::strncmp(p, lit, n) != 0) return fail("bad literal");
        p += n;
        *out = std::move(v);
        return true;
    }

    bool parseNumber(Value* out) {
        const char* start = p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        bool any = false;
        while (p < end && std::isdigit(static_cast<unsigned char>(*p))) { ++p; any = true; }
        if (p < end && *p == '.') {
            ++p;
            while (p < end && std::isdigit(static_cast<unsigned char>(*p))) { ++p; any = true; }
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p < end && (*p == '-' || *p == '+')) ++p;
            while (p < end && std::isdigit(static_cast<unsigned char>(*p))) ++p;
        }
        if (!any) return fail("bad number");
        double d = std::strtod(std::string(start, p).c_str(), nullptr);
        if (std::isnan(d) || std::isinf(d)) return fail("bad number value");
        *out = Value(d);
        return true;
    }

    bool parseString(std::string* out) {
        if (*p != '"') return fail("expect string");
        ++p;
        out->clear();
        while (p < end) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (c == '"') { ++p; return true; }
            if (c == '\\') {
                ++p;
                if (p >= end) return fail("bad escape");
                char e = *p++;
                switch (e) {
                    case '"': out->push_back('"'); break;
                    case '\\': out->push_back('\\'); break;
                    case '/': out->push_back('/'); break;
                    case 'b': out->push_back('\b'); break;
                    case 'f': out->push_back('\f'); break;
                    case 'n': out->push_back('\n'); break;
                    case 'r': out->push_back('\r'); break;
                    case 't': out->push_back('\t'); break;
                    case 'u': {
                        if (end - p < 4) return fail("bad \\u");
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = *p++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                            else return fail("bad \\u hex");
                        }
                        // 代理对 → UTF-8
                        if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                            unsigned lo = 0;
                            const char* q = p + 2;
                            bool ok = true;
                            for (int i = 0; i < 4; ++i) {
                                char h = q[i];
                                lo <<= 4;
                                if (h >= '0' && h <= '9') lo |= static_cast<unsigned>(h - '0');
                                else if (h >= 'a' && h <= 'f') lo |= static_cast<unsigned>(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') lo |= static_cast<unsigned>(h - 'A' + 10);
                                else { ok = false; break; }
                            }
                            if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                p += 6;
                            }
                        }
                        AppendUtf8(cp, out);
                        break;
                    }
                    default: return fail("bad escape char");
                }
            } else {
                out->push_back(static_cast<char>(c));
                ++p;
            }
        }
        return fail("unterminated string");
    }

    static void AppendUtf8(unsigned cp, std::string* out) {
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

    bool parseArray(Value* out, int depth) {
        ++p;  // '['
        Array arr;
        skipWs();
        if (p < end && *p == ']') { ++p; *out = Value(std::move(arr)); return true; }
        while (true) {
            Value v;
            if (!parseValue(&v, depth + 1)) return false;
            arr.push_back(std::move(v));
            skipWs();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == ']') { ++p; *out = Value(std::move(arr)); return true; }
            return fail("expect , or ]");
        }
    }

    bool parseObject(Value* out, int depth) {
        ++p;  // '{'
        Object obj;
        skipWs();
        if (p < end && *p == '}') { ++p; *out = Value(std::move(obj)); return true; }
        while (true) {
            skipWs();
            std::string key;
            if (p >= end || *p != '"') return fail("expect key");
            if (!parseString(&key)) return false;
            skipWs();
            if (p >= end || *p != ':') return fail("expect :");
            ++p;
            Value v;
            if (!parseValue(&v, depth + 1)) return false;
            obj.emplace(std::move(key), std::move(v));
            skipWs();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == '}') { ++p; *out = Value(std::move(obj)); return true; }
            return fail("expect , or }");
        }
    }
};

bool ReadFile(const std::string& path, std::string* out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) { std::fclose(f); return false; }
    out->resize(static_cast<size_t>(n));
    size_t got = n > 0 ? std::fread(out->data(), 1, static_cast<size_t>(n), f) : 0;
    std::fclose(f);
    // 去掉 UTF-8 BOM（Windows 工具常写入）
    if (out->size() >= 3 && static_cast<unsigned char>((*out)[0]) == 0xEF &&
        static_cast<unsigned char>((*out)[1]) == 0xBB &&
        static_cast<unsigned char>((*out)[2]) == 0xBF) {
        out->erase(0, 3);
    }
    return got == static_cast<size_t>(n) || out->size() == static_cast<size_t>(n) - 3;
}

}  // namespace

std::unique_ptr<Value> Parse(const std::string& text, std::string* err) {
    Parser ps{text.data(), text.data() + text.size(), {}};
    Value v;
    if (!ps.parseValue(&v, 0)) {
        if (err) *err = ps.err;
        return nullptr;
    }
    if (err) err->clear();
    return std::make_unique<Value>(std::move(v));
}

std::unique_ptr<Value> ParseFile(const std::string& path, std::string* err) {
    std::string text;
    if (!ReadFile(path, &text)) {
        if (err) *err = "cannot read file: " + path;
        return nullptr;
    }
    return Parse(text, err);
}

}  // namespace vta::json
