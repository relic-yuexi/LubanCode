// 轨迹子代理桥的实现与开账装配(AR-12 机械拆分:自 trajectory_session.cpp
// 按桥类边界拆出,方法体一字未动)。TrajectorySubagentBridge 的实现桥与
// TrajectorySessionLedger::SpawnSubagent/SpawnSubagentV3/NoteSubagentStart
// Failed/ChildTerminalHash 住此件;账本面合同见 trajectory_session.hpp 与
// trajectory_subagent_bridge.hpp。

#include "runtime/trajectory_session.hpp"

#include <cstdio>
#include <ctime>
#include <utility>

#include "platform/log_sink.hpp"
#include "platform/paths.hpp"
#include "platform/text_encoding.hpp"  // SanitizeExternalText:子账 payload 账前兜底(UTF-8 清洗门单)
#include "runtime/trajectory_session_impl.hpp"
#include "runtime/trajectory_subagent_bridge.hpp"
#include "trajectory/directory.hpp"          // GenerateSessionId/DiscardUncommittedStream
#include "trajectory/v3/subagent.hpp"        // 接线点 1:子账五步

namespace lubancode::runtime {

// trajectory v3 简称(本件内 v3:: 一律指 trajectory::v3;runtime 命名空间
// 下裸写 v3:: 解析不到 trajectory::v3)。
namespace v3 = ::lubancode::trajectory::v3;

// 原实现文件顶部的类型简称集随段迁移(与主桥同款)。
namespace {

using trajectory::Actor;
using trajectory::Durability;
using trajectory::EventKind;
using trajectory::EventLinks;
using trajectory::EventScope;
using trajectory::Origin;
using trajectory::RecordReceipt;
using trajectory::TrainingPolicy;
using trajectory::Visibility;

}  // namespace

namespace {

// 子代理桥的具体实现:持独立 recorder,Finish 落 run 终态并关柄。
class SubagentBridgeImpl : public TrajectorySubagentBridge {
public:
    SubagentBridgeImpl(std::unique_ptr<trajectory::TrajectoryRecorder> recorder,
                       std::unique_ptr<TrajectoryTurnBridge> bridge, std::string run_id,
                       std::map<std::string, std::string>* terminal_hashes)
        : recorder_(std::move(recorder)), bridge_(std::move(bridge)), run_id_(std::move(run_id)),
          terminal_hashes_(terminal_hashes) {}

    const std::string& run_id() const override { return run_id_; }
    TrajectoryTurnBridge& turn_bridge() override { return *bridge_; }

    std::string Finish(bool ok, const std::string& reason) override {
        if (finished_) {
            return terminal_hash_;
        }
        finished_ = true;
        const auto receipt = recorder_->FinishRun(
            ok ? trajectory::EventKind::RunCompleted : trajectory::EventKind::RunFailed, reason,
            trajectory::Durability::PowerLoss);
        if (receipt.status == trajectory::RecordReceipt::Status::Committed) {
            terminal_hash_ = receipt.event_hash;
        } else {
            terminal_hash_.clear();
        }
        if (terminal_hashes_ != nullptr) {
            (*terminal_hashes_)[run_id_] = terminal_hash_;
        }
        (void)recorder_->Close();
        return terminal_hash_;
    }

private:
    std::unique_ptr<trajectory::TrajectoryRecorder> recorder_;
    std::unique_ptr<TrajectoryTurnBridge> bridge_;
    std::string run_id_;
    std::map<std::string, std::string>* terminal_hashes_;
    std::string terminal_hash_;
    bool finished_ = false;
};

// 接线点 1 的 v3 子代理桥:持子账 V3Writer(sessions/<parent>/subagents/
// <childSessionId>/<childSessionId>.jsonl),Finish 落 session.ended 并回
// 末行 hash(父侧对账用,与 v2 桥同一只口)。子轮桥是 v3 模式桥,绑子
// 账自己的共享账(system 继承父场当前版,结果仓开在子目录)。
class SubagentBridgeV3Impl : public TrajectorySubagentBridge {
public:
    SubagentBridgeV3Impl(std::unique_ptr<v3::V3Writer> writer,
                         std::unique_ptr<V3SessionBooks> books,
                         std::unique_ptr<TrajectoryTurnBridge> bridge, std::string run_id,
                         std::map<std::string, std::string>* terminal_hashes)
        : writer_(std::move(writer)), books_(std::move(books)), bridge_(std::move(bridge)),
          run_id_(std::move(run_id)), terminal_hashes_(terminal_hashes) {}

