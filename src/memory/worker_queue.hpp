// SV-09(2026-09-21 架构审查)拆出的记忆写队列与 worker 监督(内部件):
// pending 队列持久化、lifecycle 回执、回执追踪账(DrainWriteCompletions)、
// 后台 worker 池的拉起/合并唤醒/有界退避、任务台账(/memory jobs)与
// worker 主循环(RunPendingMemoryJobs)。home 与 executable 由门面构造时
// 递入(身份一份);授权闸与 job 材料拼装在 ProjectMemory 门面。

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "memory/project_memory.hpp"

namespace lubancode::memory::queue {

// 本会话排队后等回执的 job(DrainWriteCompletions 的账)。Enqueue 与
// Drain 可能落在不同线程(渠道会话),自带一把小锁;经 shared_ptr 间接
// 持有,不删 MemoryWriteQueue 的移动构造(与监督器同款手法)。
struct TrackedWrite {
    std::string job_id;
    std::string operation_id;
    std::string layer;  // project | user
    std::string title;
    std::filesystem::path lifecycle_root;  // 这笔 job 的回执落点(换区后仍准)
};
struct TrackedWriteStore {
    std::mutex mutex;
    std::vector<TrackedWrite> writes;
};

// worker 监督器的实现件(定义在 worker_queue.cpp)。
struct MemoryWorkerSupervisor;

// 进程级注册表口:同根(规范化 state_root)多份队列共用一只监督器,
// 同根多份 ProjectMemory 不再每次 enqueue 都拉一批争锁的子进程。
std::shared_ptr<MemoryWorkerSupervisor> SharedWorkerSupervisor(const std::filesystem::path& state_root);

// 盘上 pending 计数(跨工作区;监督器与 /doctor memory 共用)。
std::size_t CountPendingJobs(const std::filesystem::path& home);

// doctor 的监督器摘要(只读):各 state_root 最近一次事故一句话,只在
// 有事故时出声。
std::vector<std::string> MemorySupervisorDiagnostics();

// 一条队列 = 一份 home + executable + 共享监督器 + 回执追踪账。门面持
// shared_ptr 间接持有。
class MemoryWriteQueue {
public:
    MemoryWriteQueue(std::filesystem::path home_lubancode, std::string executable);

    // job 材料由门面拼好(memory_dir/workspace_key/正文齐备)递进来:
    // 落 pending → 回执立账 → 唤醒 worker。只有 queue 持久化失败才
    // unexpected;worker 启动成败另列字段,不污染 job_id。
    std::expected<MemoryEnqueueResult, std::string> PersistJobAndWake(const nlohmann::json& job,
                                                                      const std::string& title);

    // 有 pending job 时确保有一只后台 worker 在跑(合并唤醒/有界退避在
    // 监督器里)。会话启动/唤醒路(/memory jobs retry、session_stack)用。
    MemoryWorkerWake EnsureWorkerRunning() const;
    bool HasRunningWorker() const;                 // /memory jobs 的 worker 行与防风暴测试用
    std::uint64_t WorkerSpawnCount() const;        // 本进程累计拉起数(诊断/防风暴断言)
    void WaitForWorkersGracefully(int grace_ms) const;  // 退出收尾:有界等,超时不杀

    // 当前工作区的待写/失败任务清单(按 workspace_key 过滤,别区不算)。
    std::vector<ProjectMemory::MemoryJobInfo> ListWorkspaceJobs(const std::string& workspace_key) const;
    // 重试一笔 failed:校验工作区归属;原 job 已有 committed 回执的拒重放;
    // 否则按新 job 名重排回 pending,返回新 job_id。
    std::expected<std::string, std::string> RetryFailedJob(const std::string& workspace_key,
                                                           const std::string& job_id);

    // 空闲唤醒源用:有没有值得立刻收的账(回执已落地,或排的 job 还在
    // pending 却已无活 worker)。
    bool WakeNeededForWrites() const;
    // 收一次账:已落地的回执折成完成通知并销账;还在 pending 且无活
    // worker 的,过一次 EnsureWorkerRunning。
    std::vector<ProjectMemory::MemoryWriteCompletion> DrainWriteCompletions();

private:
    std::filesystem::path home_;
    std::string executable_;
    std::shared_ptr<MemoryWorkerSupervisor> supervisor_;
    std::shared_ptr<TrackedWriteStore> tracked_writes_;
};

}  // namespace lubancode::memory::queue
