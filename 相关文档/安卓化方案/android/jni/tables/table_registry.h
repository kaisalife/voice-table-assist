// jni/tables/table_registry.h —— 翻译自 Services/TableRegistry.cs
#pragma once

#include "../common/mutex_compat.h"
#include <string>
#include <vector>

#include "../common/strings.h"

namespace vta {

struct TableEntry {
    std::string name;
    std::string key;
    int rowsCount = 0;
    int colsCount = 0;
    int dim = 0;
    int64_t importedAtMs = 0;  // C# DateTime → epoch ms
};

// 多表注册表存储：维护 name↔key 映射清单，读写 registry.json（写时拷贝原子替换）。
// 损坏时备份 .bak 并重建空清单（不崩）。key 一律经此解析，杜绝路径穿越。
class TableRegistry {
public:
    TableRegistry(const std::string& tablesBaseDir, const std::string& defaultTable);

    const std::string& DefaultTable() const { return defaultTable_; }

    // 线程安全快照
    std::vector<TableEntry> Snapshot() const;

    // 登记/更新表，返回其 key；新表用 SanitizeTableKey，撞名追加数字后缀。
    std::string EnsureRegistered(const std::string& name, int rows, int cols, int dim);

    // 表名→key；未注册（非 default 且无记录）返回空串（调用方转错误码）。
    std::string ResolveExistingKey(const std::string& name) const;

    void Write() const;

    // 生成文件系统安全键：保留中文/数字/下划线/连字符，其余替换为 _（防路径穿越）。
    static std::string SanitizeTableKey(const std::string& name);

private:
    void Load();

    std::string tablesBaseDir_;
    std::string defaultTable_;
    mutable std::mutex gate_;
    std::vector<TableEntry> entries_;
};

}  // namespace vta
