// jni/service/main.cc —— VTA Native Service 入口（方案 §1/§7.2）
//
// 用法：
//   vta-service [--data-dir <dir>]            常驻服务：注册 "vta" 到 servicemanager 后 join
//   vta-service --selftest <f32le@16k 文件> [--data-dir <dir>]
//                                             数值自检/回放：走完整管线输出 partial/final/cells
//                                             （对应方案 §7.3 调试、§8.2 首次启动数值自检）
#include "binder_rt.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include "../common/log.h"
#include "../common/strings.h"
#include "../homophone/homophone_replacer.h"
#include "../session/voice_session.h"
#include "../tables/table_vector_manager.h"
#include "engine_host.h"
#include "selftest_runner.h"
#include "vta_config.h"
#include "vta_service_impl.h"

namespace {

using namespace vta;

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

// ---- selftest：单文件回放全管线（实现见 selftest_runner.cc，与 APK 内测试共用）----
int RunSelftest(const std::string& pcmPath, const std::string& dataDir) {
    auto r = RunSelftestOnPcm(pcmPath, dataDir);
    if (!r.ok) {
        ALOGE("[SELFTEST] failed: %s", r.error.c_str());
        return 1;
    }
    ALOGI("[SELFTEST] final: %s", r.finalText.c_str());
    ALOGI("[SELFTEST] cells: %zu", r.cells.size());
    return 0;
}

}  // namespace

extern "C" int RunMmiKernelTest(const char* int8ModelPath, const char* fp32ModelPath);

// ---- 多表切换验证：--switch-test <pcm> <表1> <表2> [表3 ...] ----
// 模型：导入多张表共存；同一时刻只用一张，用到哪张用哪张的数据。
int RunSwitchTest(const std::string& dataDir, const std::vector<std::string>& tables,
                  const std::string& pcmPath) {
    auto r = RunTableSwitchTest(pcmPath, dataDir, tables);
    if (!r.ok) {
        ALOGE("[SWITCH] failed: %s", r.error.c_str());
        return 1;
    }
    int withinOk = 0, rebuilds = 0;
    for (const auto& s : r.steps) {
        ALOGI("[SWITCH] 表=%s(%d行x%d列) 识别器重建=%s final=\"%s\"", s.table.c_str(), s.rows,
              s.cols, s.recognizerRebuilt ? "是" : "否", s.finalText.c_str());
        for (const auto& c : s.cells) ALOGI("[SWITCH]   cell: %s", c.c_str());
        if (s.cellsWithinTable) ++withinOk;
        if (s.recognizerRebuilt) ++rebuilds;
    }
    ALOGI("[SWITCH] 结果：%zu 张表切换，%d 张命中均落在本表范围，识别器重建 %d 次，"
          "会话中途强制重建=%s",
          r.steps.size(), withinOk, rebuilds, r.forcedRebuildMidSession ? "是" : "否");
    return 0;
}

int main(int argc, char** argv) {
    std::string dataDir = "/data/local/tmp/vta";
    std::string selftestPath;
    bool selftest = false;
    bool mmiTest = false;
    bool switchTest = false;
    std::string switchPcm;
    std::vector<std::string> switchTables;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            dataDir = argv[++i];
        } else if (std::strcmp(argv[i], "--selftest") == 0 && i + 1 < argc) {
            selftest = true;
            selftestPath = argv[++i];
        } else if (std::strcmp(argv[i], "--mmi-test") == 0 && i + 2 < argc) {
            mmiTest = true;
            return RunMmiKernelTest(argv[i + 1], argv[i + 2]);
        } else if (std::strcmp(argv[i], "--switch-test") == 0 && i + 2 < argc) {
            switchTest = true;
            switchPcm = argv[++i];
            // 收集连续的、非 "--" 开头的表名（紧随其后的 --data-dir 等仍按正常参数解析）
            while (i + 1 < argc && strncmp(argv[i + 1], "--", 2) != 0)
                switchTables.push_back(argv[++i]);
        }
    }

    if (selftest) return RunSelftest(selftestPath, dataDir);
    if (switchTest) return RunSwitchTest(dataDir, switchTables, switchPcm);

    vta::VtaConfig config = vta::VtaConfig::Load(dataDir);
    // VtaServiceImpl 继承 ndk::SharedRefBase，NDK 禁用 std::make_shared（防双重所有权），
    // 必须用 SharedRefBase::make
    auto service = ndk::SharedRefBase::make<vta::VtaServiceImpl>(config);
    std::string err;
    if (!service->Init(&err)) {
        ALOGE("[VTA] init failed: %s", err.c_str());
        return 1;
    }

#if defined(__ANDROID__)
    // 常驻服务：注册到 servicemanager。servicemanager/binder-process 是 platform-only API
    // （NDK 无头文件），运行期 dlsym 解析；解析失败仅报错退出（selftest 路径不受影响）。
    ALOGI("[VTA] starting native service (dataDir=%s)", dataDir.c_str());
    if (binderrt::GetSetThreadPoolMaxThreadCount())
        binderrt::GetSetThreadPoolMaxThreadCount()(4);
    auto addService = binderrt::ServiceManagerAddService();
    auto joinPool = binderrt::GetJoinThreadPool();
    if (addService == nullptr || joinPool == nullptr) {
        ALOGE("[VTA] AServiceManager_addService 不可用（需 API 30+ 且 SELinux 允许）");
        return 1;
    }
    ::ndk::SpAIBinder binder = service->asBinder();
    binder_status_t st = addService(binder.get(), "vta");
    if (st != STATUS_OK) {
        ALOGE("[VTA] addService failed: %d", st);
        return 1;
    }
    ALOGI("[VTA] service registered as \"vta\", joining binder pool");
    joinPool();
#else
    ALOGI("[VTA] host build: service loop not available (use --selftest)");
    while (true) std::this_thread::sleep_for(std::chrono::seconds(60));
#endif
    return 0;
}
