// Workflow 节点独立 v3 场实现(V3-GAP-05 棒二)。开卷五步照 subagent 的
// 既有件形状(trajectory_subagent_bridge.cpp 的 SpawnSubagentV3),只是:
//   - 子卷落 run 目录(nodes/<nodeExecutionId>/sessions/),不落父场
//     subagents/——workflow 数据归 run,父 session 只持引用(设计 §三);
//   - 委派正文不预写:执行器经 turn 桥的 BeginTurn/RecordInput 落主输入
//     (host_executors 的既有路径),这里只开卷建桥;
//   - systemMeta.cause=workflow_node + nodeExecutionRef,读侧按图索骥。
#include "workflow/node_sessions.hpp"

#include <cstdio>
#include <ctime>
#include <system_error>
#include <utility>

#include "hooks/hash.hpp"  // Sha256Hex:session id 的内容寻址(跨进程不撞)
#include "platform/log_sink.hpp"
#include "runtime/trajectory_session.hpp"
#include "runtime/trajectory_turn_bridge.hpp"
#include "trajectory/directory.hpp"  // GenerateSessionId
#include "trajectory/event.hpp"      // EventScope/Durability
#include "trajectory/v3/subagent.hpp"

namespace lubancode::workflow {

namespace {

namespace v3 = ::lubancode::trajectory::v3;
using trajectory::Durability;
using trajectory::EventScope;

// 节点场 session id:YYYYMMDD-HHMMSS-XXXXXX 形状(单段名过目录门),尾缀
// W+内容寻址(nodeExecutionId|attempt 的 SHA-256 前 12 hex)——进程内计数
// 会随恢复归零,纯计数在同秒撞号(树走的 sessionId 去重会把后开的那场
// 当环丢掉);exec id 含 runId、attempt 逐次递增,内容哈希跨进程不撞。
std::string MintNodeSessionId(const std::string& node_execution_id, int attempt) {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    const std::string digest =
        hooks::Sha256Hex(node_execution_id + "|a" + std::to_string(attempt));
    const std::string suffix = "W" + digest.substr(0, 12);
    return trajectory::GenerateSessionId(parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                                         parts.tm_hour, parts.tm_min, parts.tm_sec, suffix);
}

// 节点场的桥:持子卷写者与会话共享账,Finish 落 session.ended 回末行
// hash(编排账 terminal 事件引用它;与 SubagentBridgeV3Impl 同款)。
class NodeSessionBridgeImpl final : public runtime::TrajectoryWorkflowNodeBridge {
public:
    NodeSessionBridgeImpl(std::unique_ptr<v3::V3Writer> writer,
                          std::unique_ptr<runtime::V3SessionBooks> books,
                          std::unique_ptr<runtime::TrajectoryTurnBridge> bridge,
                          std::string run_id,
                          std::shared_ptr<std::vector<std::string>> errors)
        : writer_(std::move(writer)), books_(std::move(books)), bridge_(std::move(bridge)),
          run_id_(std::move(run_id)), errors_(std::move(errors)) {}

    const std::string& run_id() const override { return run_id_; }
    runtime::TrajectoryTurnBridge& turn_bridge() override { return *bridge_; }

