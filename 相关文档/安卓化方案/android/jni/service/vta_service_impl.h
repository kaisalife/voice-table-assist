// jni/service/vta_service_impl.h —— 语音表格服务实现（IVtaService Bn 端 + 进程内单会话）
#pragma once

#include <android/binder_ibinder.h>
#include <android/binder_status.h>

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

// 返回码（与 IVtaService.aidl 注释一致）
constexpr int kOk = 0;
constexpr int kErrInvalid = -2;
constexpr int kErrNotReady = -3;
constexpr int kErrInternal = -4;

// 单会话模型：一个物理麦克风 = 同一时刻至多一路识别。
// openSession 若已有活动会话，先自动关闭（含静默提交）再开新的——幂等，换表即换会话。
// 绑定当前活动表的索引快照与热词（与向量检索同一份 VtxIndex）。
class VtaServiceImpl : public aidl::com::vta::BnVtaService {
public:
    explicit VtaServiceImpl(const VtaConfig& config);
    ~VtaServiceImpl() override;

    // 初始化路径/注册表（构造后调用一次；失败返回 false + err）
    bool Init(std::string* err);

    // 表结构描述（测试页渲染表格用）：首行 "name|rows|cols"，随后每行一个行标签；
    // 表不存在返回空串。会顺带激活该表（测试页/会话共用同一活动表）。
    std::string DescribeTable(const std::string& tableName);

    // AIDL 方法（协议 v1 保留 sessionId 形参；语义 = 唯一活动会话）
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

    // binder 死亡回调：活动会话的 client 消失 → 自动 close（AIDL 路线；进程内路线无 client）
    void OnBinderDied(void* cookie);

    // AAudio 采音回调路由（captureMode=0 的活动会话）
    static void RouteCapturedAudio(const float* samples, int n);

private:
    int OpenSessionLocked(const std::string& tableName, int silenceMs, int captureMode);
    void CloseActiveLocked();  // 须持 mu_：关闭当前会话，flush 产生的剩余 cells 移入 drainedCells_
    void StopCaptureIfIdle();  // 须持 mu_：无 captureMode=0 会话时停采音（省电）

    VtaConfig config_;
    std::unique_ptr<EngineHost> host_;
    std::unique_ptr<TableVectorManager> manager_;

    mutable std::mutex mu_;
    std::string healthState_ = "loading:starting";
    bool denoiseEnabled_ = false;

    // 唯一活动会话
    struct Active {
        int id = 0;
        std::string tableKey;
        std::string tableName;
        std::shared_ptr<VoiceSession> session;
        AIBinder* client = nullptr;  // AIDL 路线的死亡通知对象（进程内路线为 null）
        bool linked = false;
    };
    std::optional<Active> active_;
    // 已关闭会话的剩余 cells：closeSession 触发 flush 后会话即销毁，
    // 但客户端还需一次 pollCells 拿到最后一批（对齐现网 stop→final+cells 时序）。
    std::map<int, std::vector<CellHitInternal>> drainedCells_;
    int nextSessionId_ = 1;

    static VtaServiceImpl* g_instance;
};

}  // namespace vta
