// jni/text/merge_text.h —— 流式文本合并（翻译自 VoiceInteractionSession.MergeText）
#pragma once

#include <string>

namespace vta {

// 与验证页一致的流式文本合并（前缀扩展 / 包含去重 / 重叠拼接）。
std::string MergeStreamingText(const std::string& prev, const std::string& cur);

}  // namespace vta
