// jni/service/vta_service_impl.h —— AIDL 服务端实现（IVtaService Bn 端）
#pragma once

#include <android/binder_ibinder.h>
#include <android/binder_status.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "aidl/com/vta/BnVtaService.h"
#include "aidl/com/vta/CellHit.h"
#include "aidl/com/vta/PcmFrame.h"
#include "aidl/com/vta/SessionState.h"
#include "aidl/com/vta/TableInfo.h"

#include "../session/voice_session.h"
#include "engine_host.h"
#include "vta_config.h"

namespace ndk {
class SpAIBinder;
}

namespace vta {

class TableVectorManager;
class HomophoneReplacer;
struct ClientKey;

// 返回码（与 IVtaService.aidl 注释一致）
constexpr int kOk = 0;
constexpr int kErrConflict = 409;
constexpr int kErrInvalid = -2;
constexpr int kErrNotReady = -3;
constexpr int kErrInternal = -4;

class VtaServiceImpl : public aidl::com::vta::BnVtaService {
public:
    explicit VtaServiceImpl(const VtaConfig& config);
    ~VtaServiceImpl() override;

    // 初始化路径/注册表（构造后调用一次；失败返回 false + err）
    bool Init(std::string* err);

    // 表结构描述（测试页渲染表格用）：首行 "name|rows|cols"，随后每行一个行标签；
    // 表不存在返回空串。会顺带激活该表（测试页/会话共用同一活动表）。
    std::string DescribeTable(const std::string& tableName);

    // AIDL 方法
    ::ndk::ScopedAStatus getVersion(int32_t* _aidl_return) override;
    ::ndk::ScopedAStatus health(std::string* _aidl_return) override;
    ::ndk::ScopedAStatus listTables(std::vector<aidl::com::vta::TableInfo>* _aidl_return) override;
    ::ndk::ScopedAStatus getTable(const std::string& name,
                                  std::optional<aidl::com::vta::TableInfo>* _aidl_return) override;
    ::ndk::ScopedAStatus importTable(const std::string& name,
                                     const std::vector<std::string>& rowLabels, int32_t columnCount,
                                     int32_t* _aidl_return) override;
    ::ndk::ScopedAStatus openSession(const std::string& tableName, int32_t silenceMs,
                                     int32_t captureMode, const ::ndk::SpAIBinder& client,
                                     int32_t* _aidl_return) override;
    ::ndk::ScopedAStatus pushPcm(int32_t sessionId, const aidl::com::vta::PcmFrame& frame,
                                 int32_t* _aidl_return) override;
    ::ndk::ScopedAStatus getState(int32_t sessionId,
                                  aidl::com::vta::SessionState* _aidl_return) override;
    ::ndk::ScopedAStatus pollCells(int32_t sessionId, int32_t maxN,
                                   std::vector<aidl::com::vta::CellHit>* _aidl_return) override;
    ::ndk::ScopedAStatus closeSession(int32_t sessionId, int32_t* _aidl_return) override;
    ::ndk::ScopedAStatus setDenoise(bool enabled, int32_t* _aidl_return) override;

    // binder 死亡回调：该 client 的会话全部自动 close，资源回收（方案 §8.4）
    void OnBinderDied(void* cookie);

    // AAudio 采音回调路由（captureMode=0 的会话）
    static void RouteCapturedAudio(const float* samples, int n);

private:
    struct SessionRecord {
        int id = 0;
        ClientKey* client = nullptr;
        std::string tableKey;  // 本会话绑定的表 key（并发门卫：同 client 同表只一路）
        std::unique_ptr<VoiceSession> session;
        std::unique_ptr<HomophoneReplacer> replacer;  // 该表 HR 规则（随会话捕获）
    };
    struct ClientRecord {
        std::vector<int> sessionIds;
    };

    std::string LoadTableReplacerRules(const std::string& tableKey) const;
    int OpenSessionLocked(const std::string& tableName, int silenceMs, int captureMode,
                          ClientKey* clientKey);
    void CloseSessionInternal(int sessionId);  // 须持 mu_
    void CloseSessionsOf(void* clientCookie);

    VtaConfig config_;
    std::unique_ptr<EngineHost> host_;
    std::unique_ptr<TableVectorManager> manager_;
    std::unique_ptr<HomophoneReplacer> pinyinReplacer_;  // 仅拼音表（生成规则用）

    std::mutex mu_;
    std::map<int, std::shared_ptr<SessionRecord>> sessions_;
    std::map<ClientKey*, ClientRecord> clients_;
    // 已关闭会话的剩余 cells：closeSession 触发 flush 后会话即销毁，
    // 但客户端还需一次 pollCells 拿到最后一批（对齐现网 stop→final+cells 时序）。
    std::map<int, std::vector<CellHitInternal>> drainedCells_;
    int nextSessionId_ = 1;
    std::atomic<bool> denoiseEnabled_{false};
    std::string healthState_ = "loading:starting";
};

}  // namespace vta
