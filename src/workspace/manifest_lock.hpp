// SV-11(2026-09-21 架构审查):workspace.json 读改写的事务锁(跨进程)。
//
// 病灶:OpenOrRegisterWorkspace 先 ReadWorkspaceManifest、改 checkouts、
// 再 WriteWorkspaceManifestAtomic 整份重写——两个 linked worktree 按合同共
// workspace(同 identity_root 同房),两进程同时从同一旧账开张,A 读
// [main]、B 读 [main]、A 写 [main,A]、B 写 [main,B],四次操作都成功且
// JSON 完整,A 的登记凭空消失;last_opened/last_seen 也会回退。原子替换只
// 防半份文件,不保护读改写交错。
//
// 锁语义(与 memory::OwnerLock 同范式,按本场景裁;身份核不认锁龄):
//   - 锁目录 <workspace_dir>/.manifest.lock,owner 账(目录里 owner 文件:
//     PID + 进程起始 token + 随机 owner token)判持有者活死;活持有者不因
//     时间长被夺锁;
//   - 持有者死透/PID 被复用:整目录改名 .manifest.lock.stale-<ms> 隔离留证
//     再重建,不猜死后直接删;无 owner 的旧格式锁同样隔离明报;
//   - owner 在但读不懂:BrokenLock 明报,不敢动——调用方如实拒绝,不悄悄
//     覆盖旧账;
//   - 释放前重读 owner 核 pid + owner token,不是自己这只句柄拿的锁不删;
//   - 锁粒度 = 一间 workspace 房,不锁整个 workspaces 根(各项目互不阻塞)。
// 探测(HolderAlive)与取锁(TryAcquire)/释放(Release)共用同一份裁决;
// Acquire 在此之上做有界等待(开房是启动路径,烧完仍撞如实回绝,不无限等)。
// Windows 瞬态(防病毒/过滤驱动短拒)在 owner 读、隔离改名、释放删除里都
// 有界重试,烧完才算真失败。
//
// 注:进程起始 token 的探针(Windows GetProcessTimes / macOS sysctl /
// Linux /proc)与 trajectory/session_lock.cpp 同形,各持一份——workspace
// 层不许反向 include trajectory(trajectory 吃 workspace 的结果);收编进
// platform 共用件由集成者另立单子协调,不在本条范围。
#pragma once

#include <filesystem>
#include <string>

namespace lubancode::workspace {

// 锁目录:<workspace_dir>/.manifest.lock。开房路(OpenOrRegisterWorkspace)
// 与竞争靶子(tests/support/workspace_manifest_racer)共用这一枚,不各拼各的。
std::filesystem::path ManifestLockDir(const std::filesystem::path& workspace_dir);

class ManifestLock {
public:
    enum class Status {
        Acquired,         // 占住,owner 账已落盘
        HeldByLiveHolder, // 持有者活着(或在建窗口/探不出按活保守),拒绝
        BrokenLock,       // owner 在但读不懂:不敢动,明报
        IoError,          // 占位/隔离/写账真失败
    };
    struct Result {
        Status status = Status::IoError;
        std::string detail;  // 人话诊断(持有者 pid、隔离留证落点、失败原因)
    };

    // 单发取锁(对 workspace_dir 下的锁目录;目录不在就占位新建)。out 之前
    // 持有的锁先释放。陈锁隔离后对手可能先占,内部有界重试,撞满即报。
    static Result TryAcquire(const std::filesystem::path& workspace_dir, ManifestLock* out);
    // 有界等锁:活持有者放手要时间,attempts×interval_ms 烧完仍撞才回拒;
    // BrokenLock 是真拒绝,即刻回,不磨。
    static Result Acquire(const std::filesystem::path& workspace_dir, ManifestLock* out,
                          int attempts, int interval_ms);
    // 只读探测锁目录是否被持有(在建窗口/读不懂按活保守)。不创建、不删、
    // 不偷锁——与 TryAcquire 共用同一份身份裁决。
    static bool HolderAlive(const std::filesystem::path& workspace_dir, std::string* detail = nullptr);

    ManifestLock() = default;
    ~ManifestLock();
    ManifestLock(ManifestLock&& other) noexcept;
    ManifestLock& operator=(ManifestLock&& other) noexcept;
    ManifestLock(const ManifestLock&) = delete;
    ManifestLock& operator=(const ManifestLock&) = delete;

    bool holds() const { return !dir_.empty(); }
    const std::filesystem::path& dir() const { return dir_; }
    // 显式释放;析构也会做。只删 owner 账核得上的自己的锁。
    void Release();

private:
    std::filesystem::path dir_;
    std::FILE* file_ = nullptr;  // owner 的只读句柄:Windows 上挡他者删/改名
    std::string owner_token_;    // 本次占位的随机 token,释放核账用
};

}  // namespace lubancode::workspace
