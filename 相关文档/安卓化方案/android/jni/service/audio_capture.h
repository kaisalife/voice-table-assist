// jni/service/audio_capture.h —— Service 自主采音（方案 §6.1）
// AAudio 输入流：VOICE_RECOGNITION 音源 / 16kHz / mono / float32（关系统 AGC/降噪，配合 GTCRN）。
#pragma once

#include <functional>

namespace vta {

class AudioCapture {
public:
    // dataCallback(samples, n)：采音线程回调（float32 [-1,1]，16kHz mono）。
    // 返回 false = 启动失败（无 RECORD_AUDIO 权限/无麦克风），err 带原因。
    static bool Start(std::function<void(const float*, int)> dataCallback, std::string* err);
    static void Stop();
    static bool Running();
};

}  // namespace vta
