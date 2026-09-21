// 轨迹 workflow 编排账桥的实现与开账装配(AR-12 机械拆分:自
// trajectory_session.cpp 按桥类边界拆出,方法体一字未动)。编排 Journal
// 与 node stream 的实现桥住此件;账本面合同见 trajectory_workflow_bridge.hpp
// 与 trajectory_session.hpp 的 SpawnWorkflowRun。

#include "runtime/trajectory_session.hpp"

#include <utility>

#include "hooks/hash.hpp"             // Sha256Hex:exotic 节点名的寻位名
#include "platform/atomic_write.hpp"  // AtomicWriteFile:definition.json 快照
#include "platform/log_sink.hpp"
#include "platform/paths.hpp"
#include "runtime/trajectory_session_impl.hpp"
#include "runtime/trajectory_workflow_bridge.hpp"
#include "trajectory/directory.hpp"  // IsValidSingleSegment/ReserveWorkflow*(残留清理与预留)

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// workflow 编排账(workflow 会话归属统一单):ReserveWorkflowRun/
// ReserveWorkflowNodeStream 的消费方。编排 Journal 与 node stream 的实现
// 桥,合同见 trajectory_workflow_bridge.hpp 的 TrajectoryWorkflowRunBridge。
// ---------------------------------------------------------------------------

namespace {

using trajectory::Actor;
using trajectory::EventKind;
using trajectory::Origin;
using trajectory::RecordReceipt;
using trajectory::Visibility;

// node attempt 账:recorder + 轮桥;Finish 落 run 终态并关柄(子代理桥
// 同款)。
class WorkflowNodeBridgeImpl : public TrajectoryWorkflowNodeBridge {
public:
    WorkflowNodeBridgeImpl(std::unique_ptr<trajectory::TrajectoryRecorder> recorder,
                           std::unique_ptr<TrajectoryTurnBridge> bridge, std::string run_id)
        : recorder_(std::move(recorder)), bridge_(std::move(bridge)), run_id_(std::move(run_id)) {}

    const std::string& run_id() const override { return run_id_; }
    TrajectoryTurnBridge& turn_bridge() override { return *bridge_; }

    std::string Finish(bool ok, bool cancelled, const std::string& reason) override {
        if (finished_) {
            return terminal_hash_;
        }
        finished_ = true;
        const EventKind terminal =
            cancelled ? EventKind::RunCancelled
                      : (ok ? EventKind::RunCompleted : EventKind::RunFailed);
        const auto receipt =
            recorder_->FinishRun(terminal, reason, trajectory::Durability::PowerLoss);
        if (receipt.status == RecordReceipt::Status::Committed) {
            terminal_hash_ = receipt.event_hash;
        } else {
            terminal_hash_.clear();
        }
        (void)recorder_->Close();
        return terminal_hash_;
    }

private:
    std::unique_ptr<trajectory::TrajectoryRecorder> recorder_;
    std::unique_ptr<TrajectoryTurnBridge> bridge_;
    std::string run_id_;
    std::string terminal_hash_;
    bool finished_ = false;
};

// 编排账:workflow.jsonl 的唯一写者。执行器无权碰它(§15.6)。
class WorkflowRunBridgeImpl : public TrajectoryWorkflowRunBridge {
public:
    WorkflowRunBridgeImpl(std::unique_ptr<trajectory::TrajectoryRecorder> recorder,
                          trajectory::TrajectoryDirectory directory,
                          trajectory::RecorderOptions recorder_options,
                          trajectory::TrainingPolicy training_policy, std::string run_id,
                          std::string workflow_id, std::vector<std::string>* io_errors,
                          telemetry::CommitObserver* wake,
                          std::function<std::optional<std::string>()>* node_start_fault)
        : recorder_(std::move(recorder)), directory_(std::move(directory)),
          recorder_options_(std::move(recorder_options)), training_policy_(training_policy),
          run_id_(std::move(run_id)), workflow_id_(std::move(workflow_id)), io_errors_(io_errors),
          wake_(wake), node_start_fault_(node_start_fault) {}

