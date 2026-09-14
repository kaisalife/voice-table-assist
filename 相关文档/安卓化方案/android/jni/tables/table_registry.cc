// jni/tables/table_registry.cc —— 翻译自 Services/TableRegistry.cs
#include "table_registry.h"

#include <cctype>
#include <cstdio>

#include "../common/json.h"
#include "../common/log.h"
#include "../common/strings.h"

namespace vta {

namespace {

// JSON 字符串转义（registry.json 手写序列化用）
std::string JsonEscape(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out += "\"";
    return out;
}

}  // namespace

TableRegistry::TableRegistry(const std::string& tablesBaseDir, const std::string& defaultTable)
    : tablesBaseDir_(tablesBaseDir), defaultTable_(defaultTable) {
    Load();
}

std::vector<TableEntry> TableRegistry::Snapshot() const {
    std::lock_guard<std::mutex> lk(gate_);
    return entries_;
}

std::string TableRegistry::EnsureRegistered(const std::string& name, int rows, int cols, int dim) {
    std::lock_guard<std::mutex> lk(gate_);
    for (auto& e : entries_) {
        if (e.name == name) {
            e.rowsCount = rows;
            e.colsCount = cols;
            e.dim = dim;
            e.importedAtMs = NowMs();
            return e.key;
        }
    }
    std::string baseKey = SanitizeTableKey(name);
    std::string key = baseKey;
    int suffix = 1;
    while (true) {
        bool hit = false;
        for (const auto& e : entries_)
            if (e.key == key) { hit = true; break; }
        if (!hit) break;
        key = baseKey + "_" + std::to_string(suffix++);
    }
    entries_.push_back({name, key, rows, cols, dim, NowMs()});
    return key;
}

std::string TableRegistry::ResolveExistingKey(const std::string& name) const {
    if (name == defaultTable_) return defaultTable_;
    std::lock_guard<std::mutex> lk(gate_);
    for (const auto& e : entries_)
        if (e.name == name || e.key == name) return e.key;
    return {};  // 未导入（调用方转错误码）
}

void TableRegistry::Load() {
    std::string registryPath = tablesBaseDir_ + "/registry.json";
    auto doc = json::ParseFile(registryPath);
    if (doc) {
        for (const auto& t : doc->at("tables").asArray()) {
            // 属性名兼容读取：优先 camelCase（现行写法），回退 PascalCase（旧版本文件）。
            // 曾因大小写不一致导致重启后注册表被误判损坏、全部表 404。
            auto prop = [](const json::Value& el, const std::string& camel) -> const json::Value& {
                if (!el.at(camel).isNull()) return el.at(camel);
                std::string pascalName = camel;
                if (!pascalName.empty())
                    pascalName[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(pascalName[0])));
                return el.at(pascalName);
            };
            TableEntry e;
            e.name = prop(t, "name").asString();
            e.key = prop(t, "key").asString();
            e.rowsCount = prop(t, "rowsCount").asInt();
            e.colsCount = prop(t, "colsCount").asInt();
            e.dim = prop(t, "dim").asInt();
            e.importedAtMs = ParseIso8601ToMs(prop(t, "importedAt").asString());
            entries_.push_back(std::move(e));
        }
        return;
    }
    // 首次启动且存在旧单文件 cell_index.json ↔ 登记 default（向后兼容；Android 端通常无此文件）
    std::string legacyPath = tablesBaseDir_ + "/../cell_index.json";
    if (FileExists(legacyPath)) {
        entries_.push_back({"default", defaultTable_, 0, 0, 0, NowMs()});
        Write();
    }
}

void TableRegistry::Write() const {
    // {"tables":[{name,key,rowsCount,colsCount,dim,importedAt}]}
    std::string payload = "{\"tables\":[";
    std::lock_guard<std::mutex> lk(gate_);
    bool first = true;
    for (const auto& e : entries_) {
        if (!first) payload += ",";
        first = false;
        payload += "{\"name\":" + JsonEscape(e.name);
        payload += ",\"key\":" + JsonEscape(e.key);
        payload += ",\"rowsCount\":" + std::to_string(e.rowsCount);
        payload += ",\"colsCount\":" + std::to_string(e.colsCount);
        payload += ",\"dim\":" + std::to_string(e.dim);
        payload += ",\"importedAt\":\"" + MsToIso8601(e.importedAtMs) + "\"}";
    }
    payload += "]}";
    std::string registryPath = tablesBaseDir_ + "/registry.json";
    // 写时拷贝：先写临时文件再 rename 覆盖，避免读到半成品
    if (!WriteFileAtomic(registryPath, payload.data(), payload.size()))
        ALOGW("[TABLES] registry 写入失败: %s", registryPath.c_str());
}

std::string TableRegistry::SanitizeTableKey(const std::string& name) {
    auto cps = Utf8ToCodePoints(name);
    std::string sb;
    for (CodePoint cp : cps) {
        bool asciiAlnum = (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || IsAsciiDigit(cp);
        bool invalidWin = cp == '.' || cp == '"' || cp == '*' || cp == ':' || cp == '<' ||
                          cp == '>' || cp == '?' || cp == '|' || cp == '/' || cp == '\\' || cp < 0x20;
        if (invalidWin) {
            sb += '_';
        } else if (asciiAlnum || cp == '_' || cp == '-' || IsCjk(cp)) {
            AppendUtf8(cp, &sb);
        }
        // 其余（空白、其他 Unicode 标点）丢弃——与 C# 分支语义一致
    }
    std::string trimmed = TrimUnderscore(sb);
    return trimmed.empty() ? "table" : trimmed;
}

}  // namespace vta