    std::string Finish(bool ok, bool cancelled, const std::string& reason) override {
        if (finished_) {
            return terminal_hash_;
        }
        finished_ = true;
        v3::EventDraft ended;
        ended.kind = v3::EventKindV3::SessionEnded;
        ended.payload = nlohmann::json{
            {"reason", reason.empty() ? (cancelled ? std::string("cancelled")
                                                   : (ok ? std::string("completed")
                                                         : std::string("failed")))
                                      : reason},
            {"closeQuality", ok && !cancelled ? "clean" : "incomplete"}};
        const auto receipt = writer_->AppendEvent(std::move(ended), Durability::PowerLoss);
        terminal_hash_ =
            receipt.status == v3::WriteReceipt::Status::Committed ? receipt.line_hash : std::string();
        return terminal_hash_;
    }

private:
    std::unique_ptr<v3::V3Writer> writer_;
    std::unique_ptr<runtime::V3SessionBooks> books_;
    std::unique_ptr<runtime::TrajectoryTurnBridge> bridge_;
    std::string run_id_;
    std::shared_ptr<std::vector<std::string>> errors_;  // 桥写它,opener 同份读
    std::string terminal_hash_;
    bool finished_ = false;
};

}  // namespace

std::optional<NodeSessionMaterial> NodeSessionMaterial::FromLedger(
    runtime::TrajectorySessionLedger* ledger) {
    if (ledger == nullptr) {
        return std::nullopt;
    }
    v3::V3Writer* writer = ledger->v3_main_writer();
    if (writer == nullptr || writer->broken()) {
        return std::nullopt;  // v2 场/无账:节点照跑,没有独立场(如实降级)
    }
    NodeSessionMaterial material;
    material.parent_writer = writer;
    material.parent_session_id = ledger->session_id();
    material.parent_run_id = writer->run_id();
    material.workspace_key = ledger->workspace_key();
    material.parent_session_dir = ledger->session_dir();
    return material;
}

WorkflowNodeSessions::WorkflowNodeSessions(NodeSessionMaterial material,
                                           std::filesystem::path run_dir,
                                           std::string workflow_run_id)
    : material_(std::move(material)), run_dir_(std::move(run_dir)),
      workflow_run_id_(std::move(workflow_run_id)) {}

std::vector<std::string> WorkflowNodeSessions::recent_errors() const {
    std::lock_guard<std::mutex> lock(errors_mutex_);
    std::vector<std::string> out = io_errors_;
    for (const auto& sink : session_errors_) {
        for (const auto& note : *sink) {
            out.push_back(note);
        }
    }
    return out;
}

std::expected<NodeSessionSpawn, runtime::WorkflowSpawnFailure> WorkflowNodeSessions::Open(
    const NodeExecutionIdentity& identity, const WorkflowNode& node, int attempt) {
    runtime::WorkflowSpawnFailure failure;
    failure.reserved_run_id = identity.attempt_id();
    const auto fail_out = [this, &failure](std::string stage, std::string code, std::string detail,
                                           bool retryable)
        -> std::expected<NodeSessionSpawn, runtime::WorkflowSpawnFailure> {
        failure.stage = std::move(stage);
        failure.error_code = std::move(code);
        failure.detail = std::move(detail);
        failure.retryable = retryable;
        {
            std::lock_guard<std::mutex> lock(errors_mutex_);
            io_errors_.push_back("workflow.node_session_failed:" + failure.stage + ":" +
                                 failure.error_code);
        }
        platform::LogSink::Instance().Error(
            "workflow", "节点场开张失败[" + failure.stage + "]: " + failure.error_code +
                            (failure.detail.empty() ? std::string()
                                                    : " (" + failure.detail + ")"));
        return std::unexpected(std::move(failure));
    };

    // ---- 场身份与目录(§三:nodes/<nodeExecutionId>/sessions/<sid>/)----
    const std::string session_id = MintNodeSessionId(identity.node_execution_id, attempt);
    const std::string session_run_id = identity.attempt_id();
    const std::filesystem::path session_dir =
        run_dir_ / "nodes" / identity.node_execution_id / "sessions" / session_id;
    const std::filesystem::path session_jsonl = session_dir / (session_id + ".jsonl");
    std::error_code ec;
    std::filesystem::create_directories(session_dir, ec);
    if (ec) {
        return fail_out("reserve_stream", "workflow.node_session.mkdir_failed", ec.message(), false);
    }

    // journalPath:相对父 session 目录(/usage 树走按它定位)——契约与读侧
    // 严格对齐:reader.cpp WalkSessionTreeRecursive 拿父账 jsonl 的
    // parent_path() 拼 journalPath 找子账;这里的"父 session 目录"
    // (material_.parent_session_dir,即 ledger->session_dir())就是父账
    // 自己 jsonl 所在的那个目录,两边基准同一个锚点,不该对不上。
    //
    // GAP-05 windows-msvc 真根因(诊断日志钉死,不再是本段的猜测):不在
    // relative() 本身,在读侧拼回去之后没有折叠"../../../…"——父子两棵
    // 树离得越深,未折叠前的字符串越长,windows-msvc CI 临时目录前缀本
    // 就长,量到过未折叠 297 字符、折叠后 192 字符,正好跨过 Win32 传统
    // MAX_PATH=260 这条线,未开长路径支持时直接判"不存在"。修法在
    // reader.cpp:child.jsonl_path 拼出来后补一步 lexically_normal()。
    // 这里的自证保留作为独立防线(relative() 本身若真算错,同一份自证
    // 依然能兜住,退绝对路径分支),不因根因已经找到就撤掉。
    std::string journal_path;
    {
        std::error_code canon_ec;
        const auto parent_canonical =
            std::filesystem::weakly_canonical(material_.parent_session_dir, canon_ec);
        bool ok = !canon_ec;
        std::filesystem::path child_canonical;
        if (ok) {
            child_canonical = std::filesystem::weakly_canonical(session_jsonl, canon_ec);
            ok = !canon_ec;
        }
        std::filesystem::path relative;
        if (ok) {
            relative = std::filesystem::relative(child_canonical, parent_canonical, canon_ec);
            ok = !canon_ec && !relative.empty();
        }
        if (ok) {
            // 兜底验证:按读侧的真实拼法——原始(未 canonicalize)的
            // parent_session_dir 拼上这条 relative、词法规整——必须对得上
            // 原始(未 canonicalize)的 session_jsonl 本尊。此刻文件本身
            // 还没有落地(写者未开卷),但两侧目录前缀都已存在,词法拼接
            // 足以核实这条相对路径按读侧拼法是否真的指得回去。
            const auto rebuilt = (material_.parent_session_dir / relative).lexically_normal();
            ok = rebuilt == session_jsonl.lexically_normal();
        }
        if (ok) {
            journal_path = relative.generic_string();
        } else {
            std::error_code abs_ec;
            journal_path = std::filesystem::absolute(session_jsonl, abs_ec).generic_string();
        }
    }
    if (journal_path.empty()) {
        return fail_out("reserve_stream", "workflow.node_session.no_journal_path",
                        "节点场账卷路径定位失败", false);
    }

    v3::V3Writer& parent = *material_.parent_writer;
    const std::string task_id = parent.NewTaskId();

    // ---- 步 1:父账 subagent.spawn.requested(schema:subagent.* 必带
    // actionId——工作流派工没有父工具调用,拿 attemptId 当关联键,taskArgs
    // 带 nodeExecutionRef 全套,读侧按图索骥)。----
    v3::ChildSessionRef child_ref;
    child_ref.session_id = session_id;
    child_ref.run_id = session_run_id;
    child_ref.journal_path = journal_path;
    v3::ParentActionRef parent_ref;
    parent_ref.session_id = material_.parent_session_id;
    parent_ref.run_id = material_.parent_run_id;
    v3::EventDraft spawn;
    spawn.kind = v3::EventKindV3::SubagentSpawnRequested;
    spawn.action_id = session_run_id;
    spawn.task_id = task_id;
    spawn.payload = nlohmann::json{
        {"taskId", task_id},
        {"childSessionRef", child_ref.ToJson()},
        {"attempt", 1},
        {"parentActionRef", parent_ref.ToJson()},
        {"taskArgs",
         nlohmann::json{{"workflowRunId", workflow_run_id_},
                        {"nodeId", identity.node_id},
                        {"nodeExecutionId", identity.node_execution_id},
                        {"attempt", attempt},
                        {"nodeKind", ToString(node.kind)},
                        {"nodeLabel", node.label}}},
        {"configSnapshot", nlohmann::json{{"parentRunKind", "workflow"}}},
        {"asyncStart", true}};
    const auto spawn_receipt = parent.AppendEvent(std::move(spawn), Durability::PowerLoss);
    if (spawn_receipt.status != v3::WriteReceipt::Status::Committed) {
        return fail_out("reserve_stream", "workflow.node_session.spawn_failed",
                        spawn_receipt.error_code + ": " + spawn_receipt.error_message, true);
    }
    const auto write_spawn_failed = [&parent, &session_run_id, &task_id,
                                     &child_ref](const std::string& phase,
                                                 const std::string& reason) {
        v3::EventDraft failed;
        failed.kind = v3::EventKindV3::SubagentSpawnFailed;
        failed.status = v3::OpStatus::Failed;
        failed.action_id = session_run_id;
        failed.task_id = task_id;
        failed.payload = nlohmann::json{{"taskId", task_id},
                                        {"phase", phase},
                                        {"reason", reason},
                                        {"childSessionRef", child_ref.ToJson()}};
        (void)parent.AppendEvent(std::move(failed), Durability::PowerLoss);
    };

    // ---- 步 2:开子卷。system 留空(执行器第一次模型请求带真 system 时
    // 走 §4.3 三步切换),首行 systemMeta 带派生来源与 nodeExecutionRef。----
    nlohmann::json system_extra = nlohmann::json{
        {"cause", "workflow_node"},
        {"workflowRunId", workflow_run_id_},
        {"nodeId", identity.node_id},
        {"nodeExecutionId", identity.node_execution_id},
        {"attemptId", session_run_id},
        {"spawnEventRef",
         nlohmann::json{{"sessionId", parent.session_id()},
                        {"runId", parent.run_id()},
                        {"seq", spawn_receipt.seq},
                        {"id", spawn_receipt.id},
                        {"hash", spawn_receipt.line_hash}}}};
    v3::V3WriterOptions writer_options;
    writer_options.run_kind = "workflow_node";  // v2 同枚举既有值;读侧识别用
    auto child = v3::V3Writer::Start(session_jsonl, session_id, session_run_id,
                                     /*system_content=*/std::string(), std::move(system_extra),
                                     std::move(writer_options));
    if (!child.has_value()) {
        write_spawn_failed("child_init", child.error());
        return fail_out("recorder_start", "workflow.node_session.child_start_failed", child.error(),
                        true);
    }
    // 先落进 owner(桥只认稳定地址;optional 里那份 move 之后只剩空壳)。
    auto writer_owner = std::make_unique<v3::V3Writer>(std::move(*child));

    // ---- 步 3:父账 subagent.linked(引用子账检查点);关联落稳后执行器
    // 才开始跑。----
    v3::ChildCheckpointRef checkpoint;
    checkpoint.session_id = session_id;
    checkpoint.run_id = session_run_id;
    checkpoint.seq = writer_owner->next_seq() - 1;
    checkpoint.line_hash = writer_owner->last_line_hash();
    v3::EventDraft linked;
    linked.kind = v3::EventKindV3::SubagentLinked;
    linked.status = v3::OpStatus::Done;
    linked.action_id = session_run_id;
    linked.task_id = task_id;
    linked.payload = nlohmann::json{{"taskId", task_id},
                                    {"childCheckpointRef", checkpoint.ToJson()}};
    const auto linked_receipt = parent.AppendEvent(std::move(linked), Durability::PowerLoss);
    if (linked_receipt.status != v3::WriteReceipt::Status::Committed) {
        write_spawn_failed("link", linked_receipt.error_code + " " + linked_receipt.error_message);
        return fail_out("run_started", "workflow.node_session.link_failed",
                        linked_receipt.error_message.empty() ? linked_receipt.error_code
                                                             : linked_receipt.error_message,
                        true);
    }

    // ---- 子轮桥:v3 模式,绑子卷自己的共享账(system 起步为空、首次请求
    // 切换;结果仓开在节点场目录)。 ----
    auto books = std::make_unique<runtime::V3SessionBooks>();
    books->system_content = std::string();
    books->settings_version = 1;
    EventScope identity_scope;
    identity_scope.workspace_key = material_.workspace_key;
    identity_scope.session_id = session_id;
    identity_scope.run_id = session_run_id;
    runtime::TrajectoryTurnBridge::Identity bridge_identity;
    bridge_identity.provider = "workflow";
    bridge_identity.wire = "workflow";
    bridge_identity.channel = "subagent";  // 渠道档闭合枚举里最近的一档
    auto bridge = std::make_unique<runtime::TrajectoryTurnBridge>(
        writer_owner.get(), books.get(), std::move(identity_scope), std::move(bridge_identity));
    // 每场一份错误汇(shared_ptr 保活:桥在 worker 线程写,opener 聚合读;
    // 并发各写各的份,run 收口后读无竞)。诊断留在 opener,recent_errors
    // 聚合给测试与 /doctor。
    auto session_errors = std::make_shared<std::vector<std::string>>();
    bridge->SetErrorSink(session_errors.get());

    NodeSessionSpawn out;
    out.ref.session_id = session_id;
    out.ref.run_id = session_run_id;
    out.ref.journal_path = journal_path;
    {
        std::lock_guard<std::mutex> lock(errors_mutex_);
        session_errors_.push_back(session_errors);
    }
    out.bridge = std::unique_ptr<runtime::TrajectoryWorkflowNodeBridge>(new NodeSessionBridgeImpl(
        std::move(writer_owner), std::move(books), std::move(bridge), session_run_id,
        std::move(session_errors)));
    return out;
}

}  // namespace lubancode::workflow
