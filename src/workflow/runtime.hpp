// WorkflowRuntime(自然语言编排单第 2 批):跑图的那只引擎。
//
// 状态机(单子"运行状态机"):
//   WorkflowRun: created -> validating -> ready -> running
//                 -> waiting_input / waiting_approval / paused
//                 -> succeeded / failed / cancelled / budget_exhausted
//   NodeRun:     pending -> ready -> running -> succeeded/skipped/failed/
//                 cancelled;retry_wait -> ready;waiting_input/waiting_approval
// 迁移写成纯函数与迁移表,非法迁移拒绝并记诊断,不静默改状态。
//
// 节点生命周期(单子 Node 一节):
//   ResolveInputs -> ValidateInputs -> Execute -> ValidateOutput ->
//   CommitOutput -> EmitOutcome。retry 只包 Execute。
//
// 执行器抽象:NodeExecutor 按节点种类接现成设施(ToolRegistry、
// InteractionBroker、backend);runtime 只管图,不知道工具从哪儿来。
// 单测喂 fake executor/fake clock,不靠 sleep 赌时序(单子验收末条)。

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/event_sink.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/interaction_broker.hpp"
#include "workflow/definition.hpp"
#include "workflow/journal.hpp"
#include "workflow/store.hpp"

namespace lubancode::runtime {
class TrajectorySessionLedger;          // workflow 编排账(workflow 会话归属统一单)
class TrajectoryWorkflowRunBridge;      // 同上:runtime.hpp 只持指针,实现在 runtime 层
class TrajectoryWorkflowNodeBridge;     // 同上:node attempt 账,执行器经它落模型/工具事件
}