    const std::string& run_id() const override { return run_id_; }
    const std::string& workflow_id() const override { return workflow_id_; }

    // ---- 非关键投影:写不住记诊断,不拦运行 ----

    void RecordNodeWaiting(const std::string& node_id, const std::string& wait_kind,
                           const std::string& body) override {
        nlohmann::json payload{{"node_id", node_id}, {"wait_kind", wait_kind}};
        if (!body.empty()) {
            payload["body"] = body;
        }
        (void)Put(EventKind::WorkflowNodeWaiting, std::move(payload));
    }

    void RecordBranchStarted(const std::string& node_id, const std::vector<std::string>& branches,
                             int concurrency) override {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& branch : branches) {
            list.push_back(branch);
        }
        (void)Put(EventKind::WorkflowBranchStarted,
                  nlohmann::json{{"node_id", node_id},
                                 {"branches", std::move(list)},
                                 {"concurrency", static_cast<std::uint64_t>(concurrency)}});
    }

    void RecordJoinCompleted(const std::string& node_id, const std::string& join, int succeeded,
                             int failed, const std::vector<std::string>& unavailable) override {
        nlohmann::json missing = nlohmann::json::array();
        for (const auto& id : unavailable) {
            missing.push_back(id);
        }
        (void)Put(EventKind::WorkflowJoinCompleted,
                  nlohmann::json{{"node_id", node_id},
                                 {"join", join},
                                 {"succeeded", static_cast<std::uint64_t>(succeeded)},
                                 {"failed", static_cast<std::uint64_t>(failed)},
                                 {"unavailable", std::move(missing)}});
    }

    void RecordLoopIterationStarted(const std::string& node_id, int iteration) override {
        (void)Put(EventKind::WorkflowLoopIterationStarted,
                  nlohmann::json{{"node_id", node_id},
                                 {"iteration", static_cast<std::uint64_t>(iteration)}});
    }

    void RecordLoopIterationCompleted(const std::string& node_id, int iteration,
                                      bool condition_met) override {
        (void)Put(EventKind::WorkflowLoopIterationCompleted,
                  nlohmann::json{{"node_id", node_id},
                                 {"iteration", static_cast<std::uint64_t>(iteration)},
                                 {"condition_met", condition_met}});
    }

    // ---- 关键事实:写不住即 false,调度器停在明确失败态 ----

    bool RecordNodeDispatched(const std::string& node_id, const std::string& node_run_id,
                              int attempt, const std::string& node_kind, int map_item_index,
                              const std::string& input_sha256) override {
        nlohmann::json payload{{"node_id", node_id},
                               {"node_run_id", node_run_id},
                               {"attempt", static_cast<std::uint64_t>(attempt)},
                               {"node_kind", node_kind}};
        if (map_item_index >= 0) {
            payload["map_item_index"] = map_item_index;
        }
        if (!input_sha256.empty()) {
            payload["input_sha256"] = input_sha256;
        }
        trajectory::EventLinks links;
        links.child_run_id = node_run_id;  // 编排→node 边(verifier 交叉核)
        return Put(EventKind::WorkflowNodeDispatched, std::move(payload), std::move(links))
                   .status == RecordReceipt::Status::Committed;
    }

