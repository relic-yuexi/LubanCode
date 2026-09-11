// Workflow 编排账(Workflow 接入 Session v3 第一棒):WorkflowRun/
// NodeExecution/产物提交的 schema 冻结、目录 resolver 与恢复重放。
//
// 设计合同(todos/Workflow接入SessionV3_编排账与节点恢复设计.todo):
//   §三  workspaces 下规范目录,WorkflowPathResolver 统一查找;身份分层
//        (workflowRunId/orchestrationSegmentId/nodeId/nodeExecutionId/
//        attempt/outputId);编排账只写 type=event(经 v3 事件账 writer
//        profile),不造 system、不写 message。
//   §四  定义与 hash 先落稳再执行;resume 只认快照,配置 reload 不偷换定义。
//   §五  output.commit 是"节点产物可供下游消费"的唯一依据;原件先落稳、
//        事件落稳后才许发布 Store、放行后继(fail-closed)。
//   §十  恢复判据:checkpoint 用无损受控数据与已提交 checkpointRef;
//        node_completed/outcome=error 不判成功;commit 已落、Store 未更新
//        则从 commit 重建;保存原件后崩溃从候选继续,不重跑。
//
// 与旧 RunJournal 的关系:account 模式(account_root 非空)下它是唯一编排
// 事实源,旧 journal/旧 v2 桥不开;旧路(runs_root)原样保留给既有调用方,
// 棒二 WorkflowService 收拢装配。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "workflow/journal.hpp"  // JournalClock 复用(测试喂 fake 钟)
#include "trajectory/v3/event_ledger.hpp"
#include "workflow/definition.hpp"

namespace lubancode::workflow {

// 开账/续账/提交失败的结构化账:阶段 + 稳定码 + 人话(与 SubagentSpawnFailure
// 同款——调用方吞 error() 是查不出第一因的根,失败必须带阶段过境)。
struct WorkflowAccountError {
    // 失败阶段:start_dirs | definition_snapshot | bindings_snapshot |
    // ledger_start | resume_verify | definition_hash | inputs_file |
    // checkpoint_file | output_file | commit_event | segment_open |
    // tail_repair_needed
    std::string stage;
    std::string error_code;
    std::string detail;
};

// ---------------------------------------------------------------------------
// 节点执行身份(§三 身份表)
// ---------------------------------------------------------------------------

// 定义节点(nodeId)与一次逻辑调用(nodeExecutionId)分开;attempt 是同一
// execution 的重试。id 形状冻结:<runId>-<nodeId>[-i<mapIndex>][-d<dispatch>]
// + "-a<attempt>";dispatch 号跨恢复延续(重放恢复计数,不撞名)。
struct NodeExecutionIdentity {
    std::string node_id;
    std::string node_execution_id;
    int attempt = 1;
    // reserve 时输入快照的内容寻址(canonical JSON 的 SHA-256);commit 事件
    // 引用它(§五:resolvedInputRef)。
    std::string input_sha256;
    // 层级位置(§三):{mapItemIndex?}(loop iteration/subflow 位置归后续棒)。
    nlohmann::json invocation_path = nlohmann::json::object();

    std::string attempt_id() const { return node_execution_id + "-a" + std::to_string(attempt); }
};

// ---------------------------------------------------------------------------
// 产物提交 schema(outputs/<outputId>.json 冻结形状)
// ---------------------------------------------------------------------------

// 无损结构化产物记录。payload 是候选正文原样(不脱敏——脱敏是展示/导出
// 投影的事,不是机器恢复源的事,设计 §五"输出敏感数据与展示脱敏分开")。
struct OutputCommitRecord {
    std::string schema = "workflow-output/1";
    std::string output_id;           // out-<六位号>,run 内单调
    std::string workflow_run_id;
    std::string node_id;
    std::string node_execution_id;
    std::string attempt_id;
    nlohmann::json payload = nlohmann::json::object();
    std::string payload_sha256;      // canonical JSON 的 SHA-256
    std::string input_sha256;        // reserve 时的输入快照内容寻址(可空)
    bool validation_passed = false;
    nlohmann::json validation = nlohmann::json::object();  // {passed, checks[]}
    std::int64_t created_at_ms = 0;

