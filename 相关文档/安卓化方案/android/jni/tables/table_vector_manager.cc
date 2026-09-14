// jni/tables/table_vector_manager.cc —— 翻译自 Services/TableVectorManager.cs
#include "table_vector_manager.h"

#include <algorithm>
#include <tuple>

#include "../common/log.h"
#include "../common/strings.h"
#include "../embed/embedder.h"
#include "../text/cell_phrase_generator.h"
#include "voice_resource.h"

namespace vta {

TableVectorManager::TableVectorManager(const std::string& tablesBaseDir,
                                       const std::string& hrTablesRoot,
                                       const std::string& charPinyinPath,
                                       const std::string& commonRulesPath,
                                       const std::string& defaultTable)
    : tablesBaseDir_(tablesBaseDir),
      hrTablesRoot_(hrTablesRoot),
      charPinyinPath_(charPinyinPath),
      commonRulesPath_(commonRulesPath),
      registry_(new TableRegistry(tablesBaseDir, defaultTable)) {}

std::string TableVectorManager::ActiveTable() const {
    std::lock_guard<std::mutex> lk(lock_);
    return lastDatabase_;
}

std::string TableVectorManager::ResolveTargetKey(const std::string& tableName) const {
    if (Trim(tableName).empty()) {
        std::lock_guard<std::mutex> lk(lock_);
        // 空表名 → 最近用过的表（若仍在缓存）→ 默认表
        if (!lastDatabase_.empty() && cache_.find(lastDatabase_) != cache_.end())
            return lastDatabase_;
        return registry_->ResolveExistingKey(DefaultTable());
    }
    return registry_->ResolveExistingKey(Trim(tableName));
}

std::shared_ptr<const VtxIndex> TableVectorManager::Activate(const std::string& tableName) {
    std::string name = Trim(tableName);
    std::lock_guard<std::mutex> lk(lock_);
    if (name.empty()) {
        if (!lastDatabase_.empty()) {
            auto it = cache_.find(lastDatabase_);
            if (it != cache_.end() && it->second) return it->second;
        }
        name = DefaultTable();
    }

    std::string key = registry_->ResolveExistingKey(name);
    if (key.empty()) return nullptr;

    // 命中缓存：直接返回同一份（多表并发时各会话共享只读索引）
    auto it = cache_.find(key);
    if (it != cache_.end() && it->second) {
        lastDatabase_ = key;
        return it->second;
    }

    auto idx = LoadIndex(key);
    if (!idx) return nullptr;

    // 缓存兜底上限：只驱逐"无会话引用"的旧表（use_count==1 表示仅缓存持有）
    if (cache_.size() >= maxCachedTables_) {
        for (auto e = cache_.begin(); e != cache_.end();) {
            if (e->second.use_count() == 1) {
                ALOGI("[TABLES] 缓存驱逐 key=%s（无会话引用）", e->first.c_str());
                e = cache_.erase(e);
            } else {
                ++e;
            }
        }
    }
    cache_[key] = idx;
    lastDatabase_ = key;
    ALOGI("[TABLES] 激活表 key=%s (%dx%d@%d)，缓存 %zu 张", key.c_str(), idx->rowsCount,
          idx->colsCount, idx->dim, cache_.size());
    return idx;
}

bool TableVectorManager::Import(const std::string& tableName,
                                const std::vector<std::string>& rowLabels, int columnCount,
                                EmbeddingEngine* embedder, ImportSummary* out, std::string* err) {
    std::string name = Trim(tableName);
    if (name.empty()) {
        if (err) *err = "tableName 不能为空";
        return false;
    }

    std::vector<std::tuple<int, int, std::string>> allPhrases;  // (row, col, phrase)
    // 规范坐标：行号 = rows 数组顺序 1..N（第一个行标签=1），列号 = 1..columnCount（第一个列标签=1）。
    // 与前端 UI 的原始坐标完全解耦——前端收到返回后按自身表格布局做固定偏移换算。
    for (size_t i = 0; i < rowLabels.size(); ++i) {
        int normRow = static_cast<int>(i) + 1;
        for (int col = 1; col <= columnCount; ++col) {
            for (const auto& p : GenerateCellPhrases(normRow, rowLabels[i], col))
                allPhrases.emplace_back(normRow, col, p);
        }
    }

    // 批量嵌入：将短语分批交给 ONNX 推理，大幅减少推理调用次数
    const int batchSize = 64;
    VtxIndex index;
    for (size_t i = 0; i < allPhrases.size(); i += static_cast<size_t>(batchSize)) {
        size_t take = std::min(static_cast<size_t>(batchSize), allPhrases.size() - i);
        std::vector<std::string> texts;
        texts.reserve(take);
        for (size_t j = 0; j < take; ++j) texts.push_back(std::get<2>(allPhrases[i + j]));
        auto vecs = embedder->EmbedBatch(texts);
        for (size_t j = 0; j < take; ++j) {
            VtxCell cell;
            cell.row = std::get<0>(allPhrases[i + j]);
            cell.col = std::get<1>(allPhrases[i + j]);
            cell.phrase = std::get<2>(allPhrases[i + j]);
            cell.vec = std::move(vecs[j]);
            index.entries.push_back(std::move(cell));
        }
    }

    index.dim = index.entries.empty() ? 0 : static_cast<int>(index.entries[0].vec.size());
    index.rows = rowLabels;
    index.rowsCount = static_cast<int>(rowLabels.size());
    index.colsCount = columnCount;

    std::string key = registry_->EnsureRegistered(name, index.rowsCount, columnCount, index.dim);
    {
        std::lock_guard<std::mutex> lk(lock_);
        std::string cellPath = CellIndexPath(key);
        // 二进制 VTX1 落盘（内部已原子替换）
        std::string saveErr;
        if (!Vtx1Save(cellPath, index, &saveErr)) {
            if (err) *err = "索引落盘失败: " + saveErr;
            return false;
        }
        registry_->Write();
        ALOGI("[IMPORT] 表 %s(key=%s) 落盘 %s：%zu条向量", name.c_str(), key.c_str(),
              cellPath.c_str(), index.entries.size());

        // 写入/刷新缓存（共享所有权：正在跑的会话持有的旧索引不受影响）
        cache_[key] = std::make_shared<const VtxIndex>(index);
        lastDatabase_ = key;
    }

    if (out) {
        out->rowsCount = index.rowsCount;
        out->colsCount = columnCount;
        out->entries = static_cast<int>(index.entries.size());
        out->dim = index.dim;
    }
    return true;
}

void TableVectorManager::Unload(const std::string& tableName) {
    std::lock_guard<std::mutex> lk(lock_);
    if (Trim(tableName).empty()) {
        // 只驱逐无会话引用的（有会话在用的等它们结束再走）
        for (auto e = cache_.begin(); e != cache_.end();) {
            if (e->second.use_count() == 1) e = cache_.erase(e);
            else ++e;
        }
        ALOGI("[TABLES] 已卸载（剩余缓存 %zu 张）", cache_.size());
        return;
    }
    std::string key = registry_->ResolveExistingKey(Trim(tableName));
    if (key.empty()) return;
    auto it = cache_.find(key);
    if (it == cache_.end()) return;
    if (it->second.use_count() > 1) {
        ALOGW("[TABLES] 表 %s 仍被会话引用，暂不卸载", key.c_str());
        return;
    }
    cache_.erase(it);
    if (lastDatabase_ == key) lastDatabase_.clear();
    ALOGI("[TABLES] 已卸载表 %s", key.c_str());
}

std::shared_ptr<const VtxIndex> TableVectorManager::LoadIndex(const std::string& key) {
    std::string path = CellIndexPath(key);
    if (!FileExists(path)) return nullptr;
    auto idx = std::make_shared<VtxIndex>();
    std::string err;
    if (!Vtx1Load(path, idx.get(), &err)) {
        ALOGW("[TABLES] 加载索引失败 key=%s path=%s: %s", key.c_str(), path.c_str(), err.c_str());
        return nullptr;
    }
    return idx;
}

std::string TableVectorManager::RebuildVoiceResources(const std::string& tableKey,
                                                      const std::vector<std::string>& rowLabels,
                                                      int columnCount) {
    // 目标目录：default 表 → tables/current（向后兼容）；其余 → tables/{key}
    bool isDefault = tableKey.empty() || tableKey == "default";
    std::string tableDir = isDefault ? hrTablesRoot_ + "/current" : hrTablesRoot_ + "/" + tableKey;
    return TableVoiceResourceGenerator::Rebuild(charPinyinPath_, commonRulesPath_, hrTablesRoot_,
                                                tableDir, rowLabels, columnCount);
}

}  // namespace vta
