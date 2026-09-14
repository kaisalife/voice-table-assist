// jni/service/vta_service_impl.cc —— 语音表格服务实现（单会话模型）
#include "vta_service_impl.h"

#include "../common/log.h"
#include "../common/strings.h"
#include "../embed/embedder.h"
#include "../ner/raner_engine.h"
#include "../service/audio_capture.h"
#include "../tables/table_vector_manager.h"

namespace vta {

namespace aidl = aidl::com::vta;

namespace {

VtaServiceImpl* g_instance = nullptr;
AIBinder_DeathRecipient* g_deathRecipient = nullptr;

void DeathCb(void* cookie) {
    // cookie = client AIBinder*（openSession 时 linkToDeath 登记）
    ALOGW("[BINDER] client 死亡，回收其会话 cookie=%p", cookie);
    if (g_instance != nullptr) g_instance->OnBinderDied(cookie);
}

AIBinder_DeathRecipient* GetDeathRecipient() {
    if (g_deathRecipient == nullptr) g_deathRecipient = AIBinder_DeathRecipient_new(DeathCb);
    return g_deathRecipient;
}

}  // namespace

VtaServiceImpl* VtaServiceImpl::g_instance = nullptr;  // 采音回调路由用（静态成员定义）

VtaServiceImpl::VtaServiceImpl(const VtaConfig& config) : config_(config) {
    g_instance = this;
    denoiseEnabled_ = config.denoiseEnabled;
    host_ = std::make_unique<EngineHost>(config);
}

VtaServiceImpl::~VtaServiceImpl() {
    if (g_instance == this) g_instance = nullptr;
    std::lock_guard<std::mutex> lk(mu_);
    active_.reset();
    drainedCells_.clear();
}

bool VtaServiceImpl::Init(std::string* err) {
    manager_ = std::make_unique<TableVectorManager>(config_.tablesBaseDir, config_.defaultTable);
    healthState_ = "ok";
    ALOGI("[VTA] init ok. defaultTable=%s tables=%s", config_.defaultTable.c_str(),
          config_.tablesBaseDir.c_str());
    (void)err;
    return true;
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

::ndk::ScopedAStatus VtaServiceImpl::getVersion(int32_t* _aidl_return) {
    *_aidl_return = 1;
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus VtaServiceImpl::health(std::string* _aidl_return) {
    *_aidl_return = healthState_;
    return ::ndk::ScopedAStatus::ok();
}

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
    //   行标签热词按会话即时构建；同音纠错靠"表内读音吸附"，吸附词表是**会话打开时按该表
    //   实时构建**的，所以新导入的表下一次 openSession 自动生效（零重建、零额外工作）。
    std::string key = manager_->ResolveTargetKey(name);
    ALOGI("[IMPORT] 表 %s(key=%s)：%d行x%d列 %d条向量（下次 openSession 即生效）", name.c_str(),
          key.c_str(), summary.rowsCount, summary.colsCount, summary.entries);
    *_aidl_return = kOk;
    return ::ndk::ScopedAStatus::ok();
}

// ---- 语音会话（单会话：一个麦克风 = 一路）----

int VtaServiceImpl::OpenSessionLocked(const std::string& tableName, int silenceMs, int captureMode) {
    // 调用方须持 mu_
    if (captureMode != 0 && captureMode != 1) return kErrInvalid;

    std::string key = manager_->ResolveTargetKey(tableName);
    if (key.empty()) return kErrInvalid;  // 表未注册

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
    // 取该表索引快照（共享所有权）：与向量检索同一份，切表不会让本会话悬空
    std::shared_ptr<const VtxIndex> index = manager_->Activate(key);
    if (index == nullptr) return kErrInvalid;

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

    int id = nextSessionId_++;
    auto session = VoiceSession::CreateAndStart(id, cfg, host_.get(), index, &err);
    if (session == nullptr) {
        ALOGW("[SESSION] 打开会话失败: %s", err.c_str());
        return kErrInternal;
    }
    Active a;
    a.id = id;
    a.tableKey = key;
    a.tableName = tableName;
    a.session = std::move(session);
    active_ = std::move(a);
    host_->OnSessionOpened();

    // captureMode=0：确保采音在跑（权限在原生层解决；失败则要求改用 pushPcm 模式）
    if (captureMode == 0 && !AudioCapture::Running()) {
        std::string capErr;
        bool ok = AudioCapture::Start(
            [](const float* s, int n) { VtaServiceImpl::RouteCapturedAudio(s, n); }, &capErr);
        if (!ok) {
            ALOGW("[CAPTURE] 启动失败: %s（可改用 captureMode=1 pushPcm）", capErr.c_str());
            CloseActiveLocked();
            return kErrInternal;
        }
    }
    return id;
}

void VtaServiceImpl::CloseActiveLocked() {
    // 调用方须持 mu_；Close() 阻塞收尾（flush + 提交），不持 mu_
    if (!active_) return;
    AIBinder* client = active_->client;
    bool linked = active_->linked;
    int id = active_->id;
    auto session = active_->session;
    active_.reset();

    if (session) {
        session->Close();
        // 排空收尾 flush 产生的最后一批 cells，转入 drained 缓冲供下一次 pollCells 取走
        auto rest = session->PollCells(1 << 20, 0);
        if (!rest.empty()) drainedCells_[id] = std::move(rest);
    }
    if (linked && client != nullptr) AIBinder_unlinkToDeath(client, GetDeathRecipient(), client);
    StopCaptureIfIdle();
}

void VtaServiceImpl::StopCaptureIfIdle() {
    // 调用方须持 mu_：无 captureMode=0 的活动会话就停采音（省电）
    if (!active_ || active_->session == nullptr || active_->session->CaptureMode() != 0)
        AudioCapture::Stop();
}

::ndk::ScopedAStatus VtaServiceImpl::openSession(const std::string& tableName, int32_t silenceMs,
                                                 int32_t captureMode,
                                                 const ::ndk::SpAIBinder& client,
                                                 int32_t* _aidl_return) {
    int sid = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        sid = OpenSessionLocked(tableName, silenceMs, captureMode);
        if (sid <= 0) { *_aidl_return = sid; return ::ndk::ScopedAStatus::ok(); }
        // AIDL 路线：登记 client 死亡通知（进程内路线 client 为空，跳过）
        if (client.get() != nullptr) {
            AIBinder* raw = client.get();
            if (AIBinder_linkToDeath(raw, GetDeathRecipient(), raw) == STATUS_OK) {
                active_->client = raw;
                active_->linked = true;
            }
        }
    }
    *_aidl_return = sid;
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus VtaServiceImpl::pushPcm(int32_t sessionId, const aidl::PcmFrame& frame,
                                             int32_t* _aidl_return) {
    std::shared_ptr<VoiceSession> session;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!active_ || active_->id != sessionId) {
            *_aidl_return = kErrInvalid;
            return ::ndk::ScopedAStatus::ok();
        }
        session = active_->session;
    }
    if (session->CaptureMode() != 1) { *_aidl_return = kErrInvalid; return ::ndk::ScopedAStatus::ok(); }
    // 单块上限 4096 样本（256ms）：高频小块比低频大块更省传输（方案 §9.1）
    size_t n = frame.samples.size();
    if (n == 0) { *_aidl_return = kOk; return ::ndk::ScopedAStatus::ok(); }  // 空块不崩
    if (n > 4096) { *_aidl_return = kErrInvalid; return ::ndk::ScopedAStatus::ok(); }
    session->PushPcm(frame.samples.data(), n);
    *_aidl_return = kOk;
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus VtaServiceImpl::getState(int32_t sessionId,
                                              aidl::SessionState* _aidl_return) {
    std::shared_ptr<VoiceSession> session;
    std::string tableName;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (active_ && active_->id == sessionId) {
            session = active_->session;
            tableName = active_->tableName;
        }
    }
    _aidl_return->sessionId = sessionId;
    if (!session) {
        _aidl_return->phase = "closed";
        _aidl_return->lastPartialMs = 0;
        return ::ndk::ScopedAStatus::ok();
    }
    auto st = session->GetState();
    _aidl_return->tableName = tableName;
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
        if (active_ && active_->id == sessionId) {
            // PollCells 最多等 250ms（阻塞拉取但不拖死调用方）
            hits = active_->session->PollCells(maxN > 0 ? maxN : 16, 250);
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
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!active_ || active_->id != sessionId) {
            // 已关闭（或未知）：drained 里还有尾巴就留着等下一次 pollCells
            *_aidl_return = drainedCells_.count(sessionId) ? kOk : kErrInvalid;
            return ::ndk::ScopedAStatus::ok();
        }
        CloseActiveLocked();
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

// ---- 死亡回收 / 采音路由 ----

void VtaServiceImpl::OnBinderDied(void* cookie) {
    std::shared_ptr<VoiceSession> session;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!active_ || active_->client != cookie) return;  // 不是活动会话的 client
        session = active_->session;
        active_.reset();
        StopCaptureIfIdle();
    }
    if (session) session->Close();  // 阻塞收尾（死亡回调线程内，与原实现一致）
}

void VtaServiceImpl::RouteCapturedAudio(const float* samples, int n) {
    if (g_instance == nullptr) return;
    std::shared_ptr<VoiceSession> target;
    {
        std::lock_guard<std::mutex> lk(g_instance->mu_);
        // 单会话：仅路由给 captureMode=0 的活动会话
        if (g_instance->active_ && g_instance->active_->session != nullptr &&
            g_instance->active_->session->CaptureMode() == 0)
            target = g_instance->active_->session;
    }
    if (target) target->OnCapturedAudio(samples, n);
}

}  // namespace vta
