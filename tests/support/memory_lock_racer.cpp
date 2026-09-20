// SV-01(记忆目录锁所有权)的跨进程靶子:真起一只子进程占在 worker.lock
// 上,测试端用生产同一份 memory::OwnerLock::TryAcquire 与它竞争或接尸——
// 不拿同进程顺序单测冒充双进程互斥(照 gateway_lock_racer 的册写法)。
//
// 用法: memory_lock_racer <mode:hold|crash> <lock_dir> <ready_file> [release_file]
//   hold:  TryAcquire → ready 写 "ok"/"fail: ..." → 轮询 release_file 出现
//          (20ms 一拍,120s 兜底)→ 退出(析构按 owner 核账后放锁)。
//   crash: TryAcquire → ready 写 "ok"/"fail: ..." → std::_Exit(9):不跑析
//          构、不放锁,owner 账与锁目录原样留在盘上——模拟 worker 暴毙
//          (句柄由内核回收,持有者身份已死)。
// 退出码:0 = 正常收场;2 = 用法错;3 = 没取到锁;4 = 等不到放行令。
// 栅栏协议:ready 文件是"已取到锁"的屏障,release 文件是"放行退出"的
// 屏障——交错由文件控制,不靠两头掐表。
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "memory/project_memory.hpp"

namespace {

namespace fs = std::filesystem;

void WriteText(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
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
    if (argc < 4 || (argc > 1 && std::string(argv[1]) != "hold" && std::string(argv[1]) != "crash")) {
        std::fprintf(stderr,
                     "用法: memory_lock_racer <mode:hold|crash> <lock_dir> <ready_file> "
                     "[release_file]\n");
        return 2;
    }
    const std::string mode = argv[1];
    const fs::path lock_dir = argv[2];
    const fs::path ready = argv[3];
    const fs::path release = argc > 4 ? fs::path(argv[4]) : fs::path();

    lubancode::memory::OwnerLock lock;
    const auto result = lubancode::memory::OwnerLock::TryAcquire(lock_dir, &lock);
    if (result.status != lubancode::memory::OwnerLock::Status::Acquired) {
        WriteText(ready, "fail: " + result.detail + "\n");
        return 3;
    }
    WriteText(ready, "ok\n");
    if (mode == "crash") {
        std::_Exit(9);  // 暴毙:不析构、不放锁,owner 账留在盘上
    }
    if (release.empty() || !WaitForFile(release, 120000)) return 4;
    return 0;  // 析构按 owner 核账后放锁
}
