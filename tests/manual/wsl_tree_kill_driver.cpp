// WSL 真跑的 POSIX 进程组收树夹具(ripgrep 迁移单 P0-4 验收:
// "Windows Job Object 与 POSIX 进程组各做一只 rg 再生孩子的夹具,证明
// 取消不留孤儿(POSIX 侧 WSL 验)"。Windows 侧由单测的 @spawn-child
// 场景(Job Object 连坐)覆盖;这里证 POSIX 侧 setpgid + killpg 连坐。
//
// 流程:ChildProcess 起 fake_ripgrep(@spawn-child)——它 fork 一个把 PID
// 写进 marker 的孩子,双双重睡;主线程 WaitForExit(300ms) 超时返回 false;
// Shutdown 收树;随后用 kill(pid, 0) 轮询验证孙进程死透。
// 另顺带验 WaitForExit 的 cancel 路:第二局起 @never-exit,cancel 旗在
// 等待中途置位,WaitForExit 应在一个分片内返回 false。
//
// 编译(WSL):
//   g++-13 -std=c++23 -I src build/wsl_tree_kill_driver.cpp \
//     src/platform/process_posix.cpp src/platform/text_encoding.cpp \
//     src/platform/paths_posix.cpp tests/support/fake_ripgrep.cpp \
//     src/tools/path_utils.hpp -lpthread -o build/wsl_tree_kill_driver
// (text_encoding 只用 UTF-8 边界函数,paths_posix 提供它要的窄口。)

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include <signal.h>
#include <sys/types.h>

#include "platform/process.hpp"

namespace {

std::string ReadAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return std::string();
    }
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool WaitPidDead(long pid, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH;
}

int Fail(const char* what, const std::string& detail = std::string()) {
    std::printf("FAIL: %s (errno=%d) %s\n", what, errno, detail.c_str());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <fake_ripgrep> <workdir>\n", argv[0]);
        return 2;
    }
    const std::string fake_rg = argv[1];
    const std::string workdir = argv[2];
    const std::string marker = workdir + "/grandchild.pid";
    setenv("LUBANCODE_FAKE_RG_MARKER", marker.c_str(), /*overwrite=*/1);

    // ---- 第一局:再生孩子 + 超时收树 ----
    {
        lubancode::platform::ChildProcess process;
        const auto spawn = process.Start(
            fake_rg, {"--", "@spawn-child"}, {},
            [](std::string_view) { return true; }, [](std::string_view) {}, workdir);
        if (!spawn.success) {
            return Fail("spawn fake_rg", spawn.error);
        }
        std::printf("spawned fake_rg, waiting grandchild marker...\n");
        std::string pid_text;
        for (int i = 0; i < 200; ++i) {
            pid_text = ReadAll(marker);
            if (!pid_text.empty()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (pid_text.empty()) {
            return Fail("grandchild marker never appeared");
        }
        const long long grandchild = std::strtol(pid_text.c_str(), nullptr, 10);
        std::printf("grandchild pid=%lld\n", grandchild);

        if (process.WaitForExit(300)) {
            return Fail("WaitForExit should time out on the sleeping tree");
        }
        process.Shutdown(200);
        if (!WaitPidDead(grandchild, 3000)) {
            return Fail("grandchild survived the tree kill (orphan!)");
        }
        std::printf("tree kill collected grandchild: ok\n");
    }

    // ---- 第二局:WaitForExit 的 cancel 路 ----
    {
        const std::string self_marker = workdir + "/self.pid";
        setenv("LUBANCODE_FAKE_RG_MARKER", self_marker.c_str(), 1);
        lubancode::platform::ChildProcess process;
        const auto spawn = process.Start(
            fake_rg, {"--", "@never-exit"}, {},
            [](std::string_view) { return true; }, [](std::string_view) {}, workdir);
        if (!spawn.success) {
            return Fail("spawn fake_rg (never-exit)", spawn.error);
        }
        std::atomic<bool> cancel{false};
        std::thread canceller([&cancel] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            cancel.store(true);
        });
        const auto t0 = std::chrono::steady_clock::now();
        const bool exited = process.WaitForExit(30'000, &cancel);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
        canceller.join();
        process.Shutdown(200);
        if (exited) {
            return Fail("WaitForExit returned true despite never-exit process");
        }
        if (elapsed > 3'000) {
            std::printf("FAIL: cancel took %lldms, slice polling broken\n", elapsed);
            return 1;
        }
        const std::string self_pid = ReadAll(self_marker);
        const long long pid = std::strtol(self_pid.c_str(), nullptr, 10);
        if (pid == 0 || !WaitPidDead(pid, 3000)) {
            return Fail("fake_rg survived cancel shutdown");
        }
        std::printf("cancel path: WaitForExit returned in %lldms, tree collected: ok\n", elapsed);
    }

    std::printf("WSL POSIX tree-kill fixture: ALL PASSED\n");
    return 0;
}
