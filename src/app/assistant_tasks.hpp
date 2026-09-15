// 常驻助理 Web 主界面单 W2:任务/审批/事件账三件。
//
//   - AssistantEventHub:进程级有界事件账(seq 单调、bootId 绑定)。任务
//     与审批事件先进账再推活连接——断线期间的事件不丢(留有界账),
//     重连后经 assistant/events/read 按 lastSeq 补齐;bootId 不符或缺口
//     被帽挤出 → reset,页面快照重读。这是与聊天线"没人听的事件不留"
//     (W0 合同)的分岔:任务/审批是补账面,页面不是账本但服务端留账。
//   - AssistantApprovalBroker:自动任务执行里 needs_confirm 工具的
//     "问页面"。泵线程同步问(登记 + 事件 + 等到 deadline);读线程
//     经 approval/respond 答复。超时政策默认拒绝不默认放行,拒绝文案
//     如实写"审批超时",不冒充用户拒绝。
//   - AssistantAutomationFace:任务方法面的读写侧。写侧走 gateway 控制
//     命令文件(与 gateway run 的 CLI 同一份合同,PollJobCommands 消费);
//     读侧走只读投影文件(ReadAutomationProjection/ReadOutboxProjection,
//     与泵的单写者不抢账)。方法面不碰泵内存——线程安全零锁。
//   - AssistantAutomationRuntime:装配件。持 gateway 同 profile 的
//     GatewayLock(单写者互斥:同 profile 的 gateway run 在跑 → 任务面
//     禁用、聊天线照常)+ GatewayAutomationPump + 泵线程(每 tick:
//     TickOnce → store 内存投影 diff → 事件进账)。
//
// 泵并轨定案(W2,详见 docs/features/assistant-web/README.md §三):助理
// 进程内直接驱动 GatewayAutomationPump,不经 gateway_launch(那是阻塞
// 整进程启动器,自带锁/信号/停机合同,不能内嵌);也不另起 Gateway
// 进程(单用户本机场景,进程分工没有收益,反而多一份账互斥)。
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "gateway/automation_store.hpp"
#include "gateway/process.hpp"  // GatewayLock(单写者互斥)
#include "gateway/profile.hpp"
#include "gateway/reply_outbox.hpp"
#include "gateway/work_pump.hpp"
#include "runtime/automation_pump.hpp"
#include "workspace/identity.hpp"

namespace lubancode::app_server {
class Dispatcher;
}

namespace lubancode::api {
class Backend;
}
namespace lubancode::tools {
class ToolRegistry;
}

namespace lubancode::app {

// ---------------------------------------------------------------------------
// 事件账(W2 重连补账)
// ---------------------------------------------------------------------------
class AssistantEventHub {
public:
    struct Entry {
        std::uint64_t seq = 0;
        std::string method;
        nlohmann::json params;
    };

    struct ReadResult {
        std::string boot_id;
        std::uint64_t current_seq = 0;  // 账里最大 seq(0 = 空账)
        std::uint64_t oldest_seq = 0;   // 账里最老 seq(0 = 空账)
        bool reset = false;             // bootId 不符/缺口被挤出:页面须快照重读
        std::vector<Entry> events;      // seq > lastSeq 的增量(reset 时回最近一批)
    };

    // capacity 0 视为 1(账至少留一枚,便于 reset 判定)。
    AssistantEventHub(std::string boot_id, std::size_t capacity = 512);

    // 事件出口(推当前活连接)。装配层递;须线程安全(泵线程/读线程都
    // 会 Push)。空 = 只进账不推(测试装配)。
    void set_sink(
        std::function<void(const std::string& method, const nlohmann::json& params)> sink);

    // 进账 + 推送。返回这枚事件的 seq。
    std::uint64_t Push(std::string method, nlohmann::json params);

    // 补账读:bootId 对不上 → reset + 最近 max_events 枚;lastSeq 落在
    // 账的覆盖范围里 → 增量;缺口被帽挤出 → reset + 最近一批。
    ReadResult Read(const std::string& boot_id, std::uint64_t last_seq,
                    std::size_t max_events = 256) const;

    const std::string& boot_id() const { return boot_id_; }
    std::uint64_t current_seq() const;

private:
    const std::string boot_id_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::uint64_t next_seq_ = 1;
    std::deque<Entry> entries_;
    std::function<void(const std::string& method, const nlohmann::json& params)> sink_;
};

// ---------------------------------------------------------------------------
// 审批面(任务执行里 needs_confirm 工具的"问页面")
// ---------------------------------------------------------------------------
class AssistantApprovalBroker {
public:
    // hub 是事件出口(assistant/approval/request、assistant/approval/resolved)。
    explicit AssistantApprovalBroker(AssistantEventHub* hub, std::int64_t default_timeout_ms);

