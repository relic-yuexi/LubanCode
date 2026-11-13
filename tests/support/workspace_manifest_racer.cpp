// SV-11(workspace manifest 读改写串行化)的跨进程靶子:真起子进程按生产
// 同一份机械占在 <workspace_dir>/.manifest.lock 上,或按生产同一条
// OpenOrRegisterWorkspace 开房——不拿同进程顺序单测冒充双进程互斥(照
// memory_lock_racer 的册写法)。
//
// 用法:
//   workspace_manifest_racer hold <workspace_dir> <ready_file> [release_file]
//       Acquire 磨档占锁 → ready 写 "ok"/"fail: ..." → 轮询 release_file 出现
//       (20ms 一拍,120s 兜底)→ 退出(析构按 owner 核账后放锁)。
//   workspace_manifest_racer crash <workspace_dir> <ready_file>
//       Acquire 磨档占锁 → ready 写 "ok"/"fail: ..." → std::_Exit(9):不跑析
//       构、不放锁,owner 账与锁目录原样留在盘上——模拟开房进程暴毙
//       (句柄由内核回收,持有者身份已死)。
//   workspace_manifest_racer register <workspaces_root> <project_root> <now_ms>
//                                    <ready_file> <go_file>
//       解析身份 → ready 写 "ok"/"fail: ..." → 轮询 go_file 出现(测试端
//       的发令枪,两边同一拍起跑)→ OpenOrRegisterWorkspace 走生产同一条
//       读改写事务路。退出码 0 = 开房成功;5 = 开房失败(fail 文本在
//       ready_file 里)。
// 占锁用 Acquire 磨档(60×100ms,与生产开房路同配),不单发 TryAcquire:
// 单发撞"锁正在建立"在建窗(2s)保守拒即刻死——#209 后实锤的释放残壳竞
// 态(2026-09-23 main run 35814135429 windows 腿),生产 Acquire 磨 6s 穿
// 透 2s 在建窗,夹具照同路占。
// 退出码:0 = 正常收场;2 = 用法错;3 = 没取到锁/身份解析失败;4 = 等不到
// 放行令;5 = 开房失败。栅栏协议:ready 文件是"就绪"屏障,release/go 文件
// 是"放行"屏障——交错由文件控制,不靠两头掐表。
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "workspace/identity.hpp"
#include "workspace/manifest.hpp"
#include "workspace/manifest_lock.hpp"

namespace {

namespace fs = std::filesystem;

template<class MakeRecord>
void Evidence(MakeRecord make_record) noexcept {
    try {
        const std::string line = make_record().dump(-1, ' ', true,
            nlohmann::json::error_handler_t::replace);
        std::fprintf(stderr, "[workspace-racer-evidence] %s\n", line.c_str());
        std::fflush(stderr); // diagnostic stream only; never flush/retry the ready file
    } catch (...) {
        std::fputs("[workspace-racer-evidence-unavailable]\n", stderr);
        std::fflush(stderr);
    }
}

std::string Hex(const std::string& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const unsigned char ch : bytes) {
        result.push_back(digits[ch >> 4]);
        result.push_back(digits[ch & 15]);
    }
    return result;
}

void WriteText(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const bool opened = file.is_open();
    file << text;
    const bool write_good = file.good(), write_fail = file.fail(), write_bad = file.bad();
    if (opened) file.close(); // original scope-exit close, made explicit to retain its result
    const bool still_open = file.is_open(), close_fail = file.fail(), close_bad = file.bad();
    Evidence([&] {
        return nlohmann::json{{"phase", "ready_write"}, {"pid", lubancode::platform::CurrentProcessId()},
            {"path", lubancode::platform::PathToUtf8(path)}, {"parent_error", ec.value()},
            {"requested_bytes", text.size()}, {"requested_hex", Hex(text)}, {"opened", opened},
            {"write_good", write_good}, {"write_fail", write_fail}, {"write_bad", write_bad},
            {"close_attempted", opened}, {"still_open", still_open},
            {"fail_after_close", close_fail}, {"bad_after_close", close_bad}};
    });
}