    const std::string& run_id() const override { return run_id_; }
    TrajectoryTurnBridge& turn_bridge() override { return *bridge_; }

    std::string Finish(bool ok, const std::string& reason) override {
        if (finished_) {
            return terminal_hash_;
        }
        finished_ = true;
        v3::EventDraft ended;
        ended.kind = v3::EventKindV3::SessionEnded;
        ended.payload = nlohmann::json{
            {"reason", reason.empty() ? (ok ? "completed" : "failed") : reason},
            {"closeQuality", ok ? "clean" : "incomplete"}};
        const auto receipt = writer_->AppendEvent(std::move(ended), trajectory::Durability::PowerLoss);
        terminal_hash_ = receipt.status == v3::WriteReceipt::Status::Committed
                             ? receipt.line_hash
                             : std::string();
        if (terminal_hashes_ != nullptr) {
            (*terminal_hashes_)[run_id_] = terminal_hash_;
        }
        return terminal_hash_;
    }

private:
    std::unique_ptr<v3::V3Writer> writer_;
    std::unique_ptr<V3SessionBooks> books_;
    std::unique_ptr<TrajectoryTurnBridge> bridge_;
    std::string run_id_;
    std::map<std::string, std::string>* terminal_hashes_;
    std::string terminal_hash_;
    bool finished_ = false;
};

}  // namespace

// 子代理五步用的子会话 id:YYYYMMDD-HHMMSS-XXXXXX 形状(单段名可过目录
// 门),尾缀 S+计数防同秒撞号。
std::string MintChildSessionId(std::uint64_t counter) {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "S%06llu",
                  static_cast<unsigned long long>(counter % 1000000));
    return trajectory::GenerateSessionId(parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
                                         parts.tm_hour, parts.tm_min, parts.tm_sec, suffix);
}