    // 当前在飞执行的 (jobId, occurrenceId) 上下文口:Runtime 在泵线程里
    // 装配(从 store 查 claimed 的那枚——单飞泵同时至多一枚在飞)。审批
    // 事件带归属,页面能把审批挂到任务上。空返回 = 查不到(如实,事件
    // 里 jobId/occurrenceId 留空)。
    void set_context_provider(
        std::function<std::pair<std::string, std::string>()> provider) {
        context_provider_ = std::move(provider);
    }

    // 泵线程同步问:登记 + 推事件 + 等到 deadline(分片轮询,不造线程)。
    // 返回 (allowed, denial_text)。超时 = 拒绝,denial_text 如实写超时;
    // CancelAll 收口的悬起 = 拒绝,文案写"助理收口"。
    // job/occurrence 为空时经 context_provider 取(委托路径)。
    std::pair<bool, std::string> Ask(const std::string& job_id, const std::string& occurrence_id,
                                     const std::string& tool_use_id, const std::string& tool_name,
                                     const nlohmann::json& input);

    struct RespondOutcome {
        bool resolved = false;  // false = stale(答完/收口/不认识)
        std::string reason;     // 人话(诊断用)
    };
    // 读线程答复(accept/decline)。
    RespondOutcome Respond(const std::string& request_id, bool accept);

    // 悬着的审批投影(approval/list 用;重连/刷新后可发现)。
    std::vector<nlohmann::json> ListPending() const;

    // 悬空收口(泵停/助理收口):全部按拒绝醒过来,事件 outcome=cancelled。
    void CancelAll(const std::string& reason);

    std::size_t pending_count() const;
    std::int64_t default_timeout_ms() const { return default_timeout_ms_; }

private:
    struct Pending {
        std::string request_id;
        std::string job_id;
        std::string occurrence_id;
        std::string tool_name;
        nlohmann::json input;
        std::int64_t created_at_ms = 0;
        std::int64_t deadline_wall_ms = 0;
        std::int64_t timeout_ms = 0;
        // 答复:nullopt = 尚无/收口;true = accept;false = decline。
        std::shared_ptr<std::promise<std::optional<bool>>> answer;
        // 收口旗:CancelAll 置位(Ask 分辨"没人答超时"与"收口拒")。
        std::shared_ptr<std::atomic<bool>> cancelled;
    };

    AssistantEventHub* hub_;
    const std::int64_t default_timeout_ms_;
    mutable std::mutex mutex_;
    std::map<std::string, Pending> pending_;
    std::uint64_t next_request_ = 1;
    // 只归泵线程调(store 查询不跨线程);读线程不碰。
    std::function<std::pair<std::string, std::string>()> context_provider_;
};

// ---------------------------------------------------------------------------
// 任务方法面(读写侧,纯文件操作;assistant_host 与单测共用)
// ---------------------------------------------------------------------------
class AssistantAutomationFace {
public:
    AssistantAutomationFace(gateway::GatewayProfilePaths paths, AssistantEventHub* hub,
                            AssistantApprovalBroker* broker);

    // 任务面可用吗(锁被 gateway 占/账开不了时 false,方法回稳定错误)。
    bool available() const { return available_; }
    void set_available(bool available) { available_ = available; }
    const std::string& unavailable_reason() const { return unavailable_reason_; }
    void set_unavailable_reason(std::string reason) { unavailable_reason_ = std::move(reason); }

    // ---- 方法实现(handler 的内脏;错误走 out_error_*,返回 result) ----

    // task/create:{prompt, dueAtMs?, clientOperationId}。幂等:同键再交
    // 回原 jobId(duplicate=true),不写第二枚命令。due 0/缺省 = 立即。
    nlohmann::json HandleTaskCreate(const nlohmann::json& params, std::string& out_error_code,
                                    std::string& out_error_message);
    // task/run-now:{jobId, clientOperationId}。幂等同上(runnow_keys)。
    nlohmann::json HandleTaskRunNow(const nlohmann::json& params, std::string& out_error_code,
                                    std::string& out_error_message);
    // task/list:{}。任务摘要 + 最近 occurrence + 结果状态(outbox 投影)。
    nlohmann::json HandleTaskList(const nlohmann::json& params, std::string& out_error_code,
                                  std::string& out_error_message);
    // task/read:{jobId}。任务全档 + occurrences(含结果正文,读发布文件
    // /replies 原件,有界 64KB)。
    nlohmann::json HandleTaskRead(const nlohmann::json& params, std::string& out_error_code,
                                  std::string& out_error_message);
    // task/cancel:{jobId, expectedRevision, clientOperationId}。CAS;
    // 已取消的重复取消回当前态(幂等)。
    nlohmann::json HandleTaskCancel(const nlohmann::json& params, std::string& out_error_code,
                                    std::string& out_error_message);
    // approval/list。
    nlohmann::json HandleApprovalList(const nlohmann::json& params, std::string& out_error_code,
                                      std::string& out_error_message);
    // approval/respond:{requestId, decision:"accept"|"decline"}。
    nlohmann::json HandleApprovalRespond(const nlohmann::json& params, std::string& out_error_code,
                                         std::string& out_error_message);
    // assistant/events/read:{bootId, lastSeq}。
    nlohmann::json HandleEventsRead(const nlohmann::json& params, std::string& out_error_code,
                                    std::string& out_error_message);

