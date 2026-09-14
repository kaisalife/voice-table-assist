// aidl/com/vta/IVtaService.aidl —— VTA Native Service 对企业 App 的唯一入口（Binder/AIDL）。
// 对应《安卓化方案.md》§3.1/§6.2。Java/Kotlin 桩由 AIDL 编译器自动生成（ndk backend 供服务端 C++ 使用）。
//
// 返回值约定（int 型返回）：
//   0    = 成功
//   409  = 冲突（openSession：该 client 已有会话 / 超出并发上限）
//   -2   = 参数非法（如表未注册、采样率错、captureMode 非法、sessionId 不存在）
//   -3   = 服务未就绪（模型加载中/加载失败）
//   -4   = 内部错误（IO/推理异常）
package com.vta;

import com.vta.PcmFrame;
import com.vta.SessionState;
import com.vta.TableInfo;
import com.vta.CellHit;

interface IVtaService {

    // ---- 生命周期 ----
    int getVersion();                       // 协议版本，当前 = 1
    String health();                        // "ok" / "loading:<msg>" / "error:<msg>"

    // ---- 表格管理 ----
    List<TableInfo> listTables();
    @nullable TableInfo getTable(String name);   // 未注册返回 null
    // 导入表（构建向量索引 + 生成热词/HR 规则 + 落盘 VTX1）。方案原稿标【待定】，
    // 端侧无 HTTP 后台，若无此接口表永远进不来，故先落地；集成方如另有方案可裁撤。
    int importTable(String name, in String[] rowLabels, int columnCount);

    // ---- 语音会话（流式）----
    // captureMode: 0 = Service 自主采音（默认，推荐，见方案 §6.1）；1 = 企业 App pushPcm 喂流
    // silenceMs  : final 后静默自动提交计时（<=0 取服务端默认 300ms，与现网 appsettings 一致）
    // client     : 企业 App 的任意 IBinder（通常传自身服务的 binder），用于 linkToDeath——
    //              binder 断开时 Service 自动 close 该 App 的全部会话（方案 §8.4）。
    int openSession(String tableName, int silenceMs, int captureMode, @nullable IBinder client);
    int pushPcm(int sessionId, in PcmFrame frame);   // captureMode=1 时喂 16kHz float32 PCM（每块 10~100ms，<=4096 样本）
    SessionState getState(int sessionId);            // 50~100ms 轮询拿 partial（方案 §3.4）
    CellHit[] pollCells(int sessionId, int maxN);    // 拉取累积的 cells（有货即返；无货最多等 250ms 返回空数组）
    int closeSession(int sessionId);                 // 收尾：flush 降噪/识别，fold partial，提交剩余 cells
    int setDenoise(boolean enabled);                 // GTCRN 运行时开关（方案 §5.2），对新会话生效
}