std::expected<std::unique_ptr<TrajectorySubagentBridge>, SubagentSpawnFailure>
TrajectorySessionLedger::SpawnSubagentV3(const std::string& parent_call_id,
                                         const std::string& task_label,
                                         const std::string& parent_run_id) {
    SubagentSpawnFailure failure;
    failure.stage = "reserve_stream";
    const auto fail_out = [this, &failure](std::string stage, std::string code, std::string detail,
                                           bool retryable) {
        failure.stage = std::move(stage);
        failure.error_code = std::move(code);
        failure.detail = std::move(detail);
        failure.retryable = retryable;
        io_errors_.push_back("subagent.start_failed:" + failure.stage + ":" + failure.error_code);
        platform::LogSink::Instance().Error(
            "trajectory", "v3 子账开张失败[" + failure.stage + "]: " + failure.error_code +
                              (failure.detail.empty() ? std::string() : " (" + failure.detail + ")"));
        return std::unexpected(std::move(failure));
    };
    const std::string agent_run_id =
        "agent-" + std::to_string(++impl_->subagent_counter) + "-" + impl_->main_run_id;
    failure.reserved_run_id = agent_run_id;
    // 账前兜底与 v2 路同一份合同:payload 字符串入账前再洗一道。
    const std::string clean_label = platform::SanitizeExternalText(task_label);
    const std::string clean_parent_call_id = platform::SanitizeExternalText(parent_call_id);
    const std::string clean_parent_run_id = platform::SanitizeExternalText(parent_run_id);

    v3::V3Writer& parent = *impl_->v3_books->writer;
    const std::uint64_t seq_before = parent.next_seq();
    // ---- 步 1:父账 subagent.spawn.requested(持久预留 taskId/子会话引用/
    // parentActionRef/任务参数)。parentActionRef 查共享账的声明表:provider
    // 调用号 -> v3 actionId;没声明的调用如实记 provider 号,不冒充。
    const auto declared_it = impl_->v3_books->declared_actions.find(clean_parent_call_id);
    const bool declared = declared_it != impl_->v3_books->declared_actions.end();
    v3::ParentActionRef parent_ref;
    parent_ref.session_id = impl_->active->session_id();
    parent_ref.run_id = clean_parent_run_id.empty() ? impl_->main_run_id : clean_parent_run_id;
    parent_ref.turn_id = declared ? declared_it->second.turn_id : std::string();
    parent_ref.step_id = declared ? declared_it->second.step_id : std::string();
    parent_ref.action_id = declared ? declared_it->second.action_id : clean_parent_call_id;
    parent_ref.declared_message_ref = declared ? declared_it->second.message_id : std::string();

    const std::string child_session_id = MintChildSessionId(impl_->subagent_counter);
    v3::ChildSessionRef child_ref;
    child_ref.session_id = child_session_id;
    child_ref.run_id = agent_run_id;
    child_ref.journal_path = "subagents/" + child_session_id + "/" + child_session_id + ".jsonl";

    auto spawn = v3::SubagentSpawn::Request(
        parent, parent_ref.action_id, parent_ref.turn_id, parent_ref.step_id,
        parent.NewTaskId(), child_ref, parent_ref,
        nlohmann::json{{"taskLabel", clean_label}}, nlohmann::json{{"parentRunKind", "main"}},
        trajectory::Durability::PowerLoss);
    if (parent.broken() || parent.next_seq() != seq_before + 1) {
        return fail_out("reserve_stream", "trajectory.subagent_v3_request",
                        "父账 subagent.spawn.requested 落不了", /*retryable=*/true);
    }
    // ---- 步 2:建子目录开子账(首行 system 带派生来源 + 委派 user +
    // task.started)。落稳才返检查点;此前不执行任何副作用。
    const std::string child_system = impl_->v3_books->system_content;
    auto bootstrapped = spawn.BootstrapChild(parent, agent_run_id, child_system, clean_label,
                                             trajectory::Durability::PowerLoss);
    if (!bootstrapped.error.empty()) {
        (void)spawn.Fail(parent, "child_init", bootstrapped.error, trajectory::Durability::PowerLoss);
        return fail_out("recorder_start", "trajectory.subagent_v3_bootstrap", bootstrapped.error,
                        /*retryable=*/true);
    }
    // ---- 步 3:父账 subagent.linked(引用子账检查点);关联落稳后子账
    // 才开始执行。
    const auto linked = spawn.Link(parent, bootstrapped.checkpoint, trajectory::Durability::PowerLoss);
    if (linked.status != v3::WriteReceipt::Status::Committed) {
        (void)spawn.Fail(parent, "link", linked.error_code + " " + linked.error_message,
                         trajectory::Durability::PowerLoss);
        return fail_out("run_started", "trajectory.subagent_v3_link",
                        linked.error_message.empty() ? linked.error_code : linked.error_message,
                        /*retryable=*/true);
    }
    // 子轮桥:v3 模式,绑子账自己的共享账(system 继承父场当前版,结果仓
    // 开在子 session 目录)。先把子写者落进 owner(桥只认稳定地址),
    // 再建桥——optional 里那份 move 之后只剩空壳,桥指过去必悬。
    auto child_writer_owner =
        std::make_unique<v3::V3Writer>(std::move(*bootstrapped.child_writer));
    auto child_books = std::make_unique<V3SessionBooks>();
    child_books->system_content = child_system;
    child_books->settings_version = 1;
    trajectory::EventScope identity_scope;
    identity_scope.workspace_key = impl_->active->manifest.workspace_key;
    identity_scope.session_id = child_session_id;
    identity_scope.run_id = agent_run_id;
    TrajectoryTurnBridge::Identity identity;
    identity.provider = "subagent";
    identity.wire = "subagent";
    identity.channel = "subagent";
    auto child_bridge = std::make_unique<TrajectoryTurnBridge>(
        child_writer_owner.get(), child_books.get(), std::move(identity_scope),
        std::move(identity));
    child_bridge->SetErrorSink(&io_errors_);
    if (impl_->telemetry_wake != nullptr) {
        child_bridge->SetCommitWake(impl_->telemetry_wake, child_ref.journal_path);
    }
    return std::unique_ptr<TrajectorySubagentBridge>(new SubagentBridgeV3Impl(
        std::move(child_writer_owner), std::move(child_books), std::move(child_bridge),
        agent_run_id, &impl_->child_terminal_hashes));
}

