// jni/common/mutex_compat.h —— MinGW(win32 线程模型) 主机测试的 std::mutex 兜底。
// Android NDK / Linux 上正常走 <mutex>；此兜底仅供 PC 端 vta-host-test 编译通过，
// 不承载真实多线程语义（主机测试为单线程）。
#pragma once

#if defined(_WIN32) && defined(__MINGW32__) && !defined(__CYGWIN__)

#include <windows.h>

namespace std {

class mutex {
public:
    mutex() { InitializeCriticalSection(&cs_); }
    ~mutex() { DeleteCriticalSection(&cs_); }
    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;
    void lock() { EnterCriticalSection(&cs_); }
    void unlock() { LeaveCriticalSection(&cs_); }

private:
    CRITICAL_SECTION cs_;
};

template <class Mtx>
class lock_guard {
public:
    explicit lock_guard(Mtx& m) : m_(m) { m_.lock(); }
    ~lock_guard() { m_.unlock(); }
    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;

private:
    Mtx& m_;
};

}  // namespace std

#else

#include <mutex>

#endif
