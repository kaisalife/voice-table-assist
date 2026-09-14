// jni/service/vta_service_impl.cc —— AIDL 服务端实现
#include "vta_service_impl.h"

#include <algorithm>
#include <optional>

#include "../common/log.h"
#include "../common/strings.h"
#include "../embed/embedder.h"
#include "../ner/raner_engine.h"
#include "../service/audio_capture.h"
#include "../tables/table_vector_manager.h"

namespace vta {

namespace aidl = aidl::com::vta;

namespace {

VtaServiceImpl* g_serviceInstance = nullptr;
AIBinder_DeathRecipient* g_deathRecipient = nullptr;

void DeathCb(void* cookie) {
    // cookie = ClientKey*（openSession 时 new，会话全清后 delete）
    ALOGW("[BINDER] client 死亡，回收其会话 cookie=%p", cookie);
    if (g_serviceInstance != nullptr) g_serviceInstance->OnBinderDied(cookie);
}

AIBinder_DeathRecipient* GetDeathRecipient() {
    if (g_deathRecipient == nullptr) g_deathRecipient = AIBinder_DeathRecipient_new(DeathCb);
    return g_deathRecipient;
}
}  // namespace

// ClientKey：client 生命周期 cookie（death recipient / 会话归属共用）
struct ClientKey {
    explicit ClientKey(VtaServiceImpl* s) : svc(s) {}
    VtaServiceImpl* svc;
};

VtaServiceImpl::VtaServiceImpl(const VtaConfig& config) : config_(config) {
    g_serviceInstance = this;
    denoiseEnabled_ = config.denoiseEnabled;
    host_ = std::make_unique<EngineHost>(config);
}

VtaServiceImpl::~VtaServiceImpl() {
    if (g_serviceInstance == this) g_serviceInstance = nullptr;
    std::lock_guard<std::mutex> lk(mu_);
    sessions_.clear();
    clients_.clear();
}

bool VtaServiceImpl::Init(std::string* err) {
    manager_ = std::make_unique<TableVectorManager>(config_.tablesBaseDir, config_.defaultTable);
    healthState_ = "ok";
    ALOGI("[VTA] init ok. defaultTable=%s tables=%s", config_.defaultTable.c_str(),
          config_.tablesBaseDir.c_str());
    (void)err;
    return true;
}

// ---- 生命周期 ----

::ndk::ScopedAStatus VtaServiceImpl::getVersion(int32_t* _aidl_return) {
    *_aidl_return = 1;
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus VtaServiceImpl::health(std::string* _aidl_return) {
    *_aidl_return = healthState_;
    return ::ndk::ScopedAStatus::ok();
}

// ---- 表结构（测试页渲染用）----

std::string VtaServiceImpl::DescribeTable(const std::string& tableName) {
    std::string key = manager_->ResolveTargetKey(tableName);
    if (key.empty()) return {};
    // 先取注册表元数据（name/rows/cols），再激活索引拿行标签
    std::string name;
    int rows = 0, cols = 0;
    for (const auto& e : manager_->ListTables()) {
        if (e.key == key || e.name == tableName) {
            name = e.name;
            rows = e.rowsCount;
            cols = e.colsCount;
            if (e.key == key) break;
        }
    }
    std::shared_ptr<const VtxIndex> idx = manager_->Activate(key);
    if (idx != nullptr) {
        cols = idx->colsCount;
        if (!idx->rows.empty()) rows = static_cast<int>(idx->rows.size());
    }
    std::string out = name + "|" + std::to_string(rows) + "|" + std::to_string(cols);
    if (idx != nullptr) {
        for (const auto& r : idx->rows) out += "\n" + r;
    }
    ALOGI("[TABLES] DescribeTable %s -> %d行x%d列", name.c_str(), rows, cols);
    return out;
}

// ---- 表格管理 ----

::ndk::ScopedAStatus VtaServiceImpl::listTables(std::vector<aidl::TableInfo>* _aidl_return) {
    _aidl_return->clear();
    for (const auto& e : manager_->ListTables()) {
        aidl::TableInfo info;
        info.name = e.name;
        info.key = e.key;
        info.rows = e.rowsCount;
        info.columns = e.colsCount;
        info.updatedAtMs = e.importedAtMs;
        _aidl_return->push_back(std::move(info));
    }
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus VtaServiceImpl::getTable(const std::string& name,
                                              std::optional<aidl::TableInfo>* _aidl_return) {
    for (const auto& e : manager_->ListTables()) {
        if (e.name == name || e.key == name) {
            aidl::TableInfo info;
            info.name = e.name;
            info.key = e.key;
            info.rows = e.rowsCount;
            info.columns = e.colsCount;
            info.updatedAtMs = e.importedAtMs;
            *_aidl_return = std::move(info);
            return ::ndk::ScopedAStatus::ok();
        }
    }
    _aidl_return->reset();  // 未注册返回 null
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus VtaServiceImpl::importTable(const std::string& name,
                                                 const std::vector<std::string>& rowLabels,
                                                 int32_t columnCount, int32_t* _aidl_return) {
    if (Trim(name).empty() || rowLabels.empty() || columnCount <= 0) {
        *_aidl_return = kErrInvalid;
        return ::ndk::ScopedAStatus::ok();
    }
    std::string err;
    if (!host_->EnsureEngines(nullptr, &err)) {
        healthState_ = "error:" + err;
        *_aidl_return = kErrNotReady;
        return ::ndk::ScopedAStatus::ok();
    }
    ImportSummary summary;
    if (!manager_->Import(name, rowLabels, columnCount, host_->Embed(), &summary, &err)) {
        ALOGW("[IMPORT] 失败: %s", err.c_str());
        *_aidl_return = kErrInternal;
        return ::ndk::ScopedAStatus::ok();
    }
    // 导入即完成：向量索引 + registry 落盘。语音侧不需要重建任何资源——
    //   热词已移除；同音纠错靠"表内读音吸附"，而吸附词表是**会话打开时按该表实时构建**的，
    //   所以新导入的表下一次 openSession 自动生效（零重建、零额外工作）。
    std::string key = manager_->ResolveTargetKey(name);
    ALOGI("[IMPORT] 表 %s(key=%s)：%d行x%d列 %d条向量（下次 openSession 即生效）", name.c_str(),
          key.c_str(), summary.rowsCount, summary.colsCount, summary.entries);
    *_aidl_return = kOk;
    return ::ndk::ScopedAStatus::ok();
}

// ---- 语音会话 ----

int VtaServiceImpl::OpenSessionLocked(const std::string& tableName, int silenceMs, int captureMode,
                                      ClientKey* clientKey) {
    if (captureMode != 0 && captureMode != 1) return kErrInvalid;

    std::string key = manager_->ResolveTargetKey(tableName);
    if (key.empty()) return kErrInvalid;  // 表未注册

    // 并发门卫：同一 client 同一张表只能一路（不同表可各开一路）；
    // 全局并发上限 maxSessions（多表并发时按内存预算配置，默认 4）
    ClientRecord& client = clients_[clientKey];
    if (static_cast<int>(sessions_.size()) >= config_.maxSessions) return kErrConflict;

    // 懒加载：首次 openSession 时装载引擎/识别器（progress 走 health 状态）
    std::string err;
    healthState_ = "loading:正在加载模型（首次使用需数秒）...";
    if (!host_->EnsureEngines(nullptr, &err)) {
        healthState_ = "error:" + err;
        return kErrNotReady;
    }
    // ASR 识别器：不带热词文件（同音纠错全交给表内读音吸附，见方案文档）
    if (!host_->EnsureAsr("", &err)) {
        healthState_ = "error:" + err;
        return kErrNotReady;
    }
    healthState_ = "ok";
    // 取该表索引快照（共享所有权）：多表并发时其他会话切表不会让本会话悬空
    std::shared_ptr<const VtxIndex> index = manager_->Activate(key);
    if (index == nullptr) return kErrInvalid;

    // 并发门卫：同一 client 同一张表只能一路（不同表可各开一路）
    for (int sid : client.sessionIds) {
        auto it = sessions_.find(sid);
        if (it != sessions_.end() && it->second->tableKey == key) return kErrConflict;
    }
    if (static_cast<int>(sessions_.size()) >= config_.maxSessions) return kErrConflict;

    VoiceSessionConfig cfg;
    cfg.tableName = tableName;
    cfg.tableKey = key;
    cfg.silenceMs = silenceMs > 0 ? silenceMs : config_.silenceMs;
    cfg.maxChars = config_.maxChars;
    cfg.minSim = config_.minSim;
    cfg.captureMode = captureMode;
    cfg.pinyinPath = config_.charPinyinPath;  // 表内读音对齐用
    cfg.hotwordDigits = config_.hotwordDigits;
    cfg.hotwordDigitTen = config_.hotwordDigitTen;

    // 表内读音吸附：会话只需该表的索引 + 读音词表
    int id = nextSessionId_++;
    auto record = std::make_unique<SessionRecord>();
    record->id = id;
    record->client = clientKey;
    record->tableKey = key;
    record->session =
        VoiceSession::CreateAndStart(id, cfg, host_.get(), index, &err);
    if (record->session == nullptr) {
        ALOGW("[SESSION] 打开会话失败: %s", err.c_str());
        return kErrInternal;
    }
    sessions_[id] = std::move(record);
    client.sessionIds.push_back(id);
    host_->OnSessionOpened();

    // captureMode=0：确保采音在跑（权限在原生层解决；失败则要求改用 pushPcm 模式）
    if (captureMode == 0 && !AudioCapture::Running()) {
        std::string capErr;
        bool ok = AudioCapture::Start(
            [](const float* s, int n) { VtaServiceImpl::RouteCapturedAudio(s, n); }, &capErr);
        if (!ok) {
            ALOGW("[CAPTURE] 启动失败: %s（可改用 captureMode=1 pushPcm）", capErr.c_str());
            CloseSessionInternal(id);
            return kErrNotReady;
        }
    }
    return id;
}

::ndk::ScopedAStatus VtaServiceImpl::openSession(const std::string& tableName, int32_t silenceMs,
                                                 int32_t captureMode,
                                                 const ::ndk::SpAIBinder& client,
                                                 int32_t* _aidl_return) {
    ClientKey* key = new ClientKey(this);
    bool linked = false;
    if (client.get() != nullptr) {
        // binder 断开 → 自动 close 该 client 全部会话（方案 §8.4）
        if (AIBinder_linkToDeath(client.get(), GetDeathRecipient(), key) == STATUS_OK) linked = true;
    }
    std::lock_guard<std::mutex> lk(mu_);
    *_aidl_return = OpenSessionLocked(tableName, silenceMs, captureMode, key);
    if (*_aidl_return <= 0) {
        // 失败路径：清掉可能残留的空 client 登记（含 capture 启动失败已清的情形）
        auto it = clients_.find(key);
        if (it != clients_.end() && it->second.sessionIds.empty()) clients_.erase(it);
        if (linked) AIBinder_unlinkToDeath(client.get(), GetDeathRecipient(), key);
        delete key;
    }
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus VtaServiceImpl::pushPcm(int32_t sessionId, const aidl::PcmFrame& frame,
                                             int32_t* _aidl_return) {
    std::shared_ptr<SessionRecord> rec;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) { *_aidl_return = kErrInvalid; return ::ndk::ScopedAStatus::ok(); }
        rec = it->second;
    }
    if (rec->session->CaptureMode() != 1) { *_aidl_return = kErrInvalid; return ::ndk::ScopedAStatus::ok(); }
    // 单块上限 4096 样本（256ms）：高频小块比低频大块更省 binder 事务（方案 §9.1）
    size_t n = frame.samples.size();
    if (n == 0) { *_aidl_return = kOk; return ::ndk::ScopedAStatus::ok(); }  // 空块不崩
    if (n > 4096) { *_aidl_return = kErrInvalid; return ::ndk::ScopedAStatus::ok(); }
    rec->session->PushPcm(frame.samples.data(), n);
    *_aidl_return = kOk;
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus VtaServiceImpl::getState(int32_t sessionId,
                                              aidl::SessionState* _aidl_return) {
    std::shared_ptr<SessionRecord> rec;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(sessionId);
        if (it != sessions_.end()) rec = it->second;
    }
    if (!rec) {
        _aidl_return->sessionId = sessionId;
        _aidl_return->phase = "closed";
        _aidl_return->lastPartialMs = 0;
        return ::ndk::ScopedAStatus::ok();
    }
    auto st = rec->session->GetState();
    _aidl_return->sessionId = sessionId;
    _aidl_return->tableName = rec->session->TableName();
    _aidl_return->phase = st.phase;
    _aidl_return->partial = st.partial;
    _aidl_return->lastPartialMs = st.lastPartialMs;
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus VtaServiceImpl::pollCells(int32_t sessionId, int32_t maxN,
                                               std::vector<aidl::CellHit>* _aidl_return) {
    std::vector<CellHitInternal> hits;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(sessionId);
        if (it != sessions_.end()) {
            // PollCells 最多等 250ms（阻塞拉取但不拖死 binder 调用）
            hits = it->second->session->PollCells(maxN > 0 ? maxN : 16, 250);
        } else {
            // 会话已关闭：返回 closeSession 收尾时排出的剩余 cells（一次性）
            auto dit = drainedCells_.find(sessionId);
            if (dit != drainedCells_.end()) {
                hits = std::move(dit->second);
                drainedCells_.erase(dit);
            }
        }
    }
    _aidl_return->clear();
    for (auto& h : hits) {
        aidl::CellHit c;
        // AIDL 契约 0-indexed：内部向量库 1-based，此处 -1 换算
        c.row = h.row > 0 ? h.row - 1 : 0;
        c.column = h.column > 0 ? h.column - 1 : 0;
        c.value = h.value;
        c.raw = h.raw;
        c.finalizedAtMs = h.finalizedAtMs;
        _aidl_return->push_back(std::move(c));
    }
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus VtaServiceImpl::closeSession(int32_t sessionId, int32_t* _aidl_return) {
    std::shared_ptr<SessionRecord> rec;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) { *_aidl_return = kErrInvalid; return ::ndk::ScopedAStatus::ok(); }
        rec = it->second;
    }
    // Close 阻塞收尾（flush + 提交），不持 mu_（pollCells 等调用仍可进）
    rec->session->Close();
    {
        std::lock_guard<std::mutex> lk(mu_);
        // 排出收尾 flush 产生的最后一批 cells，转入 drained 缓冲供下一次 pollCells 取走
        auto rest = rec->session->PollCells(1 << 20, 0);
        if (!rest.empty()) drainedCells_[sessionId] = std::move(rest);
        CloseSessionInternal(sessionId);
    }
    *_aidl_return = kOk;
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus VtaServiceImpl::setDenoise(bool enabled, int32_t* _aidl_return) {
    denoiseEnabled_ = enabled;
    host_->SetDenoiseEnabled(enabled);
    ALOGI("[DENOISE] 运行时开关 -> %s（对新会话生效）", enabled ? "on" : "off");
    *_aidl_return = kOk;
    return ::ndk::ScopedAStatus::ok();
}

// ---- 会话簿记 ----

void VtaServiceImpl::CloseSessionInternal(int sessionId) {
    // 调用方须持 mu_
    auto it = sessions_.find(sessionId);
    if (it == sessions_.end()) return;
    ClientKey* client = static_cast<ClientKey*>(it->second->client);
    sessions_.erase(it);
    host_->OnSessionClosed();
    auto cit = clients_.find(client);
    if (cit != clients_.end()) {
        auto& v = cit->second.sessionIds;
        v.erase(std::remove(v.begin(), v.end(), sessionId), v.end());
        if (v.empty()) clients_.erase(cit);
    }
    // 采音：无 capture 会话了就停（省电）
    bool anyCapture = false;
    for (auto& [id, rec] : sessions_)
        if (rec->session && rec->session->CaptureMode() == 0) anyCapture = true;
    if (!anyCapture) AudioCapture::Stop();
}

void VtaServiceImpl::CloseSessionsOf(void* clientCookie) {
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto cit = clients_.find(static_cast<ClientKey*>(clientCookie));
        if (cit == clients_.end()) {
            delete static_cast<ClientKey*>(clientCookie);
            return;
        }
        ids = cit->second.sessionIds;
    }
    for (int id : ids) {
        std::shared_ptr<SessionRecord> rec;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = sessions_.find(id);
            if (it != sessions_.end()) rec = it->second;
        }
        if (rec) rec->session->Close();
        std::lock_guard<std::mutex> lk(mu_);
        CloseSessionInternal(id);
    }
    delete static_cast<ClientKey*>(clientCookie);
}

void VtaServiceImpl::OnBinderDied(void* cookie) { CloseSessionsOf(cookie); }

void VtaServiceImpl::RouteCapturedAudio(const float* samples, int n) {
    if (g_serviceInstance == nullptr) return;
    std::vector<std::shared_ptr<SessionRecord>> targets;
    {
        std::lock_guard<std::mutex> lk(g_serviceInstance->mu_);
        // broadcastCapture=true：同一份 PCM 广播给所有自主采音会话（多表同时监听，谁的表匹配上谁填）
        // broadcastCapture=false：仅最近打开的自主采音会话独占麦克风
        std::shared_ptr<SessionRecord> latest;
        for (auto& [id, rec] : g_serviceInstance->sessions_) {
            if (rec->session && rec->session->CaptureMode() == 0) {
                if (!g_serviceInstance->config_.broadcastCapture) latest = rec;
                else targets.push_back(rec);
            }
        }
        if (!g_serviceInstance->config_.broadcastCapture && latest) targets.push_back(latest);
    }
    for (auto& rec : targets) rec->session->OnCapturedAudio(samples, n);
}

}  // namespace vta