    nlohmann::json ToJson() const;
    static std::optional<OutputCommitRecord> FromJson(const nlohmann::json& json);
};

// checkpoint 记录(checkpoints/<checkpointId>.json):无损 Store 快照 + 已
// 提交水位。孤立候选文件不生效——生效判据是账上的
// workflow.checkpoint.committed 事件(设计 §十)。
struct CheckpointRecord {
    std::string schema = "workflow-checkpoint/1";
    std::string checkpoint_id;       // cp-<六位号>
    std::string workflow_run_id;
    std::string segment_id;
    std::uint64_t through_seq = 0;   // 提交时账内已落稳的末行 seq
    std::uint64_t store_revision = 0;
    nlohmann::json store = nlohmann::json::object();  // 无损 Store JSON
    std::string store_sha256;
    std::int64_t created_at_ms = 0;

    nlohmann::json ToJson() const;
    static std::optional<CheckpointRecord> FromJson(const nlohmann::json& json);
};

// 产物校验结果(runtime 在 commit 前算好递进来;候选保存照旧,失败不提交)。
struct OutputValidation {
    bool passed = true;
    nlohmann::json checks = nlohmann::json::array();  // 失败项 {code,field,expected,actual}

    nlohmann::json ToJson() const {
        return nlohmann::json{{"passed", passed}, {"checks", checks}};
    }
};

// ---------------------------------------------------------------------------
// 目录 resolver(§三):workflowRun 的编排账/节点子账/产物目录,与 session
// 目录体系并列不混淆。API 按身份与引用读取,不让客户端拼路径。
// ---------------------------------------------------------------------------

class WorkflowPathResolver {
public:
    WorkflowPathResolver() = default;
    // runs_root = <workspace>/workflow-runs;run_id 须是合法单段名。
    static std::expected<WorkflowPathResolver, std::string> ForRun(
        const std::filesystem::path& runs_root, std::string_view workflow_run_id);

    const std::filesystem::path& runs_root() const { return runs_root_; }
    const std::string& run_id() const { return run_id_; }

    std::filesystem::path run_dir() const { return runs_root_ / run_id_; }
    std::filesystem::path definition_path() const { return run_dir() / "definition.json"; }
    std::filesystem::path bindings_path() const { return run_dir() / "bindings.json"; }
    std::filesystem::path inputs_path() const { return run_dir() / "inputs.json"; }
    std::filesystem::path segments_dir() const { return run_dir() / "segments"; }
    std::filesystem::path segment_stream(std::string_view segment_id) const {
        return segments_dir() / std::string(segment_id) / "workflow.jsonl";
    }
    std::filesystem::path checkpoints_dir() const { return run_dir() / "checkpoints"; }
    std::filesystem::path checkpoint_path(std::string_view checkpoint_id) const {
        return checkpoints_dir() / (std::string(checkpoint_id) + ".json");
    }
    std::filesystem::path outputs_dir() const { return run_dir() / "outputs"; }
    std::filesystem::path output_path(std::string_view output_id) const {
        return outputs_dir() / (std::string(output_id) + ".json");
    }
    std::filesystem::path nodes_dir() const { return run_dir() / "nodes"; }
    std::filesystem::path node_dir(std::string_view node_execution_id) const {
        return nodes_dir() / std::string(node_execution_id);
    }
    std::filesystem::path artifacts_dir() const { return run_dir() / "artifacts"; }
    std::filesystem::path subflows_dir() const { return run_dir() / "subflows"; }

    // 段序(seg-1、seg-2…按号升序;目录名不合规的段跳过并如实少报——
    // Resume 验链时对不上会明报,不静默)。
    std::vector<std::string> list_segments() const;
    std::optional<std::string> latest_segment_id() const;
    static std::string NextSegmentId(std::string_view latest);  // "" -> "seg-1"

private:
    WorkflowPathResolver(std::filesystem::path runs_root, std::string run_id)
        : runs_root_(std::move(runs_root)), run_id_(std::move(run_id)) {}

