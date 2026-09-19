// lock.hpp 的实现(范式样板 src/gateway/process.cpp 的 GatewayLock,语义
// 对齐 scripts/updater.py 的 InstallLock;口径差见 lock.hpp 文件头)。
#include "updater/lock.hpp"

#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <share.h>
#endif

#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/wall_clock.hpp"
#include "trajectory/session_lock.hpp"
#include "updater/manifest.hpp"

namespace lubancode::updater {

namespace {

// 锁账的三个字段(pid 必有;acquired_at_utc 必有;start_token 可选——
// 旧格式/python 版写的锁没有它,读侧缺省按 pid 裁决)。
struct LockRecord {
    unsigned long pid = 0;
    std::string acquired_at_utc;
    std::string start_token;
    bool has_start_token = false;
};

// 读锁账(容错):文件不在/坏 JSON/不是 object/pid 不合格 -> nullopt。
std::optional<LockRecord> ReadLockRecord(const std::filesystem::path& lock_file,
                                         std::string* error) {
    const auto data = ReadJsonFileTolerant(lock_file);
    if (!data.has_value() || !data->is_object()) {
        if (error != nullptr) {
            std::error_code ec;
            *error = !std::filesystem::exists(lock_file, ec) ? "gone" : "锁账不是合法 JSON object";
        }
        return std::nullopt;
    }
    if (!data->contains("pid") || !(*data)["pid"].is_number_integer()) {
        if (error != nullptr) *error = "锁账缺合格 pid";
        return std::nullopt;
    }
    LockRecord record;
    record.pid = static_cast<unsigned long>((*data)["pid"].get<std::int64_t>());
    if (data->contains("acquired_at_utc") && (*data)["acquired_at_utc"].is_string()) {
        record.acquired_at_utc = (*data)["acquired_at_utc"].get<std::string>();
    }
    if (data->contains("start_token") && (*data)["start_token"].is_string()) {
        record.start_token = (*data)["start_token"].get<std::string>();
        record.has_start_token = true;
    }
    return record;
}

}  // namespace

std::filesystem::path InstallLockPath(const LayoutPaths& paths) { return paths.updates / ".lock"; }

InstallRootLock::AcquireResult InstallRootLock::TryAcquire(const LayoutPaths& paths,
                                                           InstallRootLock* out) {
    AcquireResult result;
    if (out == nullptr) {
        result.status = AcquireResult::Status::IoError;
        result.detail = "out 指针为空";
        return result;
    }
    out->Release();

    const std::filesystem::path lock_file = InstallLockPath(paths);
    // 陈锁清掉后有界重试:并发下别人可能先占,撞满即报错,不无限绕。
    // 每一轮头一步都是 create-new 原子占位——双进程同时走到这里,OS
    // 保证至多一只成功(python 三轮同款)。
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::error_code ec;
        std::filesystem::create_directories(paths.updates, ec);
        if (ec) {
            result.status = AcquireResult::Status::IoError;
            result.detail = "建 updates/ 失败: " + platform::PathToUtf8(paths.updates) + ": " +
                            ec.message();
            return result;
        }
        std::FILE* file = nullptr;
#ifdef _WIN32
        // _SH_DENYNO:锁文件本体仍可被只读探测(status/别的更新器核身份)。
        file = _wfsopen(lock_file.c_str(), L"wbx", _SH_DENYNO);
#else
        file = std::fopen(lock_file.c_str(), "wbx");
#endif
        if (file != nullptr) {
            nlohmann::json record = nlohmann::json::object();
            record["pid"] = platform::CurrentProcessId();
            record["acquired_at_utc"] = UtcNowIso8601();
            record["start_token"] = trajectory::CurrentProcessStartToken();
            const std::string text = CanonicalJsonDump(record);
            const bool wrote =
                std::fwrite(text.data(), 1, text.size(), file) == text.size() && std::fflush(file) == 0;
            if (!wrote) {
                std::fclose(file);
                std::error_code remove_ec;
                std::filesystem::remove(lock_file, remove_ec);
                result.status = AcquireResult::Status::IoError;
                result.detail = "锁账写不进 " + platform::PathToUtf8(lock_file);
                return result;
            }
            out->lock_file_ = lock_file;
            out->file_ = file;
            result.status = AcquireResult::Status::Acquired;
            return result;
        }
        // 占位失败(文件已存在):读账、核身份再定去留。
        std::string read_error;
        const auto existing = ReadLockRecord(lock_file, &read_error);
        if (existing.has_value()) {
            const LockRecord& holder = *existing;
            const unsigned long self_pid = platform::CurrentProcessId();
            if (holder.pid == self_pid) {
                const std::string self_token = trajectory::CurrentProcessStartToken();
                if (!holder.has_start_token || holder.start_token == self_token) {
                    // 旧格式缺 token 按 pid 裁决;token 全对是同进程重入。
                    // python 口径:不可重入,拒。
                    result.status = AcquireResult::Status::RefusedReentrant;
                    result.holder_pid = holder.pid;
                    result.detail = "本进程已持有安装锁(不可重入)";
                    return result;
                }
                // pid 是自己但 token 对不上:PID 复用,旧进程死透了——按
                // 陈锁走清抢(GatewayLock 身份核)。
            } else if (platform::IsProcessAlive(holder.pid)) {
                result.status = AcquireResult::Status::RefusedAliveHolder;
                result.holder_pid = holder.pid;
                result.detail = "另一个更新器正在本安装根上运行(pid " +
                                std::to_string(holder.pid) + ");等它收尾或处理完再试";
                return result;
            }
            // 死 pid 陈锁:rename 成 .stale-<epoch秒> 再抢(python 同款,
            // 不直接删——留现场可查)。挪不动只烧一轮,不报错。
            const std::string stale_name =
                ".lock.stale-" + std::to_string(platform::WallClockNowMs() / 1000);
            const std::filesystem::path stale = paths.updates / stale_name;
            std::error_code rename_ec;
            std::filesystem::rename(lock_file, stale, rename_ec);
            if (!rename_ec) {
                result.cleared_stale = true;
                result.stale_moved_to = platform::PathToUtf8(stale);
            }
            continue;
        }
        if (read_error != "gone") {
            // 锁文件在但读不懂:不敢动,明报(口径差见文件头)。
            result.status = AcquireResult::Status::RefusedBrokenLock;
            result.detail = "锁账读不懂: " + read_error + "(锁文件: " +
                            platform::PathToUtf8(lock_file) + ")";
            return result;
        }
        // 读不到锁文件但占位又撞了:并发尾巴(别人创建后被清),重试。
    }
    result.status = AcquireResult::Status::ContentionFailure;
    result.detail = "安装锁竞争失败,稍后重试: " + platform::PathToUtf8(lock_file);
    return result;
}

InstallRootLock::InstallRootLock(InstallRootLock&& other) noexcept
    : lock_file_(std::move(other.lock_file_)), file_(other.file_) {
    other.file_ = nullptr;
    other.lock_file_.clear();
}

InstallRootLock& InstallRootLock::operator=(InstallRootLock&& other) noexcept {
    if (this != &other) {
        Release();
        lock_file_ = std::move(other.lock_file_);
        file_ = other.file_;
        other.file_ = nullptr;
        other.lock_file_.clear();
    }
    return *this;
}

InstallRootLock::~InstallRootLock() { Release(); }

void InstallRootLock::Release() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (lock_file_.empty()) return;
    // 只删 pid 是自己的锁:账被人接管过(理论上只可能出现在异常时序),
    // 删掉的就是别人的锁——读一遍账,核对 pid 再动手(python 同款)。
    const auto holder = ReadLockRecord(lock_file_, nullptr);
    if (holder.has_value() && holder->pid == platform::CurrentProcessId()) {
        std::error_code ec;
        std::filesystem::remove(lock_file_, ec);
        // 删失败只剩日志可打;残留锁由下次 TryAcquire 走身份核收口。
    }
    lock_file_.clear();
}

}  // namespace lubancode::updater
