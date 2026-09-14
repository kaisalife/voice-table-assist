// jni/service/vta_c_api.cc —— C ABI 薄封装：把 VtaServiceImpl（单会话）映射为扁平 C 函数。
// 字符串一律 UTF-8；调用方提供缓冲区，内容不足截断并返回所需长度（含结尾 0）。
#include "vta_c_api.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "../common/log.h"
#include "aidl/com/vta/CellHit.h"
#include "aidl/com/vta/TableInfo.h"
#include "vta_service_impl.h"

namespace {

std::mutex g_mu;  // 保护 g_service / 生命周期
std::shared_ptr<vta::VtaServiceImpl> g_service;  // SharedRefBase 所有权
bool g_started = false;

vta::VtaServiceImpl* Svc() { return g_service.get(); }

// 把 src 写进 out/cap：返回 0（成功）或所需长度（cap 不足，同时写入截断值）
int CopyOut(const std::string& src, char* out, int cap) {
    const int need = static_cast<int>(src.size()) + 1;
    if (out != nullptr && cap > 0) {
        const int n = cap - 1 < static_cast<int>(src.size()) ? cap - 1 : static_cast<int>(src.size());
        std::memcpy(out, src.data(), static_cast<size_t>(n));
        out[n] = '\0';
    }
    return need > cap ? need : 0;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

}  // namespace

extern "C" {

VTA_API int vta_start(const char* data_dir) {
    if (data_dir == nullptr) return vta::kErrInvalid;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_started) return vta::kOk;
    try {
        vta::VtaConfig config = vta::VtaConfig::Load(data_dir);
        config.lazyLoad = false;
        // VtaServiceImpl 继承 ndk::SharedRefBase：NDK 禁用 std::make_unique，必须 SharedRefBase::make
        auto impl = ndk::SharedRefBase::make<vta::VtaServiceImpl>(config);
        std::string err;
        if (!impl->Init(&err)) {
            ALOGE("[VTA-C] init failed: %s", err.c_str());
            return vta::kErrInternal;
        }
        g_service = std::move(impl);
        g_started = true;
        ALOGI("[VTA-C] started, dataDir=%s", data_dir);
        return vta::kOk;
    } catch (const std::exception& ex) {
        ALOGE("[VTA-C] start exception: %s", ex.what());
        return vta::kErrInternal;
    }
}

VTA_API int vta_version(void) { return 1; }

VTA_API int vta_health(char* out, int cap) {
    if (!Svc()) return CopyOut("not_started", out, cap);
    std::string h;
    (void)Svc()->health(&h);
    return CopyOut(h, out, cap);
}

VTA_API int vta_list_tables(char* out, int cap) {
    if (!Svc()) return CopyOut("[]", out, cap);
    std::vector<aidl::com::vta::TableInfo> tables;
    (void)Svc()->listTables(&tables);
    std::string json = "[";
    bool first = true;
    for (const auto& t : tables) {
        if (!first) json += ",";
        first = false;
        json += "{\"name\":\"" + JsonEscape(t.name) + "\",\"key\":\"" + JsonEscape(t.key) +
                "\",\"rows\":" + std::to_string(t.rows) + ",\"columns\":" + std::to_string(t.columns) +
                ",\"updatedAtMs\":" + std::to_string(t.updatedAtMs) + "}";
    }
    json += "]";
    return CopyOut(json, out, cap);
}

VTA_API int vta_import(const char* name, const char* rows_text, int column_count) {
    if (!Svc()) return vta::kErrNotReady;
    if (name == nullptr || rows_text == nullptr) return vta::kErrInvalid;
    std::vector<std::string> rows;
    {
        std::string s(rows_text);
        size_t start = 0;
        while (start <= s.size()) {
            size_t nl = s.find('\n', start);
            if (nl == std::string::npos) {
                if (!s.substr(start).empty()) rows.push_back(s.substr(start));
                break;
            }
            std::string line = s.substr(start, nl - start);
            if (!line.empty()) rows.push_back(line);
            start = nl + 1;
        }
    }
    int rc = 0;
    (void)Svc()->importTable(name, rows, column_count, &rc);
    return rc;
}

VTA_API int vta_open(const char* table, int silence_ms, int capture_mode) {
    if (!Svc()) return vta::kErrNotReady;
    ::ndk::SpAIBinder nullClient;  // 进程内调用：无 client binder，死亡通知不适用
    int sid = 0;
    (void)Svc()->openSession(table != nullptr ? table : "", silence_ms, capture_mode, nullClient,
                             &sid);
    return sid;
}

VTA_API int vta_push_pcm(int sid, const float* samples, int n) {
    if (!Svc()) return vta::kErrNotReady;
    if (samples == nullptr || n < 0) return vta::kErrInvalid;
    aidl::com::vta::PcmFrame frame;
    frame.timestampMs = 0;
    frame.samples.assign(samples, samples + n);
    int rc = 0;
    (void)Svc()->pushPcm(sid, frame, &rc);
    return rc;
}

VTA_API int vta_state(int sid, char* out, int cap) {
    if (!Svc()) return CopyOut("{\"phase\":\"not_started\"}", out, cap);
    aidl::com::vta::SessionState st{};
    (void)Svc()->getState(sid, &st);
    std::string json = "{\"sessionId\":" + std::to_string(st.sessionId) + ",\"tableName\":\"" +
                       JsonEscape(st.tableName) + "\",\"phase\":\"" + JsonEscape(st.phase) +
                       "\",\"partial\":\"" + JsonEscape(st.partial) + "\",\"lastPartialMs\":" +
                       std::to_string(st.lastPartialMs) + "}";
    return CopyOut(json, out, cap);
}

VTA_API int vta_cells(int sid, int max_n, char* out, int cap) {
    if (!Svc()) return CopyOut("[]", out, cap);
    std::vector<aidl::com::vta::CellHit> cells;
    (void)Svc()->pollCells(sid, max_n, &cells);
    std::string json = "[";
    bool first = true;
    for (const auto& c : cells) {
        if (!first) json += ",";
        first = false;
        json += "{\"row\":" + std::to_string(c.row) + ",\"column\":" + std::to_string(c.column) +
                ",\"value\":\"" + JsonEscape(c.value) + "\",\"raw\":\"" + JsonEscape(c.raw) +
                "\",\"finalizedAtMs\":" + std::to_string(c.finalizedAtMs) + "}";
    }
    json += "]";
    return CopyOut(json, out, cap);
}

VTA_API int vta_close(int sid) {
    if (!Svc()) return vta::kErrNotReady;
    int rc = 0;
    (void)Svc()->closeSession(sid, &rc);
    return rc;
}

VTA_API int vta_set_denoise(int on) {
    if (!Svc()) return vta::kErrNotReady;
    int rc = 0;
    (void)Svc()->setDenoise(on != 0, &rc);
    return rc;
}

VTA_API void vta_shutdown(void) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_service.reset();
    g_started = false;
}

}  // extern "C"
