// AtomicWriteFile 的实现(见 atomic_write.hpp 的合同注释)。
#include "platform/atomic_write.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <system_error>

#include "platform/paths.hpp"  // PathToUtf8/ReplaceFileAtomically;Windows 另有 Utf8ToWide

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <io.h>  // _fileno/_commit
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace lubancode::platform {

namespace {

// 测试注入旗(见 atomic_write.hpp 的测试注入面):默认 false,生产路径
// 零行为差异,只多一次原子读。
std::atomic<bool> g_file_flush_fail{false};
std::atomic<bool> g_dir_flush_fail{false};

// 唯一临时名的序号:同进程内单调递增,拼上 pid 后跨进程也不重样。
std::uint64_t NextTempSequence() {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t CurrentPid() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

// 用窄字节口打开临时文件:Windows 的 std::fopen 走 ACP,路径带非 ASCII
// 会开错/开不成;这里按平台拿宽口/字节口。
std::FILE* OpenTempFile(const std::filesystem::path& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

// 文件数据落盘(ProcessCrashDurability 档):fsync/_commit 已写出的数据。
// 返回空 = 成功;否则人话错误。失败注入旗只替代刷盘本身(测试分阶段用,
// 见头文件测试注入面),不开不关文件。
std::string FlushFileToDisk(std::FILE* file, const std::filesystem::path& path) {
    if (g_file_flush_fail.load(std::memory_order_relaxed)) {
        return "文件刷盘失败(测试注入): " + PathToUtf8(path);
    }
    if (std::fflush(file) != 0) {
        return "flush 失败: " + PathToUtf8(path);
    }
#ifdef _WIN32
    if (_commit(_fileno(file)) != 0) {
        return "commit 失败: " + PathToUtf8(path);
    }
#else
    if (fsync(fileno(file)) != 0) {
        return "fsync 失败: " + PathToUtf8(path);
    }
#endif
    return std::string();
}

// 目录条目落盘(ProcessCrashDurability 档):换名本身记进父目录后,把
// 父目录的条目也刷下去——不然换名只在页缓存里,掉电就翻案。
// Windows 开目录句柄要 FILE_FLAG_BACKUP_SEMANTICS;FlushFileBuffers 要求
// 句柄带写访问。失败不推翻已完成的替换(数据已可见),只如实报错。
// 空路径(裸文件名,无父段)没有可开的目录句柄:这一步按合同直接算过
// (FD-04 钉死的现行行为)。失败注入旗只替代真实刷盘(测试分阶段用)。
std::string FlushParentDirectory(const std::filesystem::path& dir) {
    if (dir.empty()) {
        return std::string();
    }
    if (g_dir_flush_fail.load(std::memory_order_relaxed)) {
        return "目录 flush 失败(测试注入): " + PathToUtf8(dir);
    }
#ifdef _WIN32
    HANDLE handle = CreateFileW(dir.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return "目录句柄打不开: " + PathToUtf8(dir);
    }
    const BOOL flushed = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!flushed) {
        return "目录 flush 失败: " + PathToUtf8(dir);
    }
#else
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return "目录打不开: " + PathToUtf8(dir);
    }
    const int synced = ::fsync(fd);
    ::close(fd);
    if (synced != 0) {
        return "目录 fsync 失败: " + PathToUtf8(dir);
    }
#endif
    return std::string();
}

}  // namespace

// 测试注入面(见 atomic_write.hpp):装/卸各一个原子写,生产代码不得调用。
void SetFileFlushFailureForTest(bool fail) {
    g_file_flush_fail.store(fail, std::memory_order_relaxed);
}

void SetDirectoryFlushFailureForTest(bool fail) {
    g_dir_flush_fail.store(fail, std::memory_order_relaxed);
}

// 替换失败的短拒分类:Windows 的原子换名有防病毒/索引过滤驱动短拒类
// (work_pump.cpp 三案 CI 实测注记:320 次换名被拒 48-57 次),有界重试
// 可过;少数永久因(目标是目录)混在同类里——重试无害,换名原子、失败
// 时 target 未动,有界重试后仍失败照实报。POSIX rename 无此瞬态类。
#ifdef _WIN32
constexpr WriteFailureKind kReplaceFailureKind = WriteFailureKind::TransientReject;
#else
constexpr WriteFailureKind kReplaceFailureKind = WriteFailureKind::Permanent;
#endif

std::expected<AtomicWriteReceipt, AtomicWriteError> AtomicWriteFile(const std::filesystem::path& target,
                                                                    std::string_view bytes,
                                                                    WriteDurability durability) {
    const auto fail = [](std::string code, std::string message, WriteOutcome outcome, WriteFailureKind kind) {
        return std::expected<AtomicWriteReceipt, AtomicWriteError>(
            std::unexpected(AtomicWriteError{std::move(code), std::move(message), outcome, kind}));
    };

    // 父目录:不在就建。target 没有父段(纯文件名)时 parent_path() 为空,
    // 落在进程当前目录,不建。
    const std::filesystem::path parent = target.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return fail("atomic.mkdir_failed", "父目录建不成: " + PathToUtf8(parent) + ": " + ec.message(),
                        WriteOutcome::NotCommitted, WriteFailureKind::Permanent);
        }
    }

    // 唯一临时名:同目录、进程内不重样、跨进程不撞车。
    std::filesystem::path temp = target;
    temp += "." + std::to_string(CurrentPid()) + "-" + std::to_string(NextTempSequence()) + ".tmp";

    {
        std::FILE* file = OpenTempFile(temp);
        if (file == nullptr) {
            return fail("atomic.tmp_open_failed", "临时文件打不开: " + PathToUtf8(temp),
                        WriteOutcome::NotCommitted, WriteFailureKind::Permanent);
        }
        bool write_ok = true;
        std::string write_detail;
        if (!bytes.empty()) {
            write_ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
            if (!write_ok) {
                write_detail = "写临时文件失败: " + PathToUtf8(temp);
            }
        }
        if (write_ok && durability == WriteDurability::ProcessCrashDurability) {
            write_detail = FlushFileToDisk(file, temp);
            write_ok = write_detail.empty();
        }
        // close 检查:fclose 会冲缓冲并报 IO 错,close 上的失败不放行。
        if (std::fclose(file) != 0) {
            write_ok = false;
            if (write_detail.empty()) {
                write_detail = "关临时文件失败: " + PathToUtf8(temp);
            }
        }
        if (!write_ok) {
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            // 换名没发生,一切还在"未提交"阶段。
            return fail("atomic.tmp_write_failed", write_detail, WriteOutcome::NotCommitted,
                        WriteFailureKind::Permanent);
        }
    }

    // 同进程内并发替换同一目标要串行:Windows 的 MoveFileExW 换名要短暂
    // 打开目标,两线程同时换名会撞 ERROR_ACCESS_DENIED。POSIX rename 天生
    // 原子,串不串无差,锁着也无妨。跨进程的冲突仍然如实报错——那不是
    // 本层能收场的。
    static std::mutex replace_mutex;
    const std::lock_guard<std::mutex> replace_lock(replace_mutex);
    const auto replaced = ReplaceFileAtomically(temp, target);
    if (!replaced.has_value()) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return fail("atomic.replace_failed", "原子替换失败: " + replaced.error(),
                    WriteOutcome::NotCommitted, kReplaceFailureKind);
    }
    if (durability == WriteDurability::ProcessCrashDurability) {
        const std::string dir_flush = FlushParentDirectory(parent);
        if (!dir_flush.empty()) {
            // 替换已经生效且可见;目录条目没刷下去只影响掉电那一层,如实
            // 报错让调用方记账,不回滚(回滚反而再开一个非原子窗口)。这一
            // 格的 outcome 是 CommittedDurabilityUnconfirmed——新内容已在
            // target 上,不是"未写盘"(FD-04)。
            return fail("atomic.durability_flush_failed", dir_flush,
                        WriteOutcome::CommittedDurabilityUnconfirmed, WriteFailureKind::Permanent);
        }
        return AtomicWriteReceipt{WriteOutcome::CommittedDurable};
    }
    return AtomicWriteReceipt{WriteOutcome::CommittedDurabilityNotRequested};
}

}  // namespace lubancode::platform
