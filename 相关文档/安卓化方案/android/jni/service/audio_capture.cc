// jni/service/audio_capture.cc —— AAudio 自主采音（方案 §6.1）
#include "audio_capture.h"

#include <aaudio/AAudio.h>

#include <mutex>

#include "../common/log.h"

namespace vta {

namespace {

struct State {
    AAudioStream* stream = nullptr;
    std::mutex mu;
    std::function<void(const float*, int)> cb;
};

State* g_state = nullptr;
std::mutex g_stateMu;

aaudio_data_callback_result_t DataCallback(AAudioStream* /*stream*/, void* userData,
                                           void* audioData, int32_t numFrames) {
    auto* st = static_cast<State*>(userData);
    std::function<void(const float*, int)> cb;
    {
        std::lock_guard<std::mutex> lk(st->mu);
        cb = st->cb;
    }
    if (cb) cb(static_cast<const float*>(audioData), numFrames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void ErrorCallback(AAudioStream* /*stream*/, void* /*userData*/, aaudio_result_t error) {
    ALOGE("[CAPTURE] AAudio 错误: %s", AAudio_convertResultToText(error));
}
}  // namespace

bool AudioCapture::Start(std::function<void(const float*, int)> dataCallback, std::string* err) {
    std::lock_guard<std::mutex> lk(g_stateMu);
    if (g_state != nullptr && g_state->stream != nullptr) return true;  // 已在采

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t r = AAudio_createStreamBuilder(&builder);
    if (r != AAUDIO_OK) {
        if (err) *err = std::string("AAudio_createStreamBuilder: ") + AAudio_convertResultToText(r);
        return false;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setSampleRate(builder, 16000);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    // VOICE_RECOGNITION 音源：关系统 AGC/降噪，保留原始频谱（配合 GTCRN）
    AAudioStreamBuilder_setInputPreset(builder, AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    // 预创建 State 以便 builder 阶段绑定 userData
    if (g_state == nullptr) g_state = new State();
    g_state->cb = std::move(dataCallback);
    AAudioStreamBuilder_setDataCallback(builder, DataCallback, g_state);
    AAudioStreamBuilder_setErrorCallback(builder, ErrorCallback, nullptr);

    AAudioStream* stream = nullptr;
    r = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (r != AAUDIO_OK) {
        if (err)
            *err = std::string("打开麦克风失败（检查 RECORD_AUDIO 权限）: ") +
                   AAudio_convertResultToText(r);
        return false;
    }

    r = AAudioStream_requestStart(stream);
    if (r != AAUDIO_OK) {
        AAudioStream_close(stream);
        if (err) *err = std::string("启动采音失败: ") + AAudio_convertResultToText(r);
        return false;
    }
    g_state->stream = stream;
    ALOGI("[CAPTURE] AAudio 采音已启动 (16kHz/mono/float, VOICE_RECOGNITION)");
    return true;
}

bool AudioCapture::Running() {
    std::lock_guard<std::mutex> lk(g_stateMu);
    return g_state != nullptr && g_state->stream != nullptr;
}

void AudioCapture::Stop() {
    std::lock_guard<std::mutex> lk(g_stateMu);
    if (!g_state) return;
    std::lock_guard<std::mutex> lk2(g_state->mu);
    if (g_state->stream) {
        AAudioStream_requestStop(g_state->stream);
        AAudioStream_close(g_state->stream);
        g_state->stream = nullptr;
    }
    g_state->cb = nullptr;
    ALOGI("[CAPTURE] AAudio 采音已停止");
}

}  // namespace vta
