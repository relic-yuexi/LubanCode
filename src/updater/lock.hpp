// 更新助手 C++ 化·批二第③单:安装根独占锁(§六:更新器对安装根持锁,
// 防两个更新器串账)。
//
// 语义真源 scripts/updater.py 的 InstallLock(L258-297)+ 锁范式样板
// src/gateway/process.hpp 的 GatewayLock,合流如下:
//   - 锁住在 updates/.lock;create-new 原子占位(wbx,OS 保证至多一只成功),
//     持有期间保持句柄,占位即持有——互斥不依赖"先读后写"的顺序;
//   - 账记 {pid, acquired_at_utc, start_token}:start_token 是可选字段
//     (python 版没有),读旧格式缺 token 按 pid 裁决;写侧带上它,持有者
//     死透与 PID 复用就分得开(GatewayLock 同款身份核);
//   - 撞上占位:读账 -> pid 是自己且 token 对得上 = 不可重入拒(python
//     "本进程已持有安装锁(不可重入)");pid 是自己但 token 对不上 = PID
//     复用,陈锁;pid 活着 = 活拒(探不到按活保守,IsProcessAlive 同口径);
//     pid 死透 = 陈锁,rename 成 .stale-<epoch秒> 再抢(断线恢复,python
//     同款;不直接删——留现场可查);
//   - 三轮清抢失败报错(python "安装锁竞争失败,稍后重试");
//   - release 只删 pid 是自己的锁(python 同款):句柄先关再删,账上 pid
//     不是自己就不动(锁被人接管过,删的是别人的锁)。
//
// 口径差(如实声明):python 对读不懂的坏锁也走"挪走再抢"(pid 取不到
// ->当死),本侧照 GatewayLock 的陈锁安全裁决保守拒绝(RefusedBrokenLock,
// 不动文件)——对手 create-new 与写账之间存在毫秒窗口,把活对手的半截
// 账挪走会让两只更新器同时持锁(POSIX 无句柄保护,窗口真实存在);保守
// 拒只是多退避一次,串账风险归零。真死透的半截账(持有者崩在写账上)
// 留人工删,发生面是微秒级窗口内的进程崩溃,可忽略。
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>

#include "updater/layout.hpp"

namespace lubancode::updater {

// 安装根独占锁:RAII,析构即 Release(幂等)。move-only。
class InstallRootLock {
public:
    struct AcquireResult {
        enum class Status {
            Acquired,             // 本进程拿到锁(含清掉陈锁后重拿)
            RefusedAliveHolder,   // 活进程持着:另一个更新器在跑
            RefusedReentrant,     // 本进程已持有(python:不可重入)
            RefusedBrokenLock,    // 锁文件在但读不懂:保守不动(GatewayLock 口径)
            ContentionFailure,    // 三轮清抢仍占不到位:稍后重试
            IoError,              // 建目录/写账失败
        };
        Status status = Status::IoError;
        unsigned long holder_pid = 0;   // RefusedAliveHolder/Reentrant 时 = 持有者
        std::string detail;
        bool cleared_stale = false;      // 本次取锁清过陈锁
        std::string stale_moved_to;      // 陈锁挪到了哪(.stale-<ts>,诊断/测试)
    };

    // 取锁次序:建 updates/ -> create-new 原子占位 -> 撞上则读账核身份
    // (活拒/重入拒/死清重试/坏锁保守拒)。三轮清抢失败 ContentionFailure。
    static AcquireResult TryAcquire(const LayoutPaths& paths, InstallRootLock* out);

    InstallRootLock() = default;
    ~InstallRootLock();
    InstallRootLock(const InstallRootLock&) = delete;
    InstallRootLock& operator=(const InstallRootLock&) = delete;
    InstallRootLock(InstallRootLock&& other) noexcept;
    InstallRootLock& operator=(InstallRootLock&& other) noexcept;

    bool holds() const { return !lock_file_.empty(); }

    // 放锁:只删 pid 是自己的锁(先关句柄再删;账上 pid 不是自己就不动)。
    // 幂等。
    void Release();

private:
    std::filesystem::path lock_file_;  // 空 = 未持锁
    std::FILE* file_ = nullptr;        // create-new 的原始句柄(占位即持有)
};

// 锁文件路径(updates/.lock)。
std::filesystem::path InstallLockPath(const LayoutPaths& paths);

}  // namespace lubancode::updater
