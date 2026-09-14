// jni/service/vta_c_api.h —— VTA 纯 C ABI（供 .NET Android DllImport 直接 P/Invoke）
//
// 设计约束：主应用团队只有 .NET8 + npm，无 Android 工具链——因此对外只暴露扁平 C 函数，
// 无 Java、无 AIDL。字符串一律 UTF-8，调用方提供缓冲区（不足截断并返回所需长度）。
//
// 语义：**单会话**——一个物理麦克风 = 同一时刻至多一路识别。
//   vta_open 若已有活动会话，先自动关闭（含静默提交）再开新的；换表即换会话。
//   cells 的 row/column 为 0-indexed（与 H5 表格第 0 行第 0 列直接对应）。
//
// 生命周期：vta_start → vta_import/vta_open → vta_state|vta_cells|vta_push_pcm → vta_close。
// 线程安全：所有函数可在任意线程调用（内部加锁）；vta_push_pcm 与轮询并发安全。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VTA_API __attribute__((visibility("default")))

// 返回码（与错误码表一致；字符串函数另见各函数说明）
#define VTA_OK 0
#define VTA_ERR_INVALID (-2)   /* 参数非法/表未导入/会话未开 */
#define VTA_ERR_NOT_READY (-3) /* 模型加载中/加载失败（先 vta_health 观察） */
#define VTA_ERR_INTERNAL (-4)  /* IO/推理异常 */

// 初始化：dataDir = 模型与注册表根目录（主应用把模型资产解到该目录后传入）。幂等。
VTA_API int vta_start(const char* data_dir);

// 协议版本（当前 1）
VTA_API int vta_version(void);

// 健康态：写 "ok" / "loading:..." / "error:..." / "not_started"。返回 0；cap 不足返回所需长度。
VTA_API int vta_health(char* out, int cap);

// 已导入表清单：JSON 数组 [{"name","key","rows","columns","updatedAtMs"},...]
VTA_API int vta_list_tables(char* out, int cap);

// 导入表：rows_text = 行标签，每行一个（'\n' 分隔）；同步构建索引并落盘；幂等（同名覆盖）。
VTA_API int vta_import(const char* name, const char* rows_text, int column_count);

// 打开会话（单会话；已有活动会话先自动关闭）。返回 sessionId（>0）或错误码。
VTA_API int vta_open(const char* table, int silence_ms, int capture_mode);

// 喂流（capture_mode=1 时）：16kHz float32 单声道原始样本，n<=4096。
VTA_API int vta_push_pcm(int sid, const float* samples, int n);

// 会话状态：JSON {"sessionId","tableName","phase","partial","lastPartialMs"}
VTA_API int vta_state(int sid, char* out, int cap);

// 拉取解析结果：JSON 数组 [{"row","column","value","raw","finalizedAtMs"},...]（0-indexed）。
// 有货即返；无货服务端最多挂 250ms（调用线程不可为主 UI 线程）。
VTA_API int vta_cells(int sid, int max_n, char* out, int cap);

// 关闭会话：flush + fold partial + 提交剩余 cells（关闭后的最后一批 cells 仍可取一次）。
VTA_API int vta_close(int sid);

// 降噪运行时开关（对新会话生效）
VTA_API int vta_set_denoise(int on);

// 释放全部会话与引擎（进程退出前可选调用）
VTA_API void vta_shutdown(void);

#ifdef __cplusplus
}  // extern "C"
#endif
