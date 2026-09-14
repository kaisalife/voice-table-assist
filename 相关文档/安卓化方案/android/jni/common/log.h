// jni/common/log.h —— 统一日志宏（Android logcat；主机构建退化为 stdout）
#pragma once

#if defined(__ANDROID__)
#include <android/log.h>
#define VTA_TAG "VTA"
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, VTA_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, VTA_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, VTA_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, VTA_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define ALOGD(...) do { std::fprintf(stderr, "[D] " __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#define ALOGI(...) do { std::fprintf(stderr, "[I] " __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#define ALOGW(...) do { std::fprintf(stderr, "[W] " __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#define ALOGE(...) do { std::fprintf(stderr, "[E] " __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#endif
