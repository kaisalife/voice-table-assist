// jni/service/binder_manager_shim.h —— NDK r27c sysroot 缺 android/binder_manager.h 的最小声明。
// 函数实现由系统 libbinder_ndk.so 提供（链接 -lbinder_ndk）。
#pragma once

#include <android/binder_ibinder.h>
#include <android/binder_status.h>

#ifdef __cplusplus
extern "C" {
#endif

// android/binder_manager.h（NDK r27c sysroot 未随附，签名与 platform 头一致）
__INTRODUCED_IN(29)
binder_status_t AServiceManager_addService(AIBinder* binder, const char* name);
__INTRODUCED_IN(29)
binder_status_t AServiceManager_addServiceWithFlags(AIBinder* binder, const char* name,
                                                    int32_t flags);

// android/binder_process.h（同上）
struct AIBinder_ProcessPool;
__INTRODUCED_IN(29)
void ABinderProcess_setThreadPoolMaxThreadCount(size_t maxThreads);
__INTRODUCED_IN(29)
void ABinderProcess_joinThreadPool();
__INTRODUCED_IN(29)
bool ABinderProcess_startThreadPool(size_t maxThreads);
__INTRODUCED_IN(29)
bool ABinderProcess_isThreadPoolStarted();
__INTRODUCED_IN(29)
void ABinderProcess_setThreadPool(AIBinder_ProcessPool* pool);

#ifdef __cplusplus
}  // extern "C"
#endif
