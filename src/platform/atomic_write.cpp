// AtomicWriteFile 的实现(见 atomic_write.hpp 的合同注释)。
#include "platform/atomic_write.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

#include "platform/paths.hpp"  // PathToUtf8/ReplaceFileAtomically;Windows 另有 Utf8ToWide

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <io.h>  // _fileno/_commit
#include <share.h>
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

void Observe(ImmutableIoObservation& out, std::int64_t result, bool succeeded,
             ImmutableErrorDomain domain, std::int64_t error) noexcept {
    out.attempted = true;
    out.succeeded = succeeded;
    out.result = result;
    out.error_domain = succeeded ? ImmutableErrorDomain::None : domain;
    out.native_error = succeeded ? 0 : error;
}

// 文件数据落盘(ProcessCrashDurability 档):fsync/_commit 已写出的数据。
// 返回空 = 成功;否则人话错误。失败注入旗只替代刷盘本身(测试分阶段用,
// 见头文件测试注入面),不开不关文件。
std::string FlushFileToDisk(std::FILE* file, const std::filesystem::path& path,
                           ImmutableWriteReceipt* witness = nullptr) {
    if (g_file_flush_fail.load(std::memory_order_relaxed)) {
        if (witness) witness->file_sync.injected_failure = true;
        return "文件刷盘失败(测试注入): " + PathToUtf8(path);
    }
    if (witness) errno = 0;
    const int flushed = std::fflush(file);
    const int flush_error = errno;
    if (witness) Observe(witness->flush, flushed, flushed == 0, ImmutableErrorDomain::Errno, flush_error);
    if (flushed != 0) {
        return "flush 失败: " + PathToUtf8(path);
    }
#ifdef _WIN32
    if (witness) errno = 0;
    const int synced = _commit(_fileno(file));
    const int sync_error = errno;
    if (witness) Observe(witness->file_sync, synced, synced == 0, ImmutableErrorDomain::Errno, sync_error);
    if (synced != 0) {
        return "commit 失败: " + PathToUtf8(path);
    }
#else
    if (witness) errno = 0;
    const int synced = fsync(fileno(file));
    const int sync_error = errno;
    if (witness) Observe(witness->file_sync, synced, synced == 0, ImmutableErrorDomain::Errno, sync_error);
    if (synced != 0) {
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
std::string FlushParentDirectory(const std::filesystem::path& dir,
                                 ImmutableWriteReceipt* witness = nullptr) {
    if (dir.empty()) {
        return std::string();
    }
    if (g_dir_flush_fail.load(std::memory_order_relaxed)) {
        if (witness) witness->parent_sync.injected_failure = true;
        return "目录 flush 失败(测试注入): " + PathToUtf8(dir);
    }
#ifdef _WIN32
    HANDLE handle = CreateFileW(dir.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    const auto open_error = GetLastError();
    if (witness) {
        witness->parent_open.attempted = true;
        witness->parent_open.succeeded = handle != INVALID_HANDLE_VALUE;
        if (handle == INVALID_HANDLE_VALUE) {
            witness->parent_open.error_domain = ImmutableErrorDomain::Win32;
            witness->parent_open.native_error = open_error;
        }
    }
    if (handle == INVALID_HANDLE_VALUE) {
        return "目录句柄打不开: " + PathToUtf8(dir);
    }
    const BOOL flushed = FlushFileBuffers(handle);
    const auto flush_error = GetLastError();
    const BOOL closed = CloseHandle(handle);
    const auto close_error = GetLastError();
    if (witness) {
        Observe(witness->parent_sync, flushed, flushed != FALSE, ImmutableErrorDomain::Win32, flush_error);
        Observe(witness->parent_close, closed, closed != FALSE, ImmutableErrorDomain::Win32, close_error);
    }
    if (!flushed) {
        return "目录 flush 失败: " + PathToUtf8(dir);
    }
    if (witness && !closed) return "目录 close 失败: " + PathToUtf8(dir);
#else
    if (witness) errno = 0;
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    const int open_error = errno;
    if (witness) Observe(witness->parent_open, fd, fd >= 0, ImmutableErrorDomain::Errno, open_error);
    if (fd < 0) {
        return "目录打不开: " + PathToUtf8(dir);
    }
    if (witness) errno = 0;
    const int synced = ::fsync(fd);
    const int sync_error = errno;
    if (witness) errno = 0;
    const int closed = ::close(fd);
    const int close_error = errno;
    if (witness) {
        Observe(witness->parent_sync, synced, synced == 0, ImmutableErrorDomain::Errno, sync_error);
        Observe(witness->parent_close, closed, closed == 0, ImmutableErrorDomain::Errno, close_error);
    }
    if (synced != 0) {
        return "目录 fsync 失败: " + PathToUtf8(dir);
    }
    if (witness && closed != 0) return "目录 close 失败: " + PathToUtf8(dir);
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

std::expected<AtomicWriteReceipt, AtomicWriteError> AtomicWriteFile(const std::filesystem::path& requested_target,
                                                                    std::string_view bytes,
                                                                    WriteDurability durability) {
    const auto target = FileIoPath(requested_target);
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
    temp = FileIoPath(temp); // the suffix can cross the limit even if target did not

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

ImmutableWriteReceipt CreateImmutableFileDetailed(const std::filesystem::path& requested_target,
    std::string_view bytes, WriteDurability durability) {
    ImmutableWriteReceipt out;
    out.requested = durability;
    const auto error = [&](const char* code, std::string message) {
        if (out.error_code.empty()) { out.error_code = code; out.message = std::move(message); }
    };
    if (durability != WriteDurability::AtomicVisibility && durability != WriteDurability::ProcessCrashDurability) {
        error("immutable.invalid_durability", "unsupported immutable publication durability"); return out;
    }
    if (requested_target.empty() || PathToUtf8(requested_target).find('\0') != std::string::npos) {
        error("immutable.invalid_path", "immutable target is empty or contains NUL"); return out;
    }
    std::error_code ec;
    out.target = std::filesystem::absolute(requested_target, ec).lexically_normal();
    if (ec || out.target.filename().empty()) {
        error("immutable.invalid_path", "immutable target cannot be resolved"); return out;
    }
    const auto parent = out.target.parent_path();
    const auto native_parent = FileIoPath(parent);
    if (!std::filesystem::is_directory(native_parent, ec) || ec) {
        error("immutable.parent_unavailable", "immutable publication requires an existing parent directory"); return out;
    }
    const auto native_target = FileIoPath(out.target);
    out.temporary = out.target;
    out.temporary += "." + std::to_string(CurrentPid()) + "-" + std::to_string(NextTempSequence()) + ".tmp";
    const auto native_temp = FileIoPath(out.temporary); // normalize after adding the suffix

    struct TempOwner {
        const std::filesystem::path& path;
        ImmutableWriteReceipt& receipt;
        std::FILE* file = nullptr;
        bool owned = false;
        void Close() noexcept {
            if (!file) return;
            errno = 0;
            const int result = std::fclose(file);
            const int saved = errno;
            file = nullptr; // Never retry fclose after a native close error.
            Observe(receipt.file_close, result, result == 0, ImmutableErrorDomain::Errno, saved);
        }
        void Cleanup() noexcept {
            if (!owned) return;
#ifdef _WIN32
            const BOOL result = DeleteFileW(path.c_str());
            const auto saved = GetLastError();
            Observe(receipt.cleanup, result, result != FALSE, ImmutableErrorDomain::Win32, saved);
#else
            errno = 0;
            const int result = ::unlink(path.c_str());
            const int saved = errno;
            Observe(receipt.cleanup, result, result == 0, ImmutableErrorDomain::Errno, saved);
#endif
            owned = false; // Keep an unremoved orphan; do not retry or broaden deletion.
        }
        ~TempOwner() { Close(); Cleanup(); }
    } temp{native_temp, out};
    out.temp_open.attempted = true;
    errno = 0;
#ifdef _WIN32
    temp.file = _wfsopen(native_temp.c_str(), L"wbx", _SH_DENYNO);
#else
    temp.file = std::fopen(native_temp.c_str(), "wbx");
#endif
    const int open_error = errno;
    out.temp_open.succeeded = temp.file != nullptr;
    if (!temp.file) {
        out.temp_open.error_domain = ImmutableErrorDomain::Errno;
        out.temp_open.native_error = open_error;
        error("immutable.temp_open_failed", "exclusive temporary creation failed: " + PathToUtf8(out.temporary));
        return out; // Not ours: never remove a preexisting temp or symlink.
    }
    temp.owned = true;
    if (!bytes.empty()) {
        out.body.attempted = true; out.body.requested_bytes = bytes.size();
        errno = 0;
        out.body.written_bytes = std::fwrite(bytes.data(), 1, bytes.size(), temp.file);
        const int saved = errno;
        out.body.succeeded = out.body.written_bytes == bytes.size();
        if (!out.body.succeeded) {
            out.body.error_domain = ImmutableErrorDomain::Errno; out.body.native_error = saved;
            error("immutable.temp_write_failed", "temporary body write was incomplete");
        }
    }
    if (out.error_code.empty()) {
        if (durability == WriteDurability::ProcessCrashDurability) {
            const auto detail = FlushFileToDisk(temp.file, native_temp, &out);
            if (!detail.empty()) error("immutable.file_flush_failed", detail);
        } else {
            errno = 0;
            const int flushed = std::fflush(temp.file);
            const int saved = errno;
            Observe(out.flush, flushed, flushed == 0, ImmutableErrorDomain::Errno, saved);
            if (flushed != 0) error("immutable.file_flush_failed", "temporary stdio flush failed");
        }
    }
    temp.Close();
    if (!out.file_close.succeeded) error("immutable.file_close_failed", "temporary file close failed");
    if (!out.error_code.empty()) { temp.Cleanup(); return out; }

#ifdef _WIN32
    const BOOL published = MoveFileExW(native_temp.c_str(), native_target.c_str(), 0);
    const auto publish_error = GetLastError();
    Observe(out.publish, published, published != FALSE, ImmutableErrorDomain::Win32, publish_error);
    const bool exists = publish_error == ERROR_ALREADY_EXISTS || publish_error == ERROR_FILE_EXISTS;
#else
    errno = 0;
    const int published = ::link(native_temp.c_str(), native_target.c_str());
    const int publish_error = errno;
    Observe(out.publish, published, published == 0, ImmutableErrorDomain::Errno, publish_error);
    const bool exists = publish_error == EEXIST;
#endif
    if (!out.publish.succeeded) {
        error(exists ? "immutable.target_exists" : "immutable.publish_failed",
              exists ? "结果仓不可变名已存在，不允许覆盖" : "结果仓不可变名原生发布失败");
        temp.Cleanup(); return out;
    }
    // Set visibility before any fallible diagnostic/string operation.
    out.outcome = durability == WriteDurability::ProcessCrashDurability
        ? WriteOutcome::CommittedDurabilityUnconfirmed : WriteOutcome::CommittedDurabilityNotRequested;
#ifdef _WIN32
    temp.owned = false; // The successful no-replace move consumed this name.
#else
    temp.Cleanup(); // link published target; cleanup can only remove our temp name.
    if (!out.cleanup.succeeded) error("immutable.cleanup_failed", "published target retained; temporary unlink failed");
#endif
    if (durability == WriteDurability::ProcessCrashDurability) {
        const auto detail = FlushParentDirectory(native_parent, &out);
        if (!detail.empty()) error("immutable.directory_flush_failed", detail);
        else {
            out.confirmed_parent = parent;
            out.outcome = WriteOutcome::CommittedDurable;
        }
    }
    return out;
}

}  // namespace lubancode::platform
