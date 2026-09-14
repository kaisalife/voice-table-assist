// jni/tables/table_vector_manager.h —— 翻译自 Services/TableVectorManager.cs
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../embed/vtx1.h"
#include "table_registry.h"

namespace vta {

class EmbeddingEngine;
class AsrRecognizer;

struct ImportSummary {
    int rowsCount = 0;
    int colsCount = 0;
    int entries = 0;
    int dim = 0;
};

// 多表向量库管理器（单例）。
// 按表持久化 embedding 索引（tables/{key}/cell_index.bin 二进制 VTX1 + registry.json），
// 查询/导入按表名切换，卸载只释放内存，绝不删盘。key 一律经 TableRegistry 解析，杜绝路径穿越。
class TableVectorManager {
public:
    explicit TableVectorManager(const std::string& tablesBaseDir,
                                const std::string& defaultTable);

    const std::string& DefaultTable() const { return registry_->DefaultTable(); }
    std::string ActiveTable() const;

    // 取表索引（共享所有权，按 key 缓存）。空表名 → 当前活动表 → 默认表；
    // 未注册的表返回 nullptr（调用方转错误码）。
    //
    // 多表并发安全：返回 shared_ptr，调用方（会话）持有期间该索引不会被释放——
    // 即使其他会话 Acquire 了别的表，也不会让本会话的索引悬空。
    std::shared_ptr<const VtxIndex> Activate(const std::string& tableName);

    // 解析目标表 key（快操作，纯内存）；未注册返回空串。
    std::string ResolveTargetKey(const std::string& tableName) const;

    // 导入表：构建索引 → 写时拷贝落盘 → 更新 registry → 激活。
    // embedder 由调用方注入（懒加载完成后）。失败返回 ok=false。
    bool Import(const std::string& tableName, const std::vector<std::string>& rowLabels,
                int columnCount, EmbeddingEngine* embedder, ImportSummary* out,
                std::string* err);

    // 卸载：tableName 空 → 清空全部缓存；否则只清指定表（只释放内存，绝不删盘）。
    void Unload(const std::string& tableName);

    std::vector<TableEntry> ListTables() const { return registry_->Snapshot(); }

private:
    std::string CellIndexDir(const std::string& key) const { return tablesBaseDir_ + "/" + key; }
    std::string CellIndexPath(const std::string& key) const {
        return CellIndexDir(key) + "/cell_index.bin";
    }
    std::shared_ptr<const VtxIndex> LoadIndex(const std::string& key);

    std::string tablesBaseDir_;
    std::unique_ptr<TableRegistry> registry_;
    mutable std::mutex lock_;

    // 按 key 缓存已加载索引（共享所有权）：多表并发时各会话持有自己那份，互不释放。
    // 值很小（6×6 表约 2MB），缓存上限用于兜底防极端情况。
    std::map<std::string, std::shared_ptr<const VtxIndex>> cache_;
    std::string lastDatabase_;  // 最近 Acquire 的表 key（仅用于 ActiveTable() 兼容查询）
    size_t maxCachedTables_ = 16;
};

}  // namespace vta