    bool RecordNodeCompleted(const std::string& node_id, const std::string& node_run_id,
                             int attempt, const std::string& outcome, std::int64_t duration_ms,
                             std::int64_t tokens, const std::string& agent_name,
                             const std::string& output_sha256,
                             const std::string& child_terminal_event_hash) override {
        nlohmann::json payload{{"node_id", node_id},
                               {"node_run_id", node_run_id},
                               {"attempt", static_cast<std::uint64_t>(attempt)},
                               {"outcome", outcome},
                               {"duration_ms", duration_ms},
                               {"tokens", tokens}};
        if (!agent_name.empty()) {
            payload["agent"] = agent_name;
        }
        if (!output_sha256.empty()) {
            payload["output_sha256"] = output_sha256;
        }
        // child 边只在子账真有终态 hash 时挂:node stream 没开成(spawn
        // 失败)的失败事实不带 child 引用,免造孤儿边(verifier 的 orphan
        // 检查会把"父账声明派发却无子文件"判成坏账)。
        trajectory::EventLinks links;
        if (!child_terminal_event_hash.empty()) {
            payload["child_terminal_event_hash"] = child_terminal_event_hash;
            links.child_run_id = node_run_id;
        }
        return Put(EventKind::WorkflowNodeCompleted, std::move(payload), std::move(links))
                   .status == RecordReceipt::Status::Committed;
    }

    bool RecordNodeFailed(const std::string& node_id, const std::string& node_run_id, int attempt,
                          const std::string& code, const std::string& error, std::int64_t duration_ms,
                          std::int64_t tokens,
                          const std::string& child_terminal_event_hash) override {
        nlohmann::json payload{{"node_id", node_id},
                               {"node_run_id", node_run_id},
                               {"attempt", static_cast<std::uint64_t>(attempt)},
                               {"code", code},
                               {"duration_ms", duration_ms},
                               {"tokens", tokens}};
        if (!error.empty()) {
            payload["error"] = error.substr(0, 500);
        }
        // 与 completed 同规矩:child 边只挂在场的终态 hash。
        trajectory::EventLinks links;
        if (!child_terminal_event_hash.empty()) {
            payload["child_terminal_event_hash"] = child_terminal_event_hash;
            links.child_run_id = node_run_id;
        }
        return Put(EventKind::WorkflowNodeFailed, std::move(payload), std::move(links))
                   .status == RecordReceipt::Status::Committed;
    }

    void RecordNodeSkipped(const std::string& node_id, const std::string& node_run_id,
                           const std::string& reason) override {
        nlohmann::json payload{{"node_id", node_id}};
        if (!node_run_id.empty()) {
            payload["node_run_id"] = node_run_id;
        }
        if (!reason.empty()) {
            payload["reason"] = reason;
        }
        (void)Put(EventKind::WorkflowNodeSkipped, std::move(payload));
    }

    void RecordNodeRetrying(const std::string& node_id, const std::string& node_run_id,
                            int attempt, int max_attempts, const std::string& code) override {
        (void)Put(EventKind::WorkflowNodeRetrying,
                  nlohmann::json{{"node_id", node_id},
                                 {"node_run_id", node_run_id},
                                 {"attempt", static_cast<std::uint64_t>(attempt)},
                                 {"max_attempts", static_cast<std::uint64_t>(max_attempts)},
                                 {"code", code}});
    }

    bool RecordCheckpointSaved(const std::string& store_sha256) override {
        nlohmann::json payload{{"at_seq", recorder_->next_seq() - 1}};
        if (!store_sha256.empty()) {
            payload["store_sha256"] = store_sha256;
        }
        return Put(EventKind::WorkflowCheckpointSaved, std::move(payload))
                   .status == RecordReceipt::Status::Committed;
    }

    // ---- node attempt 账开张(fail closed;不留 0 字节残留)----