bool WaitForFile(const fs::path& path, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code ec;
        if (fs::exists(path, ec)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : std::string();
    // argc 数清楚:prog + mode + <按模式的参数>(hold 的 release 可省)。
    const bool usage_ok = (mode == "hold" && (argc == 4 || argc == 5)) ||
                          (mode == "crash" && argc == 4) ||
                          (mode == "register" && argc == 7);
    if (!usage_ok) {
        std::fprintf(stderr,
                     "用法: workspace_manifest_racer <hold|crash|register> ...\n"
                     "  hold <workspace_dir> <ready_file> [release_file]\n"
                     "  crash <workspace_dir> <ready_file>\n"
                     "  register <workspaces_root> <project_root> <now_ms> <ready_file> "
                     "<go_file>\n");
        return 2;
    }

    if (mode == "register") {
        const fs::path workspaces_root = argv[2];
        const fs::path project_root = argv[3];
        const std::int64_t now_ms = std::atoll(argv[4]);
        const fs::path ready = argv[5];
        const fs::path go = argv[6];
        const auto identity = lubancode::workspace::ResolveWorkspaceIdentity(project_root, {});
        if (!identity.has_value()) {
            WriteText(ready, "fail: 身份解析不出: " + identity.error() + "\n");
            return 3;
        }
        WriteText(ready, "ok\n");
        if (!WaitForFile(go, 120000)) return 4;  // 等测试端的发令枪
        const auto opened =
            lubancode::workspace::OpenOrRegisterWorkspace(workspaces_root, *identity, now_ms);
        if (!opened.has_value()) {
            WriteText(ready, "fail: " + opened.error() + "\n");
            return 5;
        }
        return 0;
    }

    const fs::path workspace_dir = argv[2];
    const fs::path ready = argv[3];
    lubancode::workspace::ManifestLock lock;
    // 磨档占锁(60×100ms,与生产开房路同配):单发撞在建窗即刻死,见头注。
    const auto acquire_started = std::chrono::steady_clock::now();
    const auto result = lubancode::workspace::ManifestLock::Acquire(
        workspace_dir, &lock, /*attempts=*/60, /*interval_ms=*/100);
    const auto acquire_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - acquire_started).count();
    const auto report_acquire = [&] {
        Evidence([&] {
            auto arguments = nlohmann::json::array();
            auto argument_bytes = nlohmann::json::array();
            for (int index = 0; index < argc; ++index) {
                arguments.push_back(argv[index]);
                argument_bytes.push_back(Hex(argv[index]));
            }
            return nlohmann::json{{"phase", "acquire"}, {"mode", mode},
                {"pid", lubancode::platform::CurrentProcessId()},
                {"argv", std::move(arguments)}, {"argv_hex", std::move(argument_bytes)},
                {"workspace_dir", lubancode::platform::PathToUtf8(workspace_dir)},
                {"status", static_cast<int>(result.status)}, {"detail", result.detail},
                {"detail_hex", Hex(result.detail)}, {"holds", lock.holds()},
                {"elapsed_ms", acquire_elapsed}, {"attempts", 60}, {"interval_ms", 100}};
        });
    };
    if (result.status != lubancode::workspace::ManifestLock::Status::Acquired) {
        WriteText(ready, "fail: " + result.detail + "\n");
        report_acquire();
        return 3;
    }
    WriteText(ready, "ok\n");
    report_acquire();
    if (mode == "crash") {
        std::_Exit(9);  // 暴毙:不析构、不放锁,owner 账留在盘上
    }
    const fs::path release = argc > 4 ? fs::path(argv[4]) : fs::path();
    if (release.empty() || !WaitForFile(release, 120000)) return 4;
    return 0;  // 析构按 owner 核账后放锁
}
