// Session 独占锁(P0 新轨迹记录单 §7.5/§3.3.2)。
//
// session 目录持一把进程级独占 lock 文件(<session_dir>/session.lock),
// owner metadata 写 PID + 进程起始 token + 获取时间:
//   - 同 session 第二只 writer:create-new 撞名即拒,稳定码 lock.held_*;
//   - 只读 inspect 不抢锁,可并行(Inspect 只读文件);
//   - 陈旧锁核身份,不强抢:持有者 PID 死透、或 PID 活着但起始 token 对
//     不上(PID 被复用),才算 stale,清掉重试;探不到身份按"活"保守处理,
//     绝不凭 mtime 硬抢(§16.7)。
//
// 起始 token 是进程创建时刻的稳定值(Windows: 进程 creation FILETIME;
// POSIX: /proc/<pid>/stat 的 starttime),PID 复用也骗不过它。探不到给
// 空串,探不到时身份核对退回"只看 PID 活死"的保守档。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace lubancode::trajectory {

// 锁持有者身份(§7.5 owner metadata)。
struct SessionLockOwner {
    unsigned long pid = 0;
    std::string process_start_token;  // 进程起始 token;空 = 探不到
    std::int64_t acquired_at_ms = 0;

    nlohmann::json ToJson() const;
    static std::optional<SessionLockOwner> FromJson(const nlohmann::json& json);
};

// 本进程/指定进程的起始 token;探不到给空串(不抛错)。
std::string CurrentProcessStartToken();
std::string ProcessStartTokenOf(unsigned long pid);

// 核一枚持有者还活着吗。Alive = 活进程且身份对得上;Dead = 进程死透或
// PID 已被复用(token 对不上);身份探不到按 Alive 保守(宁拒不抢)。
enum class LockHolderState { Alive, Dead };
LockHolderState ProbeLockHolder(const SessionLockOwner& owner);

// RAII 独占锁:析构释放(关柄 + 删文件)。move-only。
class SessionLock {
public:
    SessionLock() = default;
    ~SessionLock();
    SessionLock(SessionLock&& other) noexcept;
    SessionLock& operator=(SessionLock&& other) noexcept;
    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;

    // 独占加锁。owner 由调用方注入(SessionManager 走时钟 seam,测试可
    // 伪造死进程身份)。失败给稳定码前缀:
    //   lock.held_by_live_process  持有者活着且身份对上,拒绝
    //   lock.unreadable            锁文件在但身份读不出,保守拒绝
    //   lock.write_failed          占住后元数据写失败
    //   lock.contended             陈旧锁清后仍反复撞(有界重试耗尽)
    static std::expected<SessionLock, std::string> Acquire(
        const std::filesystem::path& session_dir, const SessionLockOwner& owner);

    // 只读查看当前持有者(inspect 可并行;无锁文件给 nullopt)。
    static std::optional<SessionLockOwner> Inspect(const std::filesystem::path& session_dir);

    bool holds() const { return file_ != nullptr; }
    const std::filesystem::path& path() const { return path_; }

    // 显式释放;析构也会做。已释放再调是空操作。
    void Release();

private:
    std::filesystem::path path_;
    std::FILE* file_ = nullptr;
};

}  // namespace lubancode::trajectory