    // 结果正文查找(outbox 投影按 sessionId+turnId 匹配;优先发布文件,
    // 回落 replies 原件)。occurrence 无绑定时回 nullopt(没开过轮)。
    // 公开给单测钉形状。
    std::optional<nlohmann::json> FindOccurrenceResult(const gateway::AutomationOccurrence& occurrence,
                                                       const gateway::OutboxProjection& outbox);

private:
    // 写命令文件后轮询投影至回执出现(创建/run-now 的受理核对)。
    // 等不到(泵忙/没起来)回 false——调用方如实报错,幂等键在,客户端
    // 重发不双建。
    bool WaitForCreateReceipt(const std::string& idempotency_key, std::string* out_job_id,
                              std::string* out_occurrence_id, std::uint64_t* out_revision);
    bool WaitForRunNowReceipt(const std::string& idempotency_key, std::string* out_occurrence_id);

    gateway::GatewayProfilePaths paths_;
    AssistantEventHub* hub_;
    AssistantApprovalBroker* broker_;
    bool available_ = true;
    std::string unavailable_reason_;
};

// 方法注册(assistant_host 的 extra_method_registrar 与单测直驱共用)。
// face 须以 shared_ptr 持有(每条连接的 dispatcher 都捕一份)。
void RegisterAssistantTaskMethods(app_server::Dispatcher& dispatcher,
                                  const std::shared_ptr<AssistantAutomationFace>& face);

// ---------------------------------------------------------------------------
// 装配件:gateway 锁 + 泵 + 泵线程
// ---------------------------------------------------------------------------
class AssistantAutomationRuntime {
public:
    struct Options {
        gateway::GatewayProfilePaths paths;
        std::filesystem::path workspaces_root;
        workspace::WorkspaceIdentity workspace_identity;
        std::string cwd_utf8;
        std::string lubancode_version;
        std::string wire_name;
        std::string model;
        std::int64_t approval_timeout_ms = 120 * 1000;  // 审批超时(默认拒绝)
        int max_steps_per_turn = 32;
        int max_wall_secs = 600;
        std::int64_t max_total_tokens = 0;
    };

    // 打开:先取 gateway 同 profile 锁(单写者互斥),再开泵。任何一步
    // 不过都回 nullptr + reason(调用方把任务面标不可用,聊天线照常)。
    // broker 由装配层建、face 与 runtime 共享(任务面不可用时 approval/*
    // 仍如实回:pending 空、答复 stale)。backend/registry 借用,须活过
    // 本对象。start_thread=false(单测):不起泵线程,调用方手动
    // TickAndPublish。
    struct OpenOutcome {
        std::unique_ptr<AssistantAutomationRuntime> runtime;
        std::string unavailable_reason;  // 空 = 打开成功
    };
    static OpenOutcome Open(api::Backend& backend, tools::ToolRegistry& registry, Options options,
                            AssistantEventHub* hub,
                            const std::shared_ptr<AssistantApprovalBroker>& broker,
                            bool start_thread = true);
    ~AssistantAutomationRuntime();

    AssistantAutomationRuntime(const AssistantAutomationRuntime&) = delete;
    AssistantAutomationRuntime& operator=(const AssistantAutomationRuntime&) = delete;

    runtime::GatewayAutomationPump* pump() { return &pump_; }
    const gateway::GatewayProfilePaths& paths() const { return options_.paths; }

    // 一轮泵推进 + 事件 diff(泵线程体;单测直驱用)。
    // 返回 false = 泵 broken(账写不进),事件面停摆但进程保留供诊断。
    bool TickAndPublish(std::int64_t now_ms);

    // 收口:停接新活 → 停线程 → 泵 Close → 释放锁。悬着的审批按拒绝醒。
    void Stop();

private:
    AssistantAutomationRuntime(Options options, AssistantEventHub* hub,
                               std::shared_ptr<AssistantApprovalBroker> broker);

    void DiffAndPublish();

    Options options_;
    AssistantEventHub* hub_;
    std::shared_ptr<AssistantApprovalBroker> broker_;
    runtime::GatewayAutomationPump pump_;
    gateway::GatewayLock lock_;
    std::atomic<bool> stop_requested_{false};
    std::thread thread_;
    // 上一轮的投影快照(事件 diff 用;只归泵线程碰)。
    std::map<std::string, std::pair<std::string, std::uint64_t>> last_jobs_;      // jobId -> (state, revision)
    std::map<std::string, std::pair<std::string, std::string>> last_occurrences_;  // occurrenceId -> (state, outcome)
};

}  // namespace lubancode::app