    std::expected<std::unique_ptr<TrajectoryWorkflowNodeBridge>, WorkflowSpawnFailure>
    SpawnNodeStream(const std::string& node_id, const std::string& node_run_id, int attempt,
                    const std::string& node_kind, int map_item_index) override {
        WorkflowSpawnFailure failure;
        failure.reserved_run_id = node_run_id;
        const auto fail_out = [this, &failure, &node_run_id](std::string stage, std::string code,
                                               std::string detail, bool retryable) {
            failure.stage = std::move(stage);
            failure.error_code = std::move(code);
            failure.detail = std::move(detail);
            failure.retryable = retryable;
            // 所有权凭据清理(P0-C 同款):recorder 放干净(Windows 句柄),
            // 再按"本次预留名 + 0 字节"清未提交残留。
            const auto stream = NodeStreamPath(node_run_id);
            if (stream.has_value()) {
                (void)trajectory::DiscardUncommittedStream(*stream);
            }
            if (io_errors_ != nullptr) {
                // AR-03 联动:io_errors_ 指回 ledger 的诊断环,map 并发项的
                // worker 会同时撞子账开张失败——裸 push_back 是数据竞争
                //(TSan 实锤),诊断口也走锁。
                std::lock_guard<std::mutex> lock(io_errors_mutex_);
                io_errors_->push_back("workflow_node.start_failed:" + failure.stage + ":" +
                                      failure.error_code);
            }
            platform::LogSink::Instance().Error(
                "trajectory",
                "node 账开张失败[" + failure.stage + "]: " + failure.error_code +
                    (failure.detail.empty() ? std::string() : " (" + failure.detail + ")"));
            return std::unexpected(std::move(failure));
        };

        // 文件名:node_run_id 合法单段则直用(目录即身份,§3.6);exotic
        // 节点名(非 ASCII 等)换内容寻址名——身份以 stream 内 run_id 为准,
        // 文件名只是寻位。
        const std::string stream_name =
            trajectory::IsValidSingleSegment(node_run_id)
                ? node_run_id
                : "node-" + hooks::Sha256Hex(node_run_id).substr(0, 16);
        auto stream = directory_.ReserveWorkflowNodeStream(run_id_, stream_name);
        if (!stream.has_value()) {
            return fail_out("reserve_stream", "trajectory.workflow_node_stream", stream.error(),
                            /*retryable=*/false);
        }
        trajectory::EventScope scope = recorder_->base_scope();
        scope.run_id = node_run_id;
        scope.run_kind = trajectory::RunKind::WorkflowNode;
        scope.turn_id.reset();
        scope.request_id.reset();
        scope.call_id.reset();
        scope.visibility = {Visibility::HostOnly};
        scope.training_policy = training_policy_;
        scope.actor = Actor::Host;
        scope.origin = Origin::ScheduledHost;
        // P0-C 同款:延迟开卷——正式 .jsonl 由首枚 run.started 提交事务独占
        // 创建,开不成不在盘上留 0 字节正式 stream。
        trajectory::RecorderOptions options = recorder_options_;
        options.defer_stream_create = true;
        if (node_start_fault_ != nullptr && *node_start_fault_ != nullptr) {
            options.inject_submit_reject = [hook = *node_start_fault_](EventKind kind)
                -> std::optional<std::string> {
                if (kind == EventKind::RunStarted && hook != nullptr) {
                    return hook();
                }
                return std::nullopt;
            };
        }
        auto recorder = trajectory::TrajectoryRecorder::Start(
            *stream, directory_.artifacts_root(), scope, std::move(options));
        if (!recorder.has_value()) {
            return fail_out("recorder_start", "trajectory.workflow_node_recorder", recorder.error(),
                            /*retryable=*/false);
        }
        auto recorder_owner =
            std::make_unique<trajectory::TrajectoryRecorder>(std::move(*recorder));
        // run.started 首行(§3.6):编排归属键全上——workflow_run_id/
        // workflow_id/node_id/node_kind/attempt/map_item_index。
        nlohmann::json payload{{"run_kind", "workflow_node"},
                               {"start_reason", "workflow_node_dispatch"},
                               {"workflow_run_id", run_id_},
                               {"workflow_id", workflow_id_},
                               {"node_id", node_id},
                               {"node_kind", node_kind},
                               {"attempt", static_cast<std::uint64_t>(attempt)},
                               {"task_ref", node_id}};
        if (map_item_index >= 0) {
            payload["map_item_index"] = map_item_index;
        }
        trajectory::EventLinks links;
        links.parent_run_id = run_id_;  // 编排→node 边(子侧)
        const auto started = recorder_owner->WriteRunStarted(std::move(payload),
                                                             trajectory::Durability::PowerLoss,
                                                             std::move(links));
        if (started.status != RecordReceipt::Status::Committed) {
            const bool io_failure = started.status == RecordReceipt::Status::IoFailed;
            recorder_owner.reset();  // 先放句柄再清残留(Windows)
            return fail_out("run_started", "trajectory.workflow_node_run_started: " + started.error_code,
                            started.error_message, /*retryable=*/io_failure);
        }

        TrajectoryTurnBridge::Identity identity;
        identity.provider = "workflow";
        identity.wire = "workflow";
        identity.channel = "workflow";
        auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder_owner, scope, identity);
        if (io_errors_ != nullptr) {
            bridge->SetErrorSink(io_errors_);
        }
        if (wake_ != nullptr) {
            bridge->SetCommitWake(
                wake_, "workflows/" + platform::Utf8ToPath(run_id_).generic_string() + "/nodes/" +
                           std::filesystem::path(*stream).filename().generic_string());
        }
        return std::unique_ptr<TrajectoryWorkflowNodeBridge>(
            new WorkflowNodeBridgeImpl(std::move(recorder_owner), std::move(bridge), node_run_id));
    }

    bool broken() const override {
        return recorder_->broken() || degraded_;
    }

    std::string Finish(bool ok, bool cancelled, const std::string& reason) override {
        if (finished_) {
            return terminal_hash_;
        }
        finished_ = true;
        const EventKind terminal =
            cancelled ? EventKind::RunCancelled
                      : (ok ? EventKind::RunCompleted : EventKind::RunFailed);
        const auto receipt =
            recorder_->FinishRun(terminal, reason, trajectory::Durability::PowerLoss);
        if (receipt.status == RecordReceipt::Status::Committed) {
            terminal_hash_ = receipt.event_hash;
            NotifyWake_();
        } else {
            terminal_hash_.clear();
            if (io_errors_ != nullptr) {
                std::lock_guard<std::mutex> lock(io_errors_mutex_);
                io_errors_->push_back("workflow.finish_failed:" + receipt.error_code);
            }
        }
        (void)recorder_->Close();
        return terminal_hash_;
    }

