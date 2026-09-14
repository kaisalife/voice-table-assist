// jni/embed/vtx1.h —— 向量库二进制格式 VTX1（翻译自 Services/VectorIndex.cs）
//
// 布局（小端，全部 int 为 4 字节；字符串 = C# BinaryWriter 格式：7-bit LEB128 长度 + UTF8 字节）：
//   [魔数 "VTX1" 4B][dim][rowsCount][colsCount]
//   [rowsLabelCount] 每行标签: [len][UTF8 bytes]...
//   [entryCount]     每条目:   [row][col][len][UTF8 phrase][vec[dim] float32]
//
// 与 C# 端 round-trip 二进制一致（方案 §8.1 验收线）：C++ 写的 .bin C# 能读、反之亦然。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vta {

struct VtxCell {
    int row = 0;
    int col = 0;
    std::string phrase;
    std::vector<float> vec;
};

struct VtxIndex {
    std::vector<VtxCell> entries;
    std::vector<std::string> rows;  // 行标签
    int rowsCount = 0;
    int colsCount = 0;
    int dim = 0;
};

// 加载失败返回 false（文件头不合法/条目不足）。
bool Vtx1Load(const std::string& path, VtxIndex* out, std::string* err = nullptr);
// 原子写（tmp → rename），等价 C# File.Move(overwrite)。
bool Vtx1Save(const std::string& path, const VtxIndex& index, std::string* err = nullptr);

// 内积（与现 VectorIndex.cs 一致；向量均已归一化，内积即余弦）
inline float Vtx1Dot(const float* a, const float* b, int dim) {
    double d = 0;
    for (int i = 0; i < dim; ++i) d += static_cast<double>(a[i]) * b[i];
    return static_cast<float>(d);
}

}  // namespace vta