std::expected<std::unique_ptr<TrajectorySubagentBridge>, SubagentSpawnFailure>
TrajectorySessionLedger::SpawnSubagent(const std::string& parent_call_id, const std::string& task_label,
                                       const std::string& parent_run_id) {
    if (impl_ == nullptr || impl_->active == nullptr) {
        SubagentSpawnFailure failure;
        failure.stage = "reserve_stream";
        failure.error_code = "trajectory.no_active_session";
        failure.detail = "会话账未开,子账无处落";
        return std::unexpected(std::move(failure));
    }
    // 接线点 1:父会话是 v3 场 → 子账走 v3::SubagentSpawn 五步
    //(§4.32:requested → 建子目录开卷 → linked → 子账独立 → 终态先落子
    // 账)。子账跟随父会话,不单独读开关。v2 父场走下方原路一字不动。
    if (impl_->active->is_v3()) {
        return SpawnSubagentV3(parent_call_id, task_label, parent_run_id);
    }
    const std::string agent_run_id =
        "agent-" + std::to_string(++impl_->subagent_counter) + "-" + impl_->main_run_id;
    SubagentSpawnFailure failure;
    failure.reserved_run_id = agent_run_id;
    // 本轮失败共同收尾:recorder 先放干净(Windows 攥着句柄删不掉文件),
    // 再按所有权凭据清未提交的 0 字节残留(路径=本次预留名、大小=0)。
    const auto fail_out = [this, &failure](std::string stage, std::string code, std::string detail,
                                           bool retryable) {
        failure.stage = std::move(stage);
        failure.error_code = std::move(code);
        failure.detail = std::move(detail);
        failure.retryable = retryable;
        if (impl_ != nullptr && impl_->active != nullptr) {
            const auto stream_path = impl_->active->directory.ReserveSubagentStream(failure.reserved_run_id);
            if (stream_path.has_value()) {
                (void)trajectory::DiscardUncommittedStream(*stream_path);
            }
        }
        io_errors_.push_back("subagent.start_failed:" + failure.stage + ":" + failure.error_code);
        platform::LogSink::Instance().Error(
            "trajectory", "子账开张失败[" + failure.stage + "]: " + failure.error_code +
                              (failure.detail.empty() ? std::string() : " (" + failure.detail + ")"));
        return std::unexpected(std::move(failure));
    };
    auto stream = impl_->active->directory.ReserveSubagentStream(agent_run_id);
    if (!stream.has_value()) {
        return fail_out("reserve_stream", "trajectory.subagent_stream", stream.error(),
                        /*retryable=*/false);
    }
    trajectory::EventScope scope = impl_->active->main->base_scope();
    scope.run_id = agent_run_id;
    scope.run_kind = trajectory::RunKind::Subagent;
    scope.turn_id.reset();
    scope.request_id.reset();
    scope.call_id.reset();
    scope.visibility = {Visibility::HostOnly};
    scope.training_policy = impl_->training_policy;
    // P0-C:子账走延迟开卷——正式 .jsonl 在首枚 run.started 提交事务里独占
    // 创建;开不成/写不进都不会留下 0 字节正式 stream。
    trajectory::RecorderOptions recorder_options = impl_->recorder_options;
    recorder_options.defer_stream_create = true;
    if (impl_->subagent_start_fault != nullptr) {
        recorder_options.inject_submit_reject = [hook = impl_->subagent_start_fault](
                                                     trajectory::EventKind kind)
                                                     -> std::optional<std::string> {
            if (kind == trajectory::EventKind::RunStarted && hook != nullptr) {
                return hook();
            }
            return std::nullopt;
        };
    }
    auto recorder = trajectory::TrajectoryRecorder::Start(*stream, impl_->active->directory.artifacts_root(),
                                                          scope, std::move(recorder_options));
    if (!recorder.has_value()) {
        return fail_out("recorder_start", "trajectory.subagent_recorder", recorder.error(),
                        /*retryable=*/false);
    }
    // 账前兜底(UTF-8 清洗门单):入口消毒在 AgentTool::ExecuteDispatch,这里
    // 写 run.started 前对进 payload 的字符串再洗一道——防别的调用方
    //(workflow/peer 派工路)绕过工具入口带坏字节直灌,账层只认不洗,坏字节
    // 会把整场 spawn 拒成 fail closed。与入口同一份合同(platform::
    // SanitizeExternalText),幂等:入口洗过的合法串这里零成本原样放行。
    // parent_run_id/parent_call_id 是宿主发的 id,照洗是保险,不另外出声。
    const std::string clean_label = platform::SanitizeExternalText(task_label);
    const std::string clean_parent_call_id = platform::SanitizeExternalText(parent_call_id);
    const std::string clean_parent_run_id = platform::SanitizeExternalText(parent_run_id);
    if (clean_label != task_label) {
        platform::LogSink::Instance().Warn(
            "trajectory", "子账 task_label 含 " +
                              std::to_string(platform::CountInvalidUtf8Sites(task_label)) +
                              " 处非法 UTF-8,入账前已消毒(调用方绕过了工具入口的清洗门)");
    }
    // 先把 recorder 落到堆上再让桥引用它——expected 里的值 move 走之后,
    // 引用会悬在 moved-from 壳上(桥的 recorder_ 是裸引用)。
    auto recorder_owner = std::make_unique<trajectory::TrajectoryRecorder>(std::move(*recorder));
    // run.started:父子边界(§3.5/§6.3)——relations 带 parent_run_id 与
    // parent_call_id,正文只有任务标签,不带父会话细账。writer_version/
    // min_reader_version 由 WriteRunStarted 统一落(P0-6 收口,不再手填)。
    nlohmann::json payload = nlohmann::json{{"run_kind", "subagent"},
                                            {"start_reason", "agent_tool_dispatch"}};
    if (!clean_label.empty()) {
        payload["task_ref"] = clean_label;
    }
    trajectory::EventLinks links;
    // 嵌套轨迹边(递归派工单 P1-2):parent_run_id 非空 = 嵌套派工——它的
    // 父亲是派出它的那只子代理自己的 run,不是 main;空串(main 直派)按
    // 从前行为落回本场 main_run_id。
    links.parent_run_id = clean_parent_run_id.empty() ? impl_->main_run_id : clean_parent_run_id;
    if (!clean_parent_call_id.empty()) {
        links.parent_call_id = clean_parent_call_id;
    }
    const auto started = recorder_owner->WriteRunStarted(std::move(payload),
                                                          trajectory::Durability::PowerLoss,
                                                          std::move(links));
    if (started.status != trajectory::RecordReceipt::Status::Committed) {
        const bool io_failure = started.status == trajectory::RecordReceipt::Status::IoFailed;
        // recorder 攥着已开卷的句柄:先放掉再清残留。
        recorder_owner.reset();
        return fail_out("run_started",
                        "trajectory.subagent_run_started: " + started.error_code,
                        started.error_message, /*retryable=*/io_failure);
    }

    TrajectoryTurnBridge::Identity identity;
    identity.provider = "subagent";
    identity.wire = "subagent";
    identity.channel = "subagent";
    auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder_owner, scope, identity);
    // T1 committed wake:子账 stream 也投(subagents/<run>.jsonl,§14.3 多
    // stream 各自 cursor)。
    if (impl_->telemetry_wake != nullptr) {
        bridge->SetCommitWake(impl_->telemetry_wake,
                              "subagents/" + std::filesystem::path(*stream).filename().generic_string());
    }
    return std::unique_ptr<TrajectorySubagentBridge>(new SubagentBridgeImpl(
        std::move(recorder_owner), std::move(bridge), agent_run_id, &impl_->child_terminal_hashes));
}

