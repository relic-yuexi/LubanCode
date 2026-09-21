// worker_queue.hpp 的实现。SV-09(2026-09-21 架构审查)拆分前住
// project_memory.cpp,搬来逻辑一字未动:监督器/注册表/lifecycle 回执/
// ProcessJob/RunPendingMemoryJobs 原样;EnqueueJob 的落盘-立账-唤醒尾巴
// 折进 PersistJobAndWake,台账/重试/收账方法改从队列自身的 home/executable/
// 监督器/追踪账取数(与原 ProjectMemory 成员同一份来源)。

#include "memory/worker_queue.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <sstream>
#include <thread>

#include "memory/internal.hpp"
#include "memory/topic_store.hpp"  // store::ProcessUpsert/Forget/Verify
#include "platform/process.hpp"
#include "trajectory/safety.hpp"   // P0-4:全局目录 user-only 收紧

namespace lubancode::memory::queue {

namespace {

namespace fs = std::filesystem;

// 只读探测 worker.lock 是否被持有(SV-01):与 OwnerLock::TryAcquire 共用
// 同一份身份裁决——持有者活着就在,不认 mtime 年龄,活 worker 干满三十秒
// 也不会被判死。不创建、不删、不偷锁。
bool WorkerLockHeld(const fs::path& home) {
    std::string detail;
    return OwnerLock::HolderAlive(AbsoluteNormal(home) / "memory-jobs" / "worker.lock", &detail);
}

// ---------------------------------------------------------------------------
// P0-3(§6.3/合同 §四):异步 worker 的提交回执。与 trajectory 的
// WorkspaceLifecycle 同形(lifecycle/<operation_id>/{intent.json,result.json},
// schema_version 1,operation="memory_save"),memory 侧自写不引 trajectory 头
//——那份文件 P0-2 正在动,接缝处能不碰就不碰。result 只许写一次(已存在
// 即拒),历史结果不改写。
// 修复单 §五 C:落点由调用方递 lifecycle_root——workspace job 用
// <workspace>/lifecycle/,用户层 job 用 <memory/user>/.state/lifecycle/
//(同形回执,用户层的等价完成依据);result 带 workspace_key,收执侧
// "核对 operation_id/workspace/outcome"三件都能对上。
// ---------------------------------------------------------------------------
fs::path LifecycleRootForMemoryDir(const fs::path& memory_dir, const fs::path& home_lubancode) {
    if (memory_dir.empty()) return {};
    const bool user_job = IsWithin(memory_dir, AbsoluteNormal(home_lubancode) / "memory" / "user");
    const bool workspace_job = IsWithin(memory_dir, AbsoluteNormal(home_lubancode) / "workspaces");
    if (workspace_job) return memory_dir.parent_path() / "lifecycle";
    if (user_job) return memory_dir / ".state" / "lifecycle";
    return {};
}

std::expected<void, std::string> WriteMemorySaveIntent(const fs::path& lifecycle_root,
                                                       const std::string& operation_id,
                                                       const nlohmann::json& job) {
    const fs::path intent_path = lifecycle_root / Utf8Path(operation_id) / "intent.json";
    std::error_code ec;
    if (fs::exists(intent_path, ec)) {
        return {};  // 崩溃续跑:同 operation_id 只写一次,不覆盖历史意图
    }
    nlohmann::json intent{
        {"schema_version", 1},
        {"operation_id", operation_id},
        {"operation", "memory_save"},
        {"workspace_key", job.value("workspace_key", std::string())},
        {"session_id", std::string()},
        {"requested_at_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count()},
        {"parameters",
         nlohmann::json{{"job_operation", job.value("operation", std::string())},
                        {"memory_dir", job.value("memory_dir", std::string())},
                        {"source_event_ref", job.value("source_event_ref", std::string())},
                        {"memory_id", job.value("id", std::string())},
                        {"title", job.value("title", std::string())}}},
    };
    return AtomicWrite(intent_path, intent.dump(2) + "\n");
}

std::expected<void, std::string> WriteMemorySaveResult(const fs::path& lifecycle_root,
                                                       const std::string& operation_id,
                                                       const nlohmann::json& outcome,
                                                       const std::string& workspace_key) {
    const fs::path result_path = lifecycle_root / Utf8Path(operation_id) / "result.json";
    std::error_code ec;
    if (fs::exists(result_path, ec)) {
        return std::unexpected("lifecycle.result_exists: memory save 回执已存在: " + operation_id);
    }
    nlohmann::json result{
        {"schema_version", 1},
        {"operation_id", operation_id},
        {"status", "completed"},
        {"workspace_key", workspace_key},
        {"completed_at_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count()},
        {"outcome", outcome},
    };
    return AtomicWrite(result_path, result.dump(2) + "\n");
}

// 回执读取(收执侧/重试侧共用):committed = outcome 有 committed_at 且无
// stable_error_code;failed = outcome 带 stable_error_code。status 恒为
// completed(指这枚回执落齐了,不是指业务成功)。
struct MemorySaveReceipt {
    bool exists = false;
    bool committed = false;
    std::string error;
    std::string memory_id;
    std::string workspace_key;
};
MemorySaveReceipt ReadMemorySaveReceipt(const fs::path& lifecycle_root, const std::string& operation_id) {
    MemorySaveReceipt receipt;
    const fs::path result_path = lifecycle_root / Utf8Path(operation_id) / "result.json";
    std::error_code ec;
    if (!fs::exists(result_path, ec)) return receipt;
    nlohmann::json result;
    try {
        result = nlohmann::json::parse(ReadBounded(result_path, 64 * 1024));
    } catch (const nlohmann::json::exception&) {
        return receipt;  // 在但读不出:当作没有,由坏账路径(重试)处置
    }
    if (!result.is_object()) return receipt;
    receipt.exists = true;
    receipt.workspace_key = result.value("workspace_key", std::string());
    const nlohmann::json outcome =
        result.contains("outcome") && result["outcome"].is_object() ? result["outcome"] : nlohmann::json::object();
    if (outcome.contains("stable_error_code")) {
        receipt.error = outcome.value("error", outcome.value("stable_error_code", std::string("unknown")));
        return receipt;
    }
    if (outcome.contains("committed_at")) {
        receipt.committed = true;
        receipt.memory_id = outcome.value("memory_id", std::string());
    }
    return receipt;
}

// ---- /memory jobs 的台账小工具 ----

// 等待时长(按文件 mtime 折人话):"45s"、"3m12s"、"2h05m"、"4d03h"。
std::string WaitHintForFile(const fs::path& file) {
    std::error_code ec;
    const auto modified = fs::last_write_time(file, ec);
    if (ec) return {};
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::file_clock::now() - modified);
    auto total = elapsed.count();
    if (total < 0) return {};
    const auto seconds = static_cast<std::uint64_t>(total);
    std::ostringstream out;
    const auto two = [](std::uint64_t value) {
        std::ostringstream digits;
        if (value < 10) digits << '0';
        digits << value;
        return digits.str();
    };
    if (seconds < 60) {
        out << seconds << "s";
    } else if (seconds < 3600) {
        out << seconds / 60 << "m" << two(seconds % 60) << "s";
    } else if (seconds < 86400) {
        out << seconds / 3600 << "h" << two((seconds / 60) % 60) << "m";
    } else {
        out << seconds / 86400 << "d" << two((seconds / 3600) % 24) << "h";
    }
    return out.str();
}

// 文件首行(截到 max_bytes;读不出给空串)。
std::string FirstLineBounded(const fs::path& file, std::size_t max_bytes) {
    const std::string text = ReadBounded(file, max_bytes);
    const std::size_t newline = text.find('\n');
    std::string line = newline == std::string::npos ? text : text.substr(0, newline);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

// failed 文件名回原 job stem:JobStamp 是 "<millis>-<seq>"(无点),撞名
// 后缀是 ".<stamp>",一律截到第一个 '.'。
std::string BaseJobStem(const std::string& failed_name) {
    const std::size_t dot = failed_name.find('.');
    return dot == std::string::npos ? failed_name : failed_name.substr(0, dot);
}

}  // namespace

// ---------------------------------------------------------------------------
// worker 监督器(修复单 §五 A):按规范化 state_root 进程级共享。职责:
//   - 留住每只 worker 的可等待句柄(Windows 每任务 Job 带
//     KILL_ON_JOB_CLOSE,句柄早释即杀 worker——#124 起池子持有);
//   - EnsureRunning 串行协调:活 worker 尚在且握着 worker.lock 时合并唤醒,
//     不再起一批争锁的子进程;已退出时先收退出码再看 pending;
//   - worker 带着未消费 pending 退出 → 记 worker_exited_with_pending(带
//     退出码与日志路径),有界退避(1s→30s 封顶)重试,不忙循环。
// 跨宿主竞争仍由 worker.lock 承接(单宿主管理器不能替代跨进程锁)。
// ---------------------------------------------------------------------------

struct MemoryWorkerSupervisor {
    struct Worker {
        std::shared_ptr<platform::BackgroundProcessHandle> handle;
        std::string log_path;
        std::chrono::steady_clock::time_point started_at;
    };
    struct Snapshot {
        std::size_t alive = 0;
        std::uint64_t spawn_count = 0;
        bool last_exit_known = false;
        int last_exit_code = 0;
        std::string last_log_path;
        std::string last_incident;
        std::chrono::steady_clock::time_point next_retry_at{};
    };

    // 合并唤醒窗口:窗口内刚拉过就不再拉(连续 enqueue 不起风暴)。
    static constexpr auto kCoalesceWindow = std::chrono::milliseconds(200);
    // 退避封顶:连击 6 次后固定 30s,不再加密,也不放弃。
    static constexpr int kBackoffCapStreak = 6;

    static std::chrono::milliseconds BackoffDuration(int streak) {
        if (streak <= 1) return std::chrono::milliseconds(1000);
        const int shift = std::min(streak - 1, kBackoffCapStreak - 1);
        return std::chrono::milliseconds(std::min(1000 << shift, 30000));
    }

    MemoryWorkerWake EnsureRunning(const fs::path& home, const std::string& executable) {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        // 1) 收割已退出的 worker:拿退出码与日志。带着 pending 的**非干净**
        //    退出(提前退出/崩溃,退出码非零或未知)才是事故:记
        //    worker_exited_with_pending 并推进有界退避。干净退出(0)后盘上
        //    又有新 pending,是"worker 扫空退出、新任务恰好进来"的常态,
        //    下面第 3 步直接补拉,不记退避。
        std::vector<Worker> survivors;
        survivors.reserve(alive.size());
        for (auto& worker : alive) {
            if (!worker.handle->Wait(0)) {
                survivors.push_back(std::move(worker));
                continue;
            }
            const auto completion = worker.handle->Peek();
            last_exit_known = completion.known;
            last_exit_code = completion.exit_code;
            last_log_path = worker.log_path;
            const bool unclean = !completion.known || completion.exit_code != 0;
            if (unclean && CountPendingJobs(home) > 0) {
                ++incident_streak;
                const auto backoff = BackoffDuration(incident_streak);
                next_retry_at = now + backoff;
                last_incident =
                    "worker_exited_with_pending: exit_code=" +
                    (completion.known ? std::to_string(completion.exit_code) : std::string("unknown")) +
                    ", log=" + (worker.log_path.empty() ? std::string("none") : worker.log_path) +
                    ", backoff_ms=" + std::to_string(backoff.count()) +
                    (incident_streak >= kBackoffCapStreak ? "(已达上限间隔,不再加密;请查日志)" : "");
            }
        }
        alive = std::move(survivors);

        // 2) 盘上没活干:不拉进程,顺手清退避(pending 归零说明上一轮真跑完了)。
        if (CountPendingJobs(home) == 0) {
            incident_streak = 0;
            next_retry_at = {};
            MemoryWorkerWake wake;
            wake.state = alive.empty() ? MemoryWorkerLaunchState::Idle
                                       : MemoryWorkerLaunchState::AlreadyRunning;
            return wake;
        }

        // 3) 盘上有活。
        if (executable.empty()) {
            MemoryWorkerWake wake;
            wake.state = MemoryWorkerLaunchState::Unavailable;
            wake.error_code = "worker_unavailable";
            wake.error = "未配置记忆 worker 可执行文件";
            return wake;
        }
        if (now < next_retry_at) {
            // 有界退避中:不拉,也不算失败到底——job 都在 pending,下一个
            // 观察点(入队/状态页/空闲唤醒)自然再试。
            MemoryWorkerWake wake;
            wake.state = MemoryWorkerLaunchState::StartFailed;
            wake.error_code = "worker_retry_backoff";
            wake.error = "worker 带待写任务退出,有界退避 "
                       + std::to_string(
                              std::chrono::duration_cast<std::chrono::milliseconds>(next_retry_at - now)
                                  .count())
                       + "ms 后再试";
            return wake;
        }
        if (!alive.empty() && WorkerLockHeld(home)) {
            // 活 worker 握着锁:它正在扫队列,这次唤醒并进去(合并唤醒)。
            MemoryWorkerWake wake;
            wake.state = MemoryWorkerLaunchState::AlreadyRunning;
            return wake;
        }
        if (!alive.empty()) {
            // 活 worker 没握锁:要么刚扫空正要退出(空窗),要么锁是别家宿主
            // 的——再拉一只合并唤醒。窗口内刚拉过的不再拉。
            const bool fresh = std::any_of(alive.begin(), alive.end(), [&](const Worker& worker) {
                return now - worker.started_at < kCoalesceWindow;
            });
            if (fresh) {
                MemoryWorkerWake wake;
                wake.state = MemoryWorkerLaunchState::AlreadyRunning;
                return wake;
            }
        }
        const auto spawned =
            platform::RunProcessBackground({executable, "--memory-worker", PathUtf8(home)});
        if (!spawned.success) {
            ++incident_streak;
            const auto backoff = BackoffDuration(incident_streak);
            next_retry_at = now + backoff;
            last_incident = "worker_start_failed: " + spawned.error + ", backoff_ms=" +
                            std::to_string(backoff.count());
            MemoryWorkerWake wake;
            wake.state = MemoryWorkerLaunchState::StartFailed;
            wake.error_code = "worker_start_failed";
            wake.error = spawned.error;
            return wake;
        }
        Worker worker;
        worker.handle = spawned.handle;
        worker.log_path = spawned.log_path;
        worker.started_at = now;
        alive.push_back(std::move(worker));
        ++spawn_count;
        MemoryWorkerWake wake;
        wake.state = MemoryWorkerLaunchState::Started;
        return wake;
    }

    // 只读快照(不收割、不拉起):/memory jobs、doctor、空闲唤醒判定用。
    Snapshot TakeSnapshot() {
        std::lock_guard lock(mutex_);
        Snapshot snapshot;
        snapshot.spawn_count = spawn_count;
        snapshot.last_exit_known = last_exit_known;
        snapshot.last_exit_code = last_exit_code;
        snapshot.last_log_path = last_log_path;
        snapshot.last_incident = last_incident;
        snapshot.next_retry_at = next_retry_at;
        for (const auto& worker : alive) {
            if (!worker.handle->Wait(0)) ++snapshot.alive;
        }
        return snapshot;
    }

    // 退场收尾:有界等活 worker 自己跑完,超时不杀(pending 仍在盘上,
    // 下次会话恢复)。等在锁外,不挡别处的 EnsureRunning。
    void WaitForGrace(int grace_ms) {
        std::vector<std::shared_ptr<platform::BackgroundProcessHandle>> handles;
        {
            std::lock_guard lock(mutex_);
            handles.reserve(alive.size());
            for (const auto& worker : alive) handles.push_back(worker.handle);
        }
        for (const auto& handle : handles) {
            if (handle != nullptr) (void)handle->Wait(grace_ms);
        }
    }

    fs::path state_root;
    std::mutex mutex_;
    std::vector<Worker> alive;
    std::uint64_t spawn_count = 0;
    bool last_exit_known = false;
    int last_exit_code = 0;
    std::string last_log_path;
    std::string last_incident;  // 最近一次 worker_exited_with_pending/start_failed 的人话
    int incident_streak = 0;    // 连击次数(退避指数),pending 清零时复位
    std::chrono::steady_clock::time_point next_retry_at{};
};

// 进程级注册表:同根(规范化 state_root)多份 ProjectMemory 共用一只监督器。
// shared_ptr 落进静态表,ProjectMemory 析构/移动/替换都不放掉仍承载写入的
// worker 句柄;进程退出时句柄随全局析构关闭(KILL_ON_JOB_CLOSE 收尾)。
std::map<std::string, std::shared_ptr<MemoryWorkerSupervisor>>& SupervisorRegistry() {
    static std::map<std::string, std::shared_ptr<MemoryWorkerSupervisor>> registry;
    return registry;
}

std::shared_ptr<MemoryWorkerSupervisor> SharedWorkerSupervisor(const fs::path& state_root) {
    static std::mutex registry_mutex;
    const std::string key = PathUtf8(AbsoluteNormal(state_root));
    std::lock_guard lock(registry_mutex);
    auto& registry = SupervisorRegistry();
    auto found = registry.find(key);
    if (found != registry.end()) return found->second;
    auto supervisor = std::make_shared<MemoryWorkerSupervisor>();
    supervisor->state_root = AbsoluteNormal(state_root);
    registry.emplace(key, supervisor);
    return supervisor;
}

std::size_t CountPendingJobs(const fs::path& home) {
    std::error_code ec;
    fs::directory_iterator it(AbsoluteNormal(home) / "memory-jobs" / "pending", ec);
    if (ec) return 0;
    std::size_t count = 0;
    for (const auto& item : it) {
        std::error_code item_ec;
        if (item.is_regular_file(item_ec) && item.path().extension() == ".json") ++count;
    }
    return count;
}

std::vector<std::string> MemorySupervisorDiagnostics() {
    static std::mutex registry_mutex;
    std::lock_guard lock(registry_mutex);
    std::vector<std::string> lines;
    for (const auto& [key, supervisor] : SupervisorRegistry()) {
        const auto snapshot = supervisor->TakeSnapshot();
        if (snapshot.last_incident.empty()) continue;
        lines.push_back("[! ] memory worker(" + key + "): " + snapshot.last_incident);
    }
    return lines;
}

// ---------------------------------------------------------------------------
// 一笔 job 的 worker 侧处理(ProcessJob)与队列主循环(RunPendingMemoryJobs)。
// ---------------------------------------------------------------------------

// 坏 job 挪 failed,同址留 .error.txt 回执。
void MoveFailedJob(const fs::path& job_path, const fs::path& failed_dir, const std::string& error) {
    std::error_code ec;
    fs::create_directories(failed_dir, ec);
    fs::path destination = failed_dir / job_path.filename();
    if (fs::exists(destination, ec)) destination += "." + JobStamp();
    fs::rename(job_path, destination, ec);
    if (!ec) {
        const auto ignored = AtomicWrite(fs::path(PathUtf8(destination) + ".error.txt"), error + "\n");
        (void)ignored;
    }
}

std::expected<void, std::string> ProcessJob(const fs::path& job_path,
                                            const fs::path& home_lubancode) {
    nlohmann::json job;
    try {
        job = nlohmann::json::parse(ReadFile(job_path));
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected("job 不是合法 JSON: " + std::string(e.what()));
    }
    if (!job.is_object() || job.value("schema", 0) != 1) {
        return std::unexpected("job schema 不受支持");
    }
    const fs::path memory_dir = Utf8Path(job.value("memory_dir", std::string()));
    const fs::path project_root = Utf8Path(job.value("project_root", std::string()));
    // P0-3:job 的落点只认两处——某 workspace 的 <workspace>/memory/,或
    // 用户级 memory/user/。指旧 <home>/projects/ 的存量 job 拒办挪 failed
    //(迁移归 P0-5),生产路径不再往旧树写一个字节。
    const bool user_job = IsWithin(memory_dir, home_lubancode / "memory" / "user");
    const bool workspace_job = IsWithin(memory_dir, home_lubancode / "workspaces");
    if (memory_dir.empty() || (!workspace_job && !user_job)) {
        return std::unexpected("memory.job_failed: job 的 memory_dir 越出 workspace/用户记忆根: " +
                               PathUtf8(memory_dir));
    }

    // 提交回执(修复单 §五 C):workspace job 进 <workspace>/lifecycle/,
    // 用户层 job 进 <memory/user>/.state/lifecycle/——同形回执,用户层
    // 从此也有等价完成依据("已入库"只认它,pending 消失不算数)。
    // intent 先行,result 只写一次;operation_id 用 job 文件名(时间戳+
    // 序号,天然唯一)。
    const fs::path lifecycle_root = LifecycleRootForMemoryDir(memory_dir, home_lubancode);
    const std::string operation_id =
        "memsave-" + PathUtf8(job_path.filename().replace_extension());
    if (!lifecycle_root.empty()) {
        const auto existing = ReadMemorySaveReceipt(lifecycle_root, operation_id);
        if (existing.committed) {
            return {};  // 已提交过:幂等续跑,不重复动盘(§11.3 重复 commit 幂等)
        }
        if (existing.exists) {
            // 已有失败回执:历史结果不改写,这份 pending 是崩溃残留——
            // 挪 failed 交显式重试(新 job 名 = 新 operation_id)。
            return std::unexpected("memory.job_receipt_failed: 该 job 已有失败回执(" +
                                   operation_id + ");用 /memory jobs retry 换新单重排");
        }
        auto intent = WriteMemorySaveIntent(lifecycle_root, operation_id, job);
        if (!intent.has_value()) return std::unexpected(intent.error());
    }

    // SV-01:项目层锁走所有权裁决——活持有者不因年龄被夺;死锁/旧格式锁
    // 隔离后接手;owner 读不懂明报不动。跨工作区的用户层 job 会撞同一把
    // 用户层锁,有界等一等对手放手,烧完仍撞才按失败归档,不叫偶发争用
    // 污染 failed 账。
    OwnerLock project_lock;
    const auto lock_result = AcquireDirLockWithRetry(memory_dir / ".state" / "memory.lock",
                                                     &project_lock, 20, 100);
    if (lock_result.status != OwnerLock::Status::Acquired) {
        return std::unexpected(ProjectLockRefusal(lock_result, "项目记忆"));
    }
    // P0-4:全局层写后复紧 user-only(目录可能刚建出来)。
    if (user_job) {
        (void)trajectory::HardenDirectoryUserOnly(memory_dir);
    }

    const std::string operation = job.value("operation", std::string());
    std::expected<store::MemoryWriteOutcome, std::string> upsert;
    std::expected<void, std::string> result;
    if (operation == "upsert") {
        upsert = store::ProcessUpsert(job, memory_dir, project_root);
        result = upsert.has_value() ? std::expected<void, std::string>{}
                                    : std::unexpected(upsert.error());
    } else if (operation == "forget") {
        result = store::ProcessForget(job, memory_dir);
    } else if (operation == "verify") {
        result = store::ProcessVerify(job, memory_dir, project_root);
    } else if (operation == "rebuild") {
        result = RebuildMemoryIndex(memory_dir);
    } else {
        result = std::unexpected("不认得的 memory job operation: " + operation);
    }

    if (!lifecycle_root.empty()) {
        nlohmann::json outcome;
        if (result.has_value()) {
            // 合同 §四 memory.save.committed 的四件套(upsert 有正文指纹,
            // 其余操作只有 id/时刻)。
            outcome["memory_id"] = job.value("id", std::string());
            if (upsert.has_value()) {
                outcome["memory_id"] = upsert->memory_id;
                outcome["memory_version"] = upsert->committed_at;
                outcome["content_sha256"] = upsert->content_sha256;
                outcome["memory_path"] = upsert->memory_path;
            }
            outcome["committed_at"] = NowIsoUtc();
        } else {
            outcome["stable_error_code"] = "memory.save_failed";
            outcome["retryable"] = true;
            outcome["error"] = result.error();
        }
        auto receipt = WriteMemorySaveResult(lifecycle_root, operation_id, outcome,
                                             job.value("workspace_key", std::string()));
        if (!receipt.has_value()) {
            return std::unexpected("memory.job_failed: " + receipt.error());
        }
    }
    return result;
}

MemoryWriteQueue::MemoryWriteQueue(fs::path home_lubancode, std::string executable)
    : home_(std::move(home_lubancode)),
      executable_(std::move(executable)),
      supervisor_(SharedWorkerSupervisor(home_)),
      tracked_writes_(std::make_shared<TrackedWriteStore>()) {}

std::expected<MemoryEnqueueResult, std::string> MemoryWriteQueue::PersistJobAndWake(
    const nlohmann::json& job, const std::string& title) {
    const fs::path pending = home_ / "memory-jobs" / "pending";
    const std::string job_name = JobStamp() + ".json";
    auto written = AtomicWrite(pending / Utf8Path(job_name), job.dump(2) + "\n");
    if (!written.has_value()) return std::unexpected(written.error());  // queue 持久化失败 = 未入队
    MemoryEnqueueResult result;
    result.job_id = job_name;
    result.queue_state = MemoryQueueState::Persisted;
    // 回执记账(修复单 §五 C):这笔 job 的"已入库"只认 lifecycle 回执,
    // 排队当刻先立账,等 DrainWriteCompletions 收。
    {
        const fs::path target_memory_dir = Utf8Path(job.value("memory_dir", std::string()));
        const fs::path lifecycle_root = LifecycleRootForMemoryDir(target_memory_dir, home_);
        if (!lifecycle_root.empty()) {
            std::string stem = job_name;
            if (stem.ends_with(".json")) stem.resize(stem.size() - 5);
            TrackedWrite tracked;
            tracked.job_id = job_name;
            tracked.operation_id = "memsave-" + stem;
            tracked.layer = IsWithin(target_memory_dir, home_ / "memory" / "user") ? "user"
                                                                                    : "project";
            tracked.title = title;
            tracked.lifecycle_root = lifecycle_root;
            std::lock_guard lock(tracked_writes_->mutex);
            tracked_writes_->writes.push_back(std::move(tracked));
        }
    }
    // 唤醒 worker(监督器内含合并唤醒/有界退避):启动成败另列字段,
    // 已排队与未启动两个事实都保留,不互相污染。
    const MemoryWorkerWake wake = supervisor_->EnsureRunning(home_, executable_);
    result.worker_state = wake.state;
    result.worker_error_code = wake.error_code;
    result.worker_error = wake.error;
    return result;
}

MemoryWorkerWake MemoryWriteQueue::EnsureWorkerRunning() const {
    return supervisor_->EnsureRunning(home_, executable_);
}

bool MemoryWriteQueue::HasRunningWorker() const { return supervisor_->TakeSnapshot().alive > 0; }

std::uint64_t MemoryWriteQueue::WorkerSpawnCount() const {
    return supervisor_->TakeSnapshot().spawn_count;
}

void MemoryWriteQueue::WaitForWorkersGracefully(int grace_ms) const {
    supervisor_->WaitForGrace(grace_ms);
}

std::vector<ProjectMemory::MemoryJobInfo> MemoryWriteQueue::ListWorkspaceJobs(
    const std::string& workspace_key) const {
    std::vector<ProjectMemory::MemoryJobInfo> out;
    const fs::path jobs_root = home_ / "memory-jobs";
    const auto snapshot = supervisor_->TakeSnapshot();
    std::string worker_state;
    if (snapshot.alive > 0) {
        worker_state = "running";
    } else if (snapshot.last_exit_known) {
        worker_state = "exited(code=" + std::to_string(snapshot.last_exit_code) + ")";
    } else {
        worker_state = "none";
    }
    const auto collect = [&](const char* state_name, const fs::path& dir, bool with_error) {
        std::error_code ec;
        fs::directory_iterator it(dir, ec);
        if (ec) return;
        for (const auto& item : it) {
            std::error_code item_ec;
            if (!item.is_regular_file(item_ec) || item.path().extension() != ".json") continue;
            nlohmann::json job;
            try {
                job = nlohmann::json::parse(ReadBounded(item.path(), 256 * 1024));
            } catch (const nlohmann::json::exception&) {
                continue;  // 坏 job 不进台账(worker 会挪 failed),状态页不数它
            }
            if (!job.is_object() || job.value("workspace_key", std::string()) != workspace_key) {
                continue;  // 工作区隔离:别区的数不进来
            }
            ProjectMemory::MemoryJobInfo info;
            info.job_id = PathUtf8(item.path().filename());
            info.state = state_name;
            info.operation = job.value("operation", std::string());
            info.title = job.value("title", std::string());
            const fs::path memory_dir = Utf8Path(job.value("memory_dir", std::string()));
            info.layer = IsWithin(memory_dir, home_ / "memory" / "user") ? "user" : "project";
            info.created_at = job.value("created_at", std::string());
            info.wait_hint = WaitHintForFile(item.path());
            if (with_error) {
                const fs::path error_path = fs::path(PathUtf8(item.path()) + ".error.txt");
                info.error = FirstLineBounded(error_path, 300);
            }
            info.worker_state = worker_state;
            info.worker_log = snapshot.last_log_path;
            out.push_back(std::move(info));
        }
    };
    collect("pending", jobs_root / "pending", /*with_error=*/false);
    collect("failed", jobs_root / "failed", /*with_error=*/true);
    std::sort(out.begin(), out.end(),
              [](const ProjectMemory::MemoryJobInfo& a, const ProjectMemory::MemoryJobInfo& b) {
                  if (a.created_at != b.created_at) return a.created_at < b.created_at;
                  return a.job_id < b.job_id;
              });
    return out;
}

std::expected<std::string, std::string> MemoryWriteQueue::RetryFailedJob(
    const std::string& workspace_key, const std::string& job_id) {
    const fs::path jobs_root = home_ / "memory-jobs";
    std::string name = job_id;
    if (!name.ends_with(".json")) name += ".json";
    const fs::path source = jobs_root / "failed" / Utf8Path(name);
    std::error_code ec;
    if (!fs::is_regular_file(source, ec)) {
        return std::unexpected("找不到失败任务: " + job_id + "(/memory jobs 先看名单)");
    }
    nlohmann::json job;
    try {
        job = nlohmann::json::parse(ReadBounded(source, 256 * 1024));
    } catch (const nlohmann::json::exception& e) {
        return std::unexpected("failed job 不是合法 JSON: " + std::string(e.what()));
    }
    if (!job.is_object()) return std::unexpected("failed job 形状不对(不是 object)");
    if (job.value("workspace_key", std::string()) != workspace_key) {
        return std::unexpected("该任务不属于当前工作区;查询与重试都按工作区隔离");
    }
    // 防重放:原 job 的回执若已是 committed,说明提交真发生过(失败出在
    // 后续环节),不再跑一遍——那只会重复写正文。
    const std::string base_stem = BaseJobStem(name);
    const fs::path memory_dir = Utf8Path(job.value("memory_dir", std::string()));
    const fs::path lifecycle_root = LifecycleRootForMemoryDir(memory_dir, home_);
    if (!lifecycle_root.empty()) {
        const auto receipt = ReadMemorySaveReceipt(lifecycle_root, "memsave-" + base_stem);
        if (receipt.committed) {
            return std::unexpected("原任务已提交过(回执 memsave-" + base_stem +
                                   "),不重放;请 /memory list 核对结果");
        }
    }
    // 新 job 名重排:全新 operation_id、全新 lifecycle 账,历史回执与
    // failed 台账都不回改。
    job["retried_from"] = base_stem;
    const std::string new_name = JobStamp() + ".json";
    auto written = AtomicWrite(jobs_root / "pending" / Utf8Path(new_name), job.dump(2) + "\n");
    if (!written.has_value()) return std::unexpected(written.error());
    fs::remove(source, ec);
    fs::remove(fs::path(PathUtf8(source) + ".error.txt"), ec);
    // 回执记账与唤醒(与首次入队同一条路)。
    if (!lifecycle_root.empty()) {
        TrackedWrite tracked;
        tracked.job_id = new_name;
        tracked.operation_id = "memsave-" + BaseJobStem(new_name);
        tracked.layer = IsWithin(memory_dir, home_ / "memory" / "user") ? "user" : "project";
        tracked.title = job.value("title", std::string());
        tracked.lifecycle_root = lifecycle_root;
        std::lock_guard lock(tracked_writes_->mutex);
        tracked_writes_->writes.push_back(std::move(tracked));
    }
    (void)EnsureWorkerRunning();
    return new_name;
}

bool MemoryWriteQueue::WakeNeededForWrites() const {
    std::vector<TrackedWrite> tracked;
    {
        std::lock_guard lock(tracked_writes_->mutex);
        tracked = tracked_writes_->writes;
    }
    if (tracked.empty()) return false;
    const fs::path jobs_root = home_ / "memory-jobs";
    std::error_code ec;
    for (const auto& item : tracked) {
        // 回执已落地:收账去。
        if (fs::exists(item.lifecycle_root / Utf8Path(item.operation_id) / "result.json", ec)) {
            return true;
        }
    }
    // 排的 job 还在 pending 却已无活 worker:该补拉了(退避由监督器挡,
    // 退避未到时这里的快照也标着 next_retry_at,不到点不醒,不空转)。
    if (executable_.empty()) return false;  // 没配 exe 的形态不追这个唤醒
    const auto snapshot = supervisor_->TakeSnapshot();
    if (snapshot.alive > 0) return false;
    if (std::chrono::steady_clock::now() < snapshot.next_retry_at) return false;
    for (const auto& item : tracked) {
        if (fs::is_regular_file(jobs_root / "pending" / Utf8Path(item.job_id), ec)) return true;
    }
    return false;
}

std::vector<ProjectMemory::MemoryWriteCompletion> MemoryWriteQueue::DrainWriteCompletions() {
    std::vector<TrackedWrite> tracked;
    {
        std::lock_guard lock(tracked_writes_->mutex);
        tracked.swap(tracked_writes_->writes);
    }
    std::vector<ProjectMemory::MemoryWriteCompletion> out;
    std::vector<TrackedWrite> keep;
    const fs::path jobs_root = home_ / "memory-jobs";
    bool stalled_in_flight = false;
    for (auto& item : tracked) {
        const fs::path result_path =
            item.lifecycle_root / Utf8Path(item.operation_id) / "result.json";
        std::error_code ec;
        if (fs::exists(result_path, ec)) {
            ProjectMemory::MemoryWriteCompletion completion;
            completion.job_id = item.job_id;
            completion.operation_id = item.operation_id;
            completion.layer = item.layer;
            completion.title = item.title;
            nlohmann::json result;
            try {
                result = nlohmann::json::parse(ReadBounded(result_path, 64 * 1024));
            } catch (const nlohmann::json::exception&) {
                completion.outcome = "failed";
                completion.error = "提交回执损坏: " + PathUtf8(result_path);
                out.push_back(std::move(completion));
                continue;  // 坏回执不回炉重收
            }
            // 核对 operation_id(目录绑定 + 内容对上)与 workspace(落点
            // 目录即工作区边界;result 里也带 workspace_key,同根才认)。
            if (!result.is_object() ||
                result.value("operation_id", std::string()) != item.operation_id) {
                completion.outcome = "failed";
                completion.error = "回执 operation_id 对不上,弃收";
                out.push_back(std::move(completion));
                continue;
            }
            const nlohmann::json outcome = result.contains("outcome") && result["outcome"].is_object()
                                               ? result["outcome"]
                                               : nlohmann::json::object();
            if (outcome.contains("stable_error_code")) {
                completion.outcome = "failed";
                completion.error = outcome.value("error", outcome.value("stable_error_code", std::string("unknown")));
            } else if (outcome.contains("committed_at")) {
                completion.outcome = "committed";
                completion.memory_id = outcome.value("memory_id", std::string());
            } else {
                completion.outcome = "failed";
                completion.error = "回执无结局字段,弃收";
            }
            out.push_back(std::move(completion));
            continue;
        }
        // 回执没落地:pending 还在 = 在途(留着,顺带补拉 worker);
        // 不在 pending、也不在 failed = 去向不明——不按 pending 消失猜成功。
        if (fs::is_regular_file(jobs_root / "pending" / Utf8Path(item.job_id), ec)) {
            stalled_in_flight = true;
            keep.push_back(std::move(item));
            continue;
        }
        if (fs::is_regular_file(jobs_root / "failed" / Utf8Path(item.job_id), ec)) {
            ProjectMemory::MemoryWriteCompletion completion;
            completion.job_id = item.job_id;
            completion.operation_id = item.operation_id;
            completion.layer = item.layer;
            completion.title = item.title;
            completion.outcome = "failed";
            completion.error = FirstLineBounded(
                fs::path(PathUtf8(jobs_root / "failed" / Utf8Path(item.job_id)) + ".error.txt"), 300);
            if (completion.error.empty()) completion.error = "worker 判失败(无回执详情)";
            out.push_back(std::move(completion));
            continue;
        }
        ProjectMemory::MemoryWriteCompletion completion;
        completion.job_id = item.job_id;
        completion.operation_id = item.operation_id;
        completion.layer = item.layer;
        completion.title = item.title;
        completion.outcome = "failed";
        completion.error = "job 已不在队列且无提交回执,去向不明;请 /memory jobs 核对";
        out.push_back(std::move(completion));
    }
    if (!keep.empty()) {
        std::lock_guard lock(tracked_writes_->mutex);
        tracked_writes_->writes.insert(tracked_writes_->writes.end(),
                                       std::make_move_iterator(keep.begin()),
                                       std::make_move_iterator(keep.end()));
    }
    // 在途但无活 worker:补拉一次(监督器里合并唤醒/有界退避挡着风暴)。
    if (stalled_in_flight) {
        (void)EnsureWorkerRunning();
    }
    return out;
}

}  // namespace lubancode::memory::queue

namespace lubancode::memory {

namespace fs = std::filesystem;

// 隐藏 CLI 子命令调用(公共口,声明在 project_memory.hpp)。串行捞
// pending/*.json;成功删 job,坏 job 挪 failed。
std::expected<std::size_t, std::string> RunPendingMemoryJobs(const fs::path& home_lubancode) {
    const fs::path jobs_root = AbsoluteNormal(home_lubancode) / "memory-jobs";
    const fs::path pending = jobs_root / "pending";
    std::error_code ec;
    if (!fs::exists(pending, ec)) return std::size_t{0};
    // SV-01:队列独占走所有权裁决。活持有者(别宿主的 worker)在 → 有界
    // 等它放手;真失败(owner 读不懂)不磨轮数,明报上抛。
    OwnerLock worker_lock;
    OwnerLock::Result lock_result;
    for (int attempt = 0; attempt < 50; ++attempt) {
        lock_result = OwnerLock::TryAcquire(jobs_root / "worker.lock", &worker_lock);
        if (lock_result.status == OwnerLock::Status::Acquired) break;
        if (lock_result.status == OwnerLock::Status::BrokenLock) {
            return std::unexpected("memory worker 锁 owner 账读不懂,不敢动: " +
                                   lock_result.detail);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (lock_result.status != OwnerLock::Status::Acquired) {
        return std::unexpected("等待 memory worker 锁超时: " + lock_result.detail);
    }

    std::size_t completed = 0;
    int empty_scans = 0;
    while (true) {
        std::vector<fs::path> jobs;
        ec.clear();
        fs::directory_iterator it(pending, ec);
        if (ec) return std::unexpected("读取 memory pending 目录失败: " + ec.message());
        for (const auto& item : it) {
            if (item.is_regular_file(ec) && item.path().extension() == ".json") jobs.push_back(item.path());
        }
        if (jobs.empty()) {
            // 空扫复核(修复单 §五 A 空窗护栏):枚举刚完、宿主恰好在这一拍
            // 落进新 job 的窗口,由复核补上——锁还在手里,所见即所得。复核
            // 仍空才真退出。
            if (++empty_scans >= 2) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            continue;
        }
        empty_scans = 0;
        std::sort(jobs.begin(), jobs.end());
        for (const fs::path& job : jobs) {
            auto result = queue::ProcessJob(job, home_lubancode);
            if (result.has_value()) {
                ec.clear();
                if (!fs::remove(job, ec) || ec) {
                    return std::unexpected("删除已完成 memory job 失败: " + ec.message());
                }
                ++completed;
            } else {
                queue::MoveFailedJob(job, jobs_root / "failed", result.error());
                ec.clear();
                if (fs::exists(job, ec)) {
                    return std::unexpected("归档失败 memory job 失败: " + result.error());
                }
            }
        }
    }
    return completed;
}

}  // namespace lubancode::memory