namespace lubancode::workflow {

// ---------------------------------------------------------------------------
// 状态机(纯函数,单测钉)
// ---------------------------------------------------------------------------

enum class RunState {
    Created, Validating, Ready, Running,
    WaitingInput, WaitingApproval, WaitingIo, Paused,
    Succeeded, Failed, Cancelled, BudgetExhausted,
};
std::string ToString(RunState state);
bool ParseRunState(const std::string& s, RunState& out);
bool IsTerminalRunState(RunState state);

enum class NodeState {
    Pending, Ready, Running, RetryWait,
    WaitingInput, WaitingApproval, WaitingIo,
    Succeeded, Skipped, Failed, Cancelled, Interrupted,
};
std::string ToString(NodeState state);
bool ParseNodeState(const std::string& s, NodeState& out);
bool IsTerminalNodeState(NodeState state);

// 迁移表查询:from --动作--> to 是否合法。动作即目标状态(简化:状态即
// 动作),非法组合返回 false。
bool IsValidRunTransition(RunState from, RunState to);
bool IsValidNodeTransition(NodeState from, NodeState to);

// ---------------------------------------------------------------------------
// 节点执行器抽象
// ---------------------------------------------------------------------------

// 一次节点执行的上下文与产物。Execute 只产候选 output,不直接写 Store
// (CommitOutput 由 runtime 统一做,单子 Node 生命周期)。
struct NodeExecRequest {
    const WorkflowDefinition* definition = nullptr;
    const WorkflowNode* node = nullptr;
    std::string run_id;
    std::string node_run_id;    // 本 attempt 的独立 id
    int attempt = 1;
    nlohmann::json resolved_input = nlohmann::json::object();  // ResolveInputs 产物
    const Store* store = nullptr;
    const std::atomic<bool>* cancel = nullptr;  // 长活儿须合作检查
    // node attempt 账(workflow 会话归属统一单):编排账开出的 scoped sink,
    // agent/llm 节点把模型请求/工具事件落这(ownership 与子代理同款)。
    // 空 = 没接轨迹(headless 测试),执行器照旧零挂账。
    lubancode::runtime::TrajectoryWorkflowNodeBridge* trajectory = nullptr;
};

struct NodeExecResult {
    bool ok = false;
    std::string error_code;      // 稳定 code;空 = 成功
    std::string error_message;
    nlohmann::json output = nlohmann::json::object();
    bool empty = false;          // "没有产出"分支(outcome=empty)
    std::int64_t tokens_used = 0;   // 计入 run 预算(重试也计,不开免单账)
    std::int64_t duration_ms = 0;
    // agent 节点派的自定义 Agent 的 resolved 名(阶段 5;空 = 非自定义
    // 路或未解析)。回执身份用——journal 与事件不冒用户写的短名。
    std::string agent_name;
};

// 执行器:一只节点种类的落地。宿主(tool/llm/agent/approval...)各给实现;
// 单测给 fake。Execute 不碰 Store 写路径、不直接发事件(事件由 runtime 按
// 生命周期发)。
class NodeExecutor {
public:
    virtual ~NodeExecutor() = default;
    virtual NodeExecResult Execute(const NodeExecRequest& request) = 0;
};

// ---------------------------------------------------------------------------
// 运行时
// ---------------------------------------------------------------------------

// 节点运行账。
struct NodeRunRecord {
    std::string node_id;
    NodeState state = NodeState::Pending;
    int attempt = 0;
    std::string error_code;
    std::string error_message;
    // agent 节点派的自定义 Agent 的 resolved 名(阶段 5;空 = 非自定义路)。
    std::string agent_name;
    std::int64_t started_ms = 0;
    std::int64_t ended_ms = 0;
    std::int64_t tokens_used = 0;
};

// 一场 run 的总账。
struct WorkflowRunSummary {
    std::string run_id;
    std::string workflow_id;
    RunState state = RunState::Created;
    std::string error_code;      // 终态非成功时的稳定 code
    std::string error_message;
    nlohmann::json result = nlohmann::json::object();  // result 映射的产物
    std::vector<std::string> unavailable_sources;       // skip 的节点账
    std::int64_t duration_ms = 0;
    std::int64_t tokens_used = 0;
    int tool_calls = 0;
    std::map<std::string, NodeRunRecord> nodes;
};

// runtime 的装配材料。executors 按 NodeKind 配;EventSink 可空(没画面
// 的跑法,测试/headless)。broker 供 approval/ask_user 用,可空(空则这
// 两种节点直接报 not_configured 失败,不挂死)。
struct RuntimeOptions {
    std::map<NodeKind, std::shared_ptr<NodeExecutor>> executors;
    runtime::EventSink* event_sink = nullptr;
    runtime::InteractionBroker* broker = nullptr;
    std::filesystem::path runs_root;   // 空 = 不落盘(headless 测试)
    std::shared_ptr<JournalClock> clock;
    std::function<std::string()> run_id_generator;  // 空 = 时间戳+随机
    std::string thread_id;  // 事件信封用;空 = "workflow"
    // 事件信封 seq 的发号局(批五):空 = 进程级 ProcessIdAuthority。
    // 从前是本文件里一只 static 计数器——发号局规矩"只此一家",收编。
    runtime::IdAuthority* id_authority = nullptr;
    // 编排账(workflow 会话归属统一单):非空 = run 起动经
    // ReserveWorkflowRun 开轨迹 Journal、逐 attempt 开 node stream(fail
    // closed)。空 = 不落轨迹账(旧 workflow-runs/ 路照旧,兼容头less 测试)。
    lubancode::runtime::TrajectorySessionLedger* trajectory_ledger = nullptr;
};

// 起跑入参。
struct RunInputs {
    nlohmann::json values = nlohmann::json::object();
    // 聚合构造顺手:RunInputs{{"topic", "x"}} 直接给 values。
    explicit RunInputs(nlohmann::json v) : values(std::move(v)) {}
    RunInputs() = default;
};

class WorkflowRuntime {
public:
    explicit WorkflowRuntime(RuntimeOptions options);

    // 同步跑完整张图(顺序与并行皆可;首版图在跑动中不接外部输入,
    // waiting 分支经 broker 悬起)。取消经 cancel_token。
    WorkflowRunSummary Run(const WorkflowDefinition& definition, const RunInputs& inputs,
                           const std::atomic<bool>* cancel_token = nullptr);

    // 断点续跑(第 4 批):从 run_dir 的 definition 快照 + journal 重放,
    // 已 succeeded 的节点不再跑(副作用不重复做),从第一只未完成节点续。
    // 定义 hash 对不上(定义被改/删)给差异报错,不硬续。
    std::expected<WorkflowRunSummary, std::string> Resume(const std::filesystem::path& run_dir,
                                                          const std::atomic<bool>* cancel_token = nullptr);

    // 预算检查点(内部用,公开给测试):步数/工具调用/token 是否越帽。
    bool WithinBudget(const WorkflowLimits& limits, const WorkflowRunSummary& account) const;

    const RuntimeOptions& options() const { return options_; }

private:
    // 带预置 Store 与已完成节点账的内部跑法(Resume 用)。
    WorkflowRunSummary RunWithStore(const WorkflowDefinition& definition, const nlohmann::json& inputs,
                                    Store&& preloaded,
                                    const std::map<std::string, NodeRunRecord>& precompleted,
                                    const std::atomic<bool>* cancel_token, bool resuming);