void TrajectorySessionLedger::NoteSubagentStartFailed(const SubagentSpawnFailure& failure,
                                                      const std::string& parent_run_id,
                                                      const std::string& parent_call_id,
                                                      const std::string& turn_id) {
    trajectory::TrajectoryRecorder* recorder = main();
    if (recorder == nullptr) {
        return;  // main 都没了,诊断只能进 io_errors(fail_out 那侧已记)
    }
    // 持有者与 SpawnSubagent 的 relations 同一口径:main 直派(空串)记
    // main_run_id;嵌套派工记派工者自己的 run。
    const std::string owner_run_id =
        parent_run_id.empty() ? impl_->main_run_id : parent_run_id;
    trajectory::RecordRequest request;
    request.kind = trajectory::EventKind::SubagentRunStartFailed;
    request.scope = recorder->base_scope();
    request.scope.actor = trajectory::Actor::Tool;
    request.scope.origin = trajectory::Origin::SubagentTool;
    request.scope.visibility = {Visibility::HostOnly};
    // 子账开张失败是宿主侧诊断事实,不进训练集。
    request.scope.training_policy = trajectory::TrainingPolicy::Exclude;
    request.scope.turn_id = turn_id.empty() ? std::optional<std::string>() : std::optional<std::string>(turn_id);
    request.scope.call_id =
        parent_call_id.empty() ? std::optional<std::string>() : std::optional<std::string>(parent_call_id);
    nlohmann::json payload = nlohmann::json{{"stage", failure.stage},
                                            {"error_code", failure.error_code},
                                            {"parent_run_id", owner_run_id}};
    if (!failure.detail.empty()) {
        // 事件只记稳定码与引用,不抄敏感绝对路径:io 细节里的会话目录
        // 原文换占位符。
        std::string detail = failure.detail;
        if (impl_ != nullptr && impl_->active != nullptr) {
            const std::string session_root =
                platform::PathToUtf8(impl_->active->directory.session_dir());
            const auto pos = detail.find(session_root);
            if (pos != std::string::npos) {
                detail.replace(pos, session_root.size(), "<session_dir>");
            }
        }
        payload["detail"] = std::move(detail);
    }
    if (!parent_call_id.empty()) {
        payload["parent_call_id"] = parent_call_id;
    }
    if (!failure.reserved_run_id.empty()) {
        payload["reserved_run_id"] = failure.reserved_run_id;
    }
    // stream_ref:session 相对引用(subagents/<file>.jsonl),不写绝对路径。
    if (!failure.reserved_run_id.empty() && impl_ != nullptr && impl_->active != nullptr) {
        payload["stream_ref"] = "subagents/" + failure.reserved_run_id + ".jsonl";
    }
    payload["retryable"] = failure.retryable;
    request.payload = std::move(payload);
    const auto receipt = recorder->Record(std::move(request), trajectory::Durability::PowerLoss);
    if (receipt.status != trajectory::RecordReceipt::Status::Committed) {
        const std::string note =
            "subagent.run.start_failed:" + receipt.error_code +
            (receipt.error_message.empty() ? std::string() : " (" + receipt.error_message + ")");
        io_errors_.push_back(note);
        platform::LogSink::Instance().Error("trajectory", "子账开张失败事件落不了: " + note);
        return;
    }
    NotifyCommitted_();
}

std::optional<std::string> TrajectorySessionLedger::ChildTerminalHash(const std::string& agent_run_id) const {
    const auto it = impl_->child_terminal_hashes.find(agent_run_id);
    if (it == impl_->child_terminal_hashes.end()) {
        return std::nullopt;
    }
    return it->second;
}

}  // namespace lubancode::runtime
