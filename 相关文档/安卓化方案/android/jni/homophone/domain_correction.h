// jni/homophone/domain_correction.h
#pragma once

#include <string>

namespace vta {

// 通用后处理：把数字上下文里的数字同音字归一（实/石/时→十，灵→零，付→负，寺→四…）。
// 与表无关（不依赖任何表内容）；表相关的同音纠正由 SoundAligner 的表内读音吸附负责。
std::string DomainCorrect(const std::string& text);

}  // namespace vta