    std::filesystem::path runs_root_;
    std::string run_id_;
};

// ---------------------------------------------------------------------------
// 恢复重放(§十 纠正后的判据)
// ---------------------------------------------------------------------------

// 一次 nodeExecution 的重放账。成功判据 = output.committed 已落(设计 §五:
// commit 是唯一依据);node.completed(outcome)只是收口事实,outcome=error
// 绝不判成功。
struct RecoveredExecution {
    std::string node_id;
    std::string node_execution_id;
    int attempt = 0;
    bool reserved = false;
    bool dispatched = false;
    bool output_committed = false;
    std::string output_id;
    nlohmann::json committed_output;  // outputs/<id>.json 无损读回(已验 hash)
    std::string outcome;              // "" 未收口 | success | empty | failed | cancelled | skipped
    std::string error_code;
    std::int64_t tokens = 0;
};

struct RecoveryState {
    std::string workflow_run_id;
    std::string workflow_id;
    std::string workflow_version;
    std::string definition_hash;
    nlohmann::json definition;  // 归一化快照(definition.json 读回)
    std::string tail_segment_id;
    std::uint64_t tail_segment_last_seq = 0;
    std::string tail_segment_last_hash;
    std::string run_terminal;  // "" 未终态 | succeeded | failed | cancelled
    std::optional<CheckpointRecord> checkpoint;
    std::map<std::string, RecoveredExecution> executions;  // key: nodeExecutionId
    std::vector<std::string> execution_order;              // reserve 先后
    std::map<std::string, std::string> node_latest_execution;  // nodeId -> 最新 execution
    // 悬置候选:原件文件在、账上无 output.committed(保存原件后崩溃)。
    // validation_passed=false 的是被拒候选(校验失败留档),不参与采纳。
    std::vector<OutputCommitRecord> dangling_candidates;
    nlohmann::json inputs = nlohmann::json::object();
    std::int64_t tokens_used = 0;  // 已收口执行带回的 token 账(计数不归零)
    int dispatch_count = 0;        // 已派发执行数(预算恢复的近似底数)
};

// ---------------------------------------------------------------------------
// 一场 workflow run 的编排账(唯一写者 = 持有者)
// ---------------------------------------------------------------------------

class WorkflowRunAccount {
public:
    struct Options {
        std::shared_ptr<JournalClock> clock;
        // 注入提交失败(测试专用;生产恒空):每次账面事件提交前问一次。
        std::function<std::optional<std::string>()> inject_io_failure;
    };
    struct DefinitionInfo {
        std::string workflow_id;
        std::string workflow_version;
        std::string content_hash;
        std::string cwd;
        std::string definition_json;  // 归一化快照文本(definition.json 原文)
    };

    WorkflowRunAccount() = default;
    WorkflowRunAccount(WorkflowRunAccount&&) noexcept;
    WorkflowRunAccount& operator=(WorkflowRunAccount&&) noexcept;
    WorkflowRunAccount(const WorkflowRunAccount&) = delete;
    WorkflowRunAccount& operator=(const WorkflowRunAccount&) = delete;
    ~WorkflowRunAccount();

    // 开新账:建目录树、definition.json/bindings.json 原子落稳、开 seg-1
    // (首事件 workflow.definition.loaded)、inputs.json +
    // workflow.inputs.committed。run 目录已存在即失败(单写者,create-new)。
    static std::expected<WorkflowRunAccount, WorkflowAccountError> Start(
        const std::filesystem::path& runs_root, const std::string& workflow_run_id,
        const DefinitionInfo& definition, const nlohmann::json& effective_inputs,
        Options options = Options{});

    // 续账(只读):验全部段链(哈希/seq/只 event 行)、验定义快照 hash、
    // 重放恢复态。run 已终态时也如实重放(run_terminal 留给调用方裁决——
    // 终态 run 拒续跑,显式重跑另起新 run)。不动盘。
    static std::expected<WorkflowRunAccount, WorkflowAccountError> Resume(
        const std::filesystem::path& runs_root, const std::string& workflow_run_id,
        Options options = Options{});

    // 开恢复段(seg-<n+1>):workflow.segment.opened 链接源水位(五键指尾段
    // 末行)。恢复决定继续时才调;Resume 后未 OpenSegment 前账面只读。
    std::expected<std::string, WorkflowAccountError> OpenSegment();

    const WorkflowPathResolver& paths() const;
    const RecoveryState& recovery() const;
    const std::string& run_id() const;
    bool broken() const;

    // ---- 关键事实(写不住返回失败,调用方须停在明确失败态、零派发)----

