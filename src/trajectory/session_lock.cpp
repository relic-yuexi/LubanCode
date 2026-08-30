#include "trajectory/session_lock.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>

#include "platform/paths.hpp"
#include "platform/process.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <share.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace lubancode::trajectory {
namespace {

#ifdef _WIN32
// Windows 起始 token:进程 creation FILETIME 的 64 位十六进制。
std::string StartTokenFromHandle(HANDLE process) {
    if (process == nullptr) {
        return {};
    }
    FILETIME creation = {};
    FILETIME exit_time = {};
    FILETIME kernel = {};
    FILETIME user = {};
    if (!GetProcessTimes(process, &creation, &exit_time, &kernel, &user)) {
        return {};
    }
    ULARGE_INTEGER value;
    value.LowPart = creation.dwLowDateTime;
    value.HighPart = creation.dwHighDateTime;
    if (value.QuadPart == 0) {
        return {};
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%llx",
                  static_cast<unsigned long long>(value.QuadPart));
    return buffer;
}
#endif

std::optional<SessionLockOwner> ReadOwnerFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const auto json = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (json.is_discarded()) {
        return std::nullopt;
    }
    return SessionLockOwner::FromJson(json);
}

}  // namespace

nlohmann::json SessionLockOwner::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema_version"] = 1;
    json["pid"] = static_cast<std::uint64_t>(pid);
    json["process_start_token"] = process_start_token;
    json["acquired_at_ms"] = acquired_at_ms;
    return json;
}

std::optional<SessionLockOwner> SessionLockOwner::FromJson(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("pid") || !json.at("pid").is_number_unsigned()) {
        return std::nullopt;
    }
    SessionLockOwner owner;
    owner.pid = static_cast<unsigned long>(json.at("pid").get<std::uint64_t>());
    if (json.contains("process_start_token") && json.at("process_start_token").is_string()) {
        owner.process_start_token = json.at("process_start_token").get<std::string>();
    }
    if (json.contains("acquired_at_ms") && json.at("acquired_at_ms").is_number_integer()) {
        owner.acquired_at_ms = json.at("acquired_at_ms").get<std::int64_t>();
    }
    return owner;
}

std::string CurrentProcessStartToken() {
#ifdef _WIN32
    return StartTokenFromHandle(GetCurrentProcess());
#else
    return ProcessStartTokenOf(static_cast<unsigned long>(::getpid()));
#endif
}

std::string ProcessStartTokenOf(unsigned long pid) {
    if (pid == 0) {
        return {};
    }
#ifdef _WIN32
    const HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        return {};
    }
    const std::string token = StartTokenFromHandle(process);
    CloseHandle(process);
    return token;
#else
    // /proc/<pid>/stat 第 22 字段 starttime(自 boot 的时钟滴答)。comm 字段
    // 可能含空格与括号,先找最后一个 ')' 再从尾巴取。
    std::ifstream file(std::filesystem::path("/proc") / std::to_string(pid) / "stat");
    if (!file.is_open()) {
        return {};
    }
    std::string line;
    std::getline(file, line);
    const auto close = line.rfind(')');
    if (close == std::string::npos) {
        return {};
    }
    std::istringstream tail(line.substr(close + 1));
    std::string field;
    // 尾巴第 1 个字段是 state(全表第 3);starttime 是全表第 22 → 尾巴第 20。
    for (int index = 1; index <= 20; ++index) {
        if (!(tail >> field)) {
            return {};
        }
    }
    return field;
#endif
}

LockHolderState ProbeLockHolder(const SessionLockOwner& owner) {
    if (owner.pid == 0) {
        return LockHolderState::Dead;
    }
    if (!platform::IsProcessAlive(owner.pid)) {
        return LockHolderState::Dead;
    }
    // 身份核对:token 对不上 = 这个 PID 已经换成了另一个进程(PID 复用),
    // 锁是上个世纪的残留,判 stale。任一侧 token 探不到,只凭活死保守判活。
    const bool same_process = owner.pid == platform::CurrentProcessId();
    const std::string actual =
        same_process ? CurrentProcessStartToken() : ProcessStartTokenOf(owner.pid);
    if (actual.empty() || owner.process_start_token.empty()) {
        return LockHolderState::Alive;
    }
    return actual == owner.process_start_token ? LockHolderState::Alive : LockHolderState::Dead;
}

SessionLock::SessionLock(SessionLock&& other) noexcept
    : path_(std::move(other.path_)), file_(std::exchange(other.file_, nullptr)) {}

SessionLock& SessionLock::operator=(SessionLock&& other) noexcept {
    if (this != &other) {
        Release();
        path_ = std::move(other.path_);
        file_ = std::exchange(other.file_, nullptr);
    }
    return *this;
}

SessionLock::~SessionLock() { Release(); }

void SessionLock::Release() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (!path_.empty()) {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        path_.clear();
    }
}

std::optional<SessionLockOwner> SessionLock::Inspect(const std::filesystem::path& session_dir) {
    return ReadOwnerFile(session_dir / "session.lock");
}

std::expected<SessionLock, std::string> SessionLock::Acquire(const std::filesystem::path& session_dir,
                                                             const SessionLockOwner& owner) {
    const std::filesystem::path path = session_dir / "session.lock";
    // 陈旧锁清掉后有界重试:并发下别人可能先占,撞满即报争用,不无限绕。
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::FILE* file = nullptr;
#ifdef _WIN32
        // _SH_DENYNO:锁文件本体仍可被只读 inspect 并行打开。
        file = _wfsopen(path.c_str(), L"wbx", _SH_DENYNO);
#else
        file = std::fopen(path.c_str(), "wbx");
#endif
        if (file != nullptr) {
            const std::string metadata = owner.ToJson().dump();
            const bool wrote = std::fwrite(metadata.data(), 1, metadata.size(), file) ==
                                   metadata.size() &&
                               std::fflush(file) == 0;
            if (!wrote) {
                std::fclose(file);
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
                return std::unexpected("lock.write_failed: owner 元数据落不了盘: " +
                                       platform::PathToUtf8(path));
            }
            SessionLock lock;
            lock.path_ = path;
            lock.file_ = file;
            return lock;
        }
        // 文件已存在:核持有者身份再定去留(§7.5 不凭存在/mtime 强抢)。
        const auto held = ReadOwnerFile(path);
        if (!held.has_value()) {
            return std::unexpected("lock.unreadable: 锁文件在,持有者身份读不出: " +
                                   platform::PathToUtf8(path));
        }
        if (ProbeLockHolder(*held) == LockHolderState::Alive) {
            return std::unexpected("lock.held_by_live_process: pid=" +
                                   std::to_string(held->pid) + " token=" +
                                   held->process_start_token);
        }
        // stale:清掉重试。清失败(权限/并发)下一轮 create-new 自会再报。
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    return std::unexpected("lock.contended: 陈旧锁清后仍反复撞: " + platform::PathToUtf8(path));
}

}  // namespace lubancode::trajectory