    struct ExecutionContext {
        const WorkflowDefinition* definition = nullptr;
        WorkflowRunSummary* account = nullptr;
        Store* store = nullptr;
        RunJournal* journal = nullptr;
        // 编排账(workflow 会话归属统一单):null = 本场不落轨迹账。
        lubancode::runtime::TrajectoryWorkflowRunBridge* trajectory = nullptr;
        // 派发序号(一场 run 一只,node_run_id 带 -d<N> 防 loop 重入撞名)。
        std::atomic<std::uint64_t>* dispatch_seq = nullptr;
        const std::atomic<bool>* cancel = nullptr;
        int* steps = nullptr;  // 主图与 loop body 共用同一把 max_steps 尺
        std::int64_t started_ms = 0;  // async 等待也守整场 run 的总时限
        // 并行分支共写 account.nodes(std::map 并发写会坏):所有
        // NodeRunRecord 的读改走这把锁。Store 自带锁,不归它管。
        std::mutex* nodes_mutex = nullptr;
    };

    // 单节点全生命周期(含 retry)。返回 outcome(success/error/empty/skipped)。
    // committed_output 可空:非空时把落账产物当场交还(map 并发逐项跑 body,
    // store 的 body 键是共用垫,回头 GetOutput 会拿到别人的那份)。
    // item_index >= 0(map/foreach 的 body):node_run_id 带 "-i<下标>" 路号——
    // 同一 body 并发跑多路时事件账才分得清谁是谁,面板/诊断不串线。
    std::string RunNode(const ExecutionContext& ctx, const WorkflowNode& node,
                        nlohmann::json* committed_output = nullptr, int item_index = -1);
    // async:I/O 等待边界。body 在工作线程跑,外壳守取消与总时限;
    // 返回 success/error/skipped/cancelled/budget_exhausted。
    std::string RunAsync(const ExecutionContext& ctx, const WorkflowNode& node);
    // 并行分支调度 + 汇合(第 3 批)。返回 join 后的 outcome
    // (success/error/skipped/cancelled);分支账进 store:<id>.outputs 按
    // 定义顺序、<id>.unavailable 记缺失。
    std::string RunParallel(const ExecutionContext& ctx, const WorkflowNode& node);
    // map/foreach:数组拆项跑 body。map 并发(foreach 顺次);map 的结果
    // 数组按 items 顺序排,完成时间只进 meta。
    std::string RunMap(const ExecutionContext& ctx, const WorkflowNode& node);
    // loop:顺次跑 body,每轮末判 until;max 可由输入给,hard_limit 写死。
    std::string RunLoop(const ExecutionContext& ctx, const WorkflowNode& node);
    // reduce:items 数组按定义顺序过 reduce_body,累加成单值。
    std::string RunReduce(const ExecutionContext& ctx, const WorkflowNode& node);
    // switch:受限条件评估(ConditionOp,禁 eval)。
    std::string EvaluateSwitch(const WorkflowDefinition& def, const WorkflowNode& node, const Store& store) const;
    // 取某节点某 outcome 的下一站;空串 = 没边(结束)。
    std::string NextNodeFor(const WorkflowDefinition& def, const std::string& node_id,
                            const std::string& outcome) const;
    // 算 result 映射(终点时)。
    std::expected<nlohmann::json, ResolveError> BuildResult(const WorkflowDefinition& def, const Store& store) const;
    void EmitRunEvent(const WorkflowRunSummary& account, const char* payload_type, nlohmann::json payload);

    RuntimeOptions options_;
};

// ---------------------------------------------------------------------------
// 内建执行器(第 2 批:transform/template/switch/end/checkpoint 直落;
// tool/llm/agent/approval/ask_user/subflow/skill 由宿主注入)
// ---------------------------------------------------------------------------

// transform:注册表里查名字,纯数据变换。找不到报 unknown_transform(
// 不认魔法字符串,单子"Workflow 定义草案"末段)。
using TransformFn = std::function<nlohmann::json(const nlohmann::json& input)>;
class TransformExecutor : public NodeExecutor {
public:
    void Register(const std::string& operation, TransformFn fn);
    NodeExecResult Execute(const NodeExecRequest& request) override;

private:
    std::map<std::string, TransformFn> transforms_;
};

// template:{{field.path}} 占位的安全渲染——不是 eval,不是 format,只做
// 点路径取值替换。缺字段按空串,不抛。
class TemplateExecutor : public NodeExecutor {
public:
    NodeExecResult Execute(const NodeExecRequest& request) override;
};

// 静态输入 echo(测试与 /workflow test 用):input 原样作 output。
class EchoExecutor : public NodeExecutor {
public:
    NodeExecResult Execute(const NodeExecRequest& request) override;
};

}  // namespace lubancode::workflow