    // reserve nodeExecution 与输入快照(§五 pipeline 第一步):铸身份、
    // workflow.node.reserved(inputHash = 输入 canonical JSON 的 SHA-256)。
    std::expected<NodeExecutionIdentity, WorkflowAccountError> ReserveNodeExecution(
        const std::string& node_id, const std::string& node_kind, int map_item_index,
        const nlohmann::json& resolved_input);

    // 只铸身份不落账(no_executor/解析失败一类未到 reserve 就死的路径,
    // 失败事实仍要有处指)。dispatch 号与 Reserve 同源,不撞名。
    NodeExecutionIdentity MintExecutionId(const std::string& node_id, int map_item_index) const;

    // 实际派发(workflow.node.dispatched)。
    bool RecordNodeDispatched(const NodeExecutionIdentity& identity);
    // inputs.json(无损)+ workflow.inputs.committed:无 checkpoint 也能恢复
    // inputs(定义在 cpp)。
    bool RecordInputs(const nlohmann::json& effective_inputs);

    // 产物提交:原件先落稳(原子、无损)→ workflow.output.committed
    // (PowerLoss)→ 才算提交成功。adopt_output_id 非空 = 采纳悬置候选
    // (沿用其 id,同一次执行的同一次提交,不重跑)。
    std::expected<OutputCommitRecord, WorkflowAccountError> CommitNodeOutput(
        const NodeExecutionIdentity& identity, const nlohmann::json& payload,
        const OutputValidation& validation, const std::string& adopt_output_id = std::string());

    // 被拒候选留档:校验失败的候选照常保存(孤立文件,validation_passed=
    // false),但不落 commit 事件、不发布——"保存候选,不提交 success"。
    // 写失败只记诊断(候选本身已判失败,不因此改变节点结局)。adopt_
    // output_id 非空 = 采纳件复检不过,改写同一份候选件为被拒留档。
    void SaveRejectedCandidate(const NodeExecutionIdentity& identity,
                               const nlohmann::json& payload, const OutputValidation& validation,
                               const std::string& adopt_output_id = std::string());

    // attempt 收口:outcome ∈ success|empty。注意语义(§五):completed 是
    // 本次执行收口,下游消费资格看 output.committed,不看这里。
    bool RecordNodeCompleted(const NodeExecutionIdentity& identity, const std::string& outcome,
                             std::int64_t duration_ms, std::int64_t tokens);
    bool RecordNodeFailed(const NodeExecutionIdentity& identity, const std::string& error_code,
                          const std::string& error_message, std::int64_t duration_ms,
                          std::int64_t tokens);
    bool RecordNodeCancelled(const std::string& node_id, const std::string& node_execution_id,
                             const std::string& reason);
    bool RecordNodeSkipped(const std::string& node_id, const std::string& reason);

    // checkpoint:文件先不可变落稳 → workflow.checkpoint.committed(ref/hash/
    // throughSeq/storeRevision)。孤立候选文件不生效。
    std::expected<CheckpointRecord, WorkflowAccountError> CommitCheckpoint(
        const nlohmann::json& store_json);

    // run 终态(幂等:已写过终态的账拒写第二枚)。
    bool RecordRunCompleted(const nlohmann::json& result);
    bool RecordRunFailed(const std::string& error_code, const std::string& error_message);
    bool RecordRunCancelled(const std::string& reason);

    // ---- 非关键投影(写不住记诊断,不拦运行)----

    void RecordNodeWaiting(const std::string& node_id, const std::string& wait_kind,
                           const std::string& reason, const std::string& body = std::string());
    void RecordNodeRetrying(const NodeExecutionIdentity& identity, int max_attempts,
                            const std::string& error_code);
    void RecordBranchStarted(const std::string& node_id, const std::vector<std::string>& branches,
                             int concurrency);
    void RecordJoinCompleted(const std::string& node_id, const std::string& join, int succeeded,
                             int failed, const std::vector<std::string>& unavailable);
    void RecordLoopIterationStarted(const std::string& node_id, int iteration);
    void RecordLoopIterationCompleted(const std::string& node_id, int iteration,
                                      bool condition_met);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit WorkflowRunAccount(std::unique_ptr<Impl> impl);
};

}  // namespace lubancode::workflow
