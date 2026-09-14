// jni/service/binder_rt.h —— servicemanager/binder-process API 运行期解析（dlopen）。
// NDK 不随附 android/binder_manager.h、android/binder_process.h（platform-only API），
// 但系统 libbinder_ndk.so（API 30+）实际导出这些符号；selftest 路径不依赖本模块。
#pragma once

#include <android/binder_ibinder.h>
#include <android/binder_status.h>

#include <dlfcn.h>

namespace vta::binderrt {

// 解析失败返回 nullptr 的函数指针；调用方判空。
inline binder_status_t (*ServiceManagerAddService())(AIBinder*, const char*) {
    static auto fn = reinterpret_cast<binder_status_t (*)(AIBinder*, const char*)>(
        dlsym(RTLD_DEFAULT, "AServiceManager_addService"));
    return fn;
}

using SetThreadPoolMaxThreadCountFn = void (*)(size_t);
inline SetThreadPoolMaxThreadCountFn GetSetThreadPoolMaxThreadCount() {
    static auto fn = reinterpret_cast<SetThreadPoolMaxThreadCountFn>(
        dlsym(RTLD_DEFAULT, "ABinderProcess_setThreadPoolMaxThreadCount"));
    return fn;
}

using JoinThreadPoolFn = void (*)();
inline JoinThreadPoolFn GetJoinThreadPool() {
    static auto fn = reinterpret_cast<JoinThreadPoolFn>(
        dlsym(RTLD_DEFAULT, "ABinderProcess_joinThreadPool"));
    return fn;
}

}  // namespace vta::binderrt
