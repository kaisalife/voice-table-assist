// jni/embed/vtx1.cc —— VTX1 读写（C# BinaryReader/Writer 二进制兼容）
#include "vtx1.h"

#include <cstring>
#include <fstream>

#include "../common/log.h"
#include "../common/strings.h"

namespace vta {

namespace {

constexpr uint32_t kMagic = 0x31585456;  // "VTX1"（小端: 56 54 58 31）

// C# BinaryWriter.Write(string)：7-bit LEB128 长度前缀 + UTF8 字节
void Write7BitLength(std::string* out, int32_t len) {
    uint32_t v = static_cast<uint32_t>(len);
    while (v >= 0x80) {
        out->push_back(static_cast<char>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out->push_back(static_cast<char>(v));
}

void WriteString(std::string* out, const std::string& s) {
    Write7BitLength(out, static_cast<int32_t>(s.size()));
    out->append(s);
}

void WriteI32(std::string* out, int32_t v) {
    char buf[4];
    std::memcpy(buf, &v, 4);
    out->append(buf, 4);
}

void WriteU32(std::string* out, uint32_t v) { WriteI32(out, static_cast<int32_t>(v)); }

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}

    bool ReadU32(uint32_t* v) {
        if (end_ - p_ < 4) return false;
        std::memcpy(v, p_, 4);
        p_ += 4;
        return true;
    }
    bool ReadI32(int32_t* v) { return ReadU32(reinterpret_cast<uint32_t*>(v)); }
    bool ReadF32(float* v) { return ReadU32(reinterpret_cast<uint32_t*>(v)); }

    bool ReadString(std::string* s) {
        // 7-bit LEB128 长度
        uint32_t len = 0;
        int shift = 0;
        while (true) {
            if (p_ >= end_) return false;
            uint8_t b = *p_++;
            len |= static_cast<uint32_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
            if (shift > 28) return false;
        }
        if (static_cast<size_t>(end_ - p_) < len) return false;
        s->assign(reinterpret_cast<const char*>(p_), len);
        p_ += len;
        return true;
    }

    bool ReadBytes(void* dst, size_t n) {
        if (static_cast<size_t>(end_ - p_) < n) return false;
        std::memcpy(dst, p_, n);
        p_ += n;
        return true;
    }

private:
    const uint8_t* p_;
    const uint8_t* end_;
};

}  // namespace

bool Vtx1Load(const std::string& path, VtxIndex* out, std::string* err) {
    std::string blob;
    if (!ReadFileBytes(path, &blob)) {
        if (err) *err = "cannot read: " + path;
        return false;
    }
    Reader r(reinterpret_cast<const uint8_t*>(blob.data()), blob.size());
    uint32_t magic = 0;
    if (!r.ReadU32(&magic) || magic != kMagic) {
        if (err) *err = "向量库文件头不合法: " + path + "（期望 VTX1 二进制格式）";
        return false;
    }
    VtxIndex idx;
    int32_t dim = 0, rowsCount = 0, colsCount = 0, labelCount = 0;
    if (!r.ReadI32(&dim) || !r.ReadI32(&rowsCount) || !r.ReadI32(&colsCount) ||
        !r.ReadI32(&labelCount)) {
        if (err) *err = "VTX1 header truncated: " + path;
        return false;
    }
    idx.dim = dim;
    idx.rowsCount = rowsCount;
    idx.colsCount = colsCount;
    idx.rows.resize(labelCount);
    for (int i = 0; i < labelCount; ++i) {
        if (!r.ReadString(&idx.rows[i])) {
            if (err) *err = "VTX1 row labels truncated";
            return false;
        }
    }
    int32_t entryCount = 0;
    if (!r.ReadI32(&entryCount)) {
        if (err) *err = "VTX1 entry count truncated";
        return false;
    }
    idx.entries.resize(entryCount);
    for (int32_t i = 0; i < entryCount; ++i) {
        auto& c = idx.entries[i];
        if (!r.ReadI32(&c.row) || !r.ReadI32(&c.col) || !r.ReadString(&c.phrase)) {
            if (err) *err = "VTX1 entry truncated: " + path;
            return false;
        }
        c.vec.resize(dim > 0 ? dim : 0);
        if (dim > 0 && !r.ReadBytes(c.vec.data(), static_cast<size_t>(dim) * sizeof(float))) {
            if (err) *err = "向量库条目不足: " + path + " entry#" + std::to_string(i);
            return false;
        }
    }
    *out = std::move(idx);
    return true;
}

bool Vtx1Save(const std::string& path, const VtxIndex& index, std::string* err) {
    std::string blob;
    blob.reserve(64 + index.entries.size() * (index.dim * 4 + 16));
    WriteU32(&blob, kMagic);
    WriteI32(&blob, index.dim);
    WriteI32(&blob, index.rowsCount);
    WriteI32(&blob, index.colsCount);
    WriteI32(&blob, static_cast<int32_t>(index.rows.size()));
    for (const auto& label : index.rows) WriteString(&blob, label);
    WriteI32(&blob, static_cast<int32_t>(index.entries.size()));
    for (const auto& e : index.entries) {
        WriteI32(&blob, e.row);
        WriteI32(&blob, e.col);
        WriteString(&blob, e.phrase);
        blob.append(reinterpret_cast<const char*>(e.vec.data()),
                    static_cast<size_t>(index.dim) * sizeof(float));
    }
    if (!WriteFileAtomic(path, blob.data(), blob.size())) {
        if (err) *err = "cannot write: " + path;
        return false;
    }
    return true;
}

}  // namespace vta