private:
    // node stream 的预留路径(残留清理用;与 SpawnNodeStream 同一命名法)。
    std::expected<std::filesystem::path, std::string> NodeStreamPath(
        const std::string& node_run_id) const {
        const std::string stream_name =
            trajectory::IsValidSingleSegment(node_run_id)
                ? node_run_id
                : "node-" + hooks::Sha256Hex(node_run_id).substr(0, 16);
        return directory_.ReserveWorkflowNodeStream(run_id_, stream_name);
    }

    RecordReceipt Put(EventKind kind, nlohmann::json payload, trajectory::EventLinks links = {}) {
        trajectory::RecordRequest request;
        request.kind = kind;
        request.scope = recorder_->base_scope();
        request.scope.actor = Actor::Host;
        request.scope.origin = Origin::ScheduledHost;
        request.links = std::move(links);
        request.payload = std::move(payload);
        const RecordReceipt receipt =
            recorder_->Record(std::move(request), trajectory::Durability::ProcessCrash);
        if (receipt.status != RecordReceipt::Status::Committed) {
            degraded_ = true;  // 关键与否由调用方分;这里只记健康
            const std::string note = std::string("workflow.fact_rejected: ") +
                                     trajectory::EventKindName(kind) + ":" + receipt.error_code;
            if (io_errors_ != nullptr) {
                // Put 是并发口(map worker 各自落编排事实),诊断同锁。
                std::lock_guard<std::mutex> lock(io_errors_mutex_);
                io_errors_->push_back(note);
            }
            platform::LogSink::Instance().Error("trajectory", "编排事实落不了: " + note);
        } else {
            NotifyWake_();
        }
        return receipt;
    }

    void NotifyWake_() const {
        if (wake_ == nullptr) {
            return;
        }
        telemetry::CommitWake wake;
        const trajectory::EventScope& scope = recorder_->base_scope();
        wake.workspace_key = scope.workspace_key;
        wake.session_id = scope.session_id;
        wake.stream_id = "workflows/" + platform::Utf8ToPath(run_id_).generic_string() +
                         "/workflow.jsonl";
        wake_->Notify(wake);
    }

    std::unique_ptr<trajectory::TrajectoryRecorder> recorder_;
    trajectory::TrajectoryDirectory directory_;
    trajectory::RecorderOptions recorder_options_;
    trajectory::TrainingPolicy training_policy_ = trajectory::TrainingPolicy::Metadata;
    std::string run_id_;
    std::string workflow_id_;
    std::vector<std::string>* io_errors_ = nullptr;
    // 诊断环的并发锁(AR-03 联动):ledger 的 io_errors_ 裸 vector 被多桥
    // 共指;workflow 侧的三个落诊断口(fail_out/Finish/Put)都可能从 map
    // worker 并发进——push_back 必须串行。纯诊断路径,无热回路。
    std::mutex io_errors_mutex_;
    telemetry::CommitObserver* wake_ = nullptr;
    std::function<std::optional<std::string>()>* node_start_fault_ = nullptr;
    std::string terminal_hash_;
    bool degraded_ = false;
    bool finished_ = false;
};

}  // namespace

std::expected<std::unique_ptr<TrajectoryWorkflowRunBridge>, WorkflowSpawnFailure>
TrajectorySessionLedger::SpawnWorkflowRun(
    const std::string& workflow_run_id, const TrajectoryWorkflowRunBridge::DefinitionInfo& definition) {
    if (impl_ == nullptr || impl_->active == nullptr) {
        WorkflowSpawnFailure failure;
        failure.stage = "reserve_run";
        failure.error_code = "trajectory.no_active_session";
        failure.detail = "会话账未开,编排账无处落";
        return std::unexpected(std::move(failure));
    }
    WorkflowSpawnFailure failure;
    failure.reserved_run_id = workflow_run_id;
    const auto fail_out = [this, &failure](std::string stage, std::string code, std::string detail,
                                           bool retryable) {
        failure.stage = std::move(stage);
        failure.error_code = std::move(code);
        failure.detail = std::move(detail);
        failure.retryable = retryable;
        if (impl_ != nullptr && impl_->active != nullptr) {
            const auto stream = impl_->active->directory.ReserveWorkflowRun(failure.reserved_run_id);
            if (stream.has_value()) {
                (void)trajectory::DiscardUncommittedStream(*stream);
            }
        }
        io_errors_.push_back("workflow.start_failed:" + failure.stage + ":" + failure.error_code);
        platform::LogSink::Instance().Error(
            "trajectory", "编排账开张失败[" + failure.stage + "]: " + failure.error_code +
                              (failure.detail.empty() ? std::string() : " (" + failure.detail + ")"));
        return std::unexpected(std::move(failure));
    };

    auto stream = impl_->active->directory.ReserveWorkflowRun(workflow_run_id);
    if (!stream.has_value()) {
        return fail_out("reserve_run", "trajectory.workflow_run", stream.error(),
                        /*retryable=*/false);
    }
    // definition.json 快照(§3.6 目录形状):归一化原文原子落 run 目录,
    // resume/审计按 content_hash 对得上。
    if (!definition.definition_json.empty()) {
        const auto written = platform::AtomicWriteFile(stream->parent_path() / "definition.json",
                                                       definition.definition_json);
        if (!written.has_value()) {
            return fail_out("definition_snapshot", "trajectory.workflow_definition",
                            written.error().code + ": " + written.error().message,
                            /*retryable=*/true);
        }
    }

    trajectory::EventScope scope = impl_->active->main->base_scope();
    scope.run_id = workflow_run_id;
    scope.run_kind = trajectory::RunKind::Workflow;
    scope.turn_id.reset();
    scope.request_id.reset();
    scope.call_id.reset();
    scope.visibility = {Visibility::HostOnly};
    scope.training_policy = impl_->training_policy;
    scope.actor = Actor::Host;
    scope.origin = Origin::ScheduledHost;
    trajectory::RecorderOptions recorder_options = impl_->recorder_options;
    recorder_options.defer_stream_create = true;
    if (impl_->workflow_start_fault != nullptr) {
        recorder_options.inject_submit_reject = [hook = impl_->workflow_start_fault](EventKind kind)
            -> std::optional<std::string> {
            if (kind == EventKind::RunStarted && hook != nullptr) {
                return hook();
            }
            return std::nullopt;
        };
    }
    auto recorder = trajectory::TrajectoryRecorder::Start(*stream,
                                                          impl_->active->directory.artifacts_root(),
                                                          scope, std::move(recorder_options));
    if (!recorder.has_value()) {
        return fail_out("recorder_start", "trajectory.workflow_recorder", recorder.error(),
                        /*retryable=*/false);
    }
    auto recorder_owner = std::make_unique<trajectory::TrajectoryRecorder>(std::move(*recorder));
    // run.started:后台派工语义——parent_run_id 指本场 main run,不带
    // parent_call_id(main 不落派发边;verifier 核 owner 与子终态)。
    // 版本/hash 走 workflow.definition.loaded,首行不重复。
    nlohmann::json payload{{"run_kind", "workflow"},
                           {"start_reason", "workflow_run"},
                           {"workflow_id", definition.workflow_id},
                           {"task_ref", definition.workflow_id}};
    trajectory::EventLinks links;
    links.parent_run_id = impl_->main_run_id;
    const auto started = recorder_owner->WriteRunStarted(std::move(payload),
                                                         trajectory::Durability::PowerLoss,
                                                         std::move(links));
    if (started.status != RecordReceipt::Status::Committed) {
        const bool io_failure = started.status == RecordReceipt::Status::IoFailed;
        recorder_owner.reset();
        return fail_out("run_started", "trajectory.workflow_run_started: " + started.error_code,
                        started.error_message, /*retryable=*/io_failure);
    }
    // workflow.definition.loaded:定义身份与 hash(快照正文已在盘上;cwd
    // 是 session 级事实,session manifest 已记,这里不重复)。
    {
        nlohmann::json def_payload{{"workflow_id", definition.workflow_id},
                                   {"content_hash", definition.content_hash}};
        if (!definition.workflow_version.empty()) {
            def_payload["workflow_version"] = definition.workflow_version;
        }
        trajectory::RecordRequest request;
        request.kind = EventKind::WorkflowDefinitionLoaded;
        request.scope = recorder_owner->base_scope();
        request.scope.actor = Actor::Host;
        request.scope.origin = Origin::ScheduledHost;
        request.payload = std::move(def_payload);
        const auto receipt = recorder_owner->Record(std::move(request), trajectory::Durability::ProcessCrash);
        if (receipt.status != RecordReceipt::Status::Committed) {
            recorder_owner.reset();
            return fail_out("definition_loaded",
                            "trajectory.workflow_definition_loaded: " + receipt.error_code,
                            receipt.error_message,
                            /*retryable=*/receipt.status == RecordReceipt::Status::IoFailed);
        }
    }
    return std::unique_ptr<TrajectoryWorkflowRunBridge>(new WorkflowRunBridgeImpl(
        std::move(recorder_owner), impl_->active->directory, impl_->recorder_options,
        impl_->training_policy, workflow_run_id, definition.workflow_id, &io_errors_,
        impl_->telemetry_wake, &impl_->workflow_node_start_fault));
}

}  // namespace lubancode::runtime
