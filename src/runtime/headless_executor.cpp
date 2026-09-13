// HeadlessExecutor 实现(常驻总装 V1)。装配合同见头文件。
#include "runtime/headless_executor.hpp"

#include <fstream>
#include <iterator>
#include <utility>

#include "agent/loop.hpp"
#include "platform/atomic_write.hpp"
#include "platform/sha256.hpp"
#include "runtime/agent_channel_engine.hpp"   // ApplyChannelToolPolicy(共用交集)
#include "runtime/channel_session_host.hpp"   // ChannelConfirmAllows(fail closed)
#include "runtime/hook_host_services.hpp"     // DefaultHookServiceCenter
#include "runtime/middleware_runtime.hpp"
#include "runtime/middleware_v3_sink.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/v3/session_switch.hpp"   // FindV3SessionStream
#include "tools/path_utils.hpp"
#include "workspace/identity.hpp"

namespace lubancode::runtime {

namespace {

std::optional<std::string> ReadFileText(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return std::nullopt;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

// message body 的 role(V3 assistant 定稿的 body 形状见 v3 桥
// V3OutputCompleted:{"role":"assistant","content":[{"type":"text",...}]}).
std::string MessageRole(const trajectory::v3::MessageLine& line) {
    if (!line.message.is_object() || !line.message.contains("role") ||
        !line.message["role"].is_string()) {
        return std::string();
    }
    return line.message["role"].get<std::string>();
}

std::string MessageTextBlocks(const trajectory::v3::MessageLine& line) {
    std::string text;
    if (!line.message.is_object() || !line.message.contains("content") ||
        !line.message["content"].is_array()) {
        return text;
    }
    for (const auto& block : line.message["content"]) {
        if (block.is_object() && block.contains("type") && block["type"] == "text" &&
            block.contains("text") && block["text"].is_string()) {
            text += block["text"].get<std::string>();
        }
    }
    return text;
}

}  // namespace

// ---------------------------------------------------------------------------
// 冻结策略纯函数:执行路与恢复路共用
// ---------------------------------------------------------------------------

ReplySelectionPlan PlanReplySelection(const trajectory::v3::V3Ledger& ledger,
                                      const std::string& turn_id) {
    ReplySelectionPlan plan;
    plan.turn_id = turn_id;
    plan.selection_id = "sel-" + turn_id;
    // 落盘序扫:turn 匹配的最后一条 assistant message = 最终正文来源。
    const trajectory::v3::MessageLine* final_assistant = nullptr;
    for (const auto& message : ledger.messages) {
        if (message.turn_id.has_value() && *message.turn_id == turn_id &&
            MessageRole(message) == "assistant") {
            final_assistant = &message;
        }
    }
    if (final_assistant == nullptr) {
        plan.error = "turn 内无 assistant 消息(生成未完成或未开始)";
        return plan;
    }
    plan.text = MessageTextBlocks(*final_assistant);
    if (plan.text.empty()) {
        plan.error = "最终 assistant 消息无 text 正文";
        return plan;
    }
    plan.source_message_ref = final_assistant->message_id;
    // completedEventRef:本轮最后的 model.response.completed 事件
    //(选择事实引用对应 V3 终态;V3 无 turn 终态行,这是最近似的终态事实)。
    for (const auto& event : ledger.events) {
        if (event.turn_id.has_value() && *event.turn_id == turn_id &&
            event.kind == trajectory::v3::EventKindV3::ModelResponseCompleted) {
            plan.completed_event_ref = event.event_id;
        }
    }
    plan.ok = true;
    return plan;
}

bool SelectionAlreadyCommitted(const trajectory::v3::V3Ledger& ledger,
                               const std::string& selection_id) {
    for (const auto& event : ledger.events) {
        if (event.kind == trajectory::v3::EventKindV3::ReplySelectionCommitted &&
            event.payload.is_object() && event.payload.contains("selectionId") &&
            event.payload["selectionId"].is_string() &&
            event.payload["selectionId"].get<std::string>() == selection_id) {
            return true;
        }
    }
    return false;
}

CommitReplySelectionResult CommitReplySelection(trajectory::v3::V3Writer* writer,
                                                const std::filesystem::path& replies_dir,
                                                const ReplySelectionPlan& plan,
                                                const std::string& session_id) {
    CommitReplySelectionResult result;
    if (!plan.ok) {
        result.error_code = "selection.plan_invalid";
        result.error = plan.error;
        return result;
    }
    // 幂等查重由调用方先读流完成(SelectionAlreadyCommitted),本函数只管
    // "原件先落稳、事实后提交"这一次落序。
    // 原件先落稳(§3 回复次序):replies/<selectionId>.txt。已在且 hash
    // 相符 = 可直接续(崩在原件后、选择事实前);不符 = 拒(已提交原件
    // 不许覆盖,§11.5)。
    const std::filesystem::path artifact = replies_dir / (plan.selection_id + ".txt");
    const std::string sha = platform::Sha256Hex(plan.text);
    if (const auto existing = ReadFileText(artifact); existing.has_value()) {
        if (platform::Sha256Hex(*existing) != sha) {
            result.error_code = "selection.artifact_failed";
            result.error = "回复原件已在但内容对不上(不覆盖已提交原件)";
            return result;
        }
    } else {
        std::error_code ec;
        std::filesystem::create_directories(replies_dir, ec);
        const auto write = platform::AtomicWriteFile(artifact, plan.text,
                                                     platform::WriteDurability::ProcessCrashDurability);
        if (!write.has_value()) {
            result.error_code = "selection.artifact_failed";
            result.error = write.error().code + ": " + write.error().message;
            return result;
        }
    }
    // 选择事实后提交:reply.selection.committed(PowerLoss)。
    nlohmann::json artifact_ref = nlohmann::json::object({
        {"artifactId", plan.selection_id},
        {"kind", "report"},
        {"path", tools::PathToUtf8(artifact)},
        {"sha256", sha},
        {"bytes", plan.text.size()},
        {"mediaType", "text/plain"},
    });
    trajectory::v3::EventDraft draft;
    draft.kind = trajectory::v3::EventKindV3::ReplySelectionCommitted;
    draft.turn_id = plan.turn_id;
    draft.payload = nlohmann::json::object({
        {"selectionId", plan.selection_id},
        {"deliveryTarget", "local:file"},
        {"formatVersion", "text/plain@1"},
        {"ordinal", 1},
        {"sourceMessageRef", plan.source_message_ref},
        {"artifactRef", std::move(artifact_ref)},
    });
    if (!plan.completed_event_ref.empty()) {
        draft.payload["completedEventRef"] = plan.completed_event_ref;
    }
    draft.payload["sessionId"] = session_id;  // 诊断冗余:恢复器定位场
    const auto receipt = writer->AppendEvent(std::move(draft), trajectory::v3::Durability::PowerLoss);
    if (receipt.status != trajectory::v3::WriteReceipt::Status::Committed) {
        result.error_code = "selection.append_failed";
        result.error = receipt.error_code + ": " + receipt.error_message;
        return result;
    }
    result.committed = true;
    result.written_now = true;
    return result;
}

// ---------------------------------------------------------------------------
// HeadlessExecutor
// ---------------------------------------------------------------------------

HeadlessExecutor::HeadlessExecutor(api::Backend& backend, tools::ToolRegistry& registry,
                                   Options options)
    : backend_(backend), registry_(registry), options_(std::move(options)) {}

HeadlessExecutor::Result HeadlessExecutor::Execute(
    const std::string& prompt, const HeadlessWorkBinding& binding,
    const std::function<void(const std::string&, const std::string&)>& on_bound,
    const std::atomic<bool>* cancel) {
    Result result;

    // 1) 开场:三端同一服务路。launch_cwd/one_shot 留缺省——Gateway 场
    //    是常驻自动任务,不是 CLI 单发;training_policy 走 Metadata 缺省。
    SessionLaunchRequest launch;
    launch.cwd_utf8 = options_.cwd_utf8;
    launch.lubancode_version = options_.lubancode_version;
    launch.workspaces_root = options_.workspaces_root;
    launch.workspace_identity = workspace::MakeFallbackIdentity(options_.workspace_root);
    SessionService service(launch);
    if (service.runtime() == nullptr) {
        result.error_code = "gateway.launch_failed";
        result.error = service.launch_error();
        return result;
    }
    if (!service.v3_format()) {
        // 单子 §二:Gateway 新执行固定 V3;遇 v2 新写配置明报拒绝。
        (void)service.Close("gateway_requires_v3");  // 拒绝场也封口,不留悬账
        result.error_code = "gateway.requires_v3";
        result.error = "当前配置会开 v2 会话;Gateway 自动任务只跑 V3 场"
                       "(LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS 不能为 0)";
        return result;
    }
    TrajectorySessionLedger* ledger = service.trajectory();
    trajectory::v3::V3Writer* v3_writer = ledger->v3_main_writer();
    result.session_id = ledger->session_id();

    // 2) 受理:幂等键 = workId(同 work 重发不重复执行);原件与账行都
    //    落稳才回 accepted(V0 受理底线)。
    SessionService::InputRequest input;
    input.client_operation_id = binding.work_id;
    input.text = prompt;
    const auto receipt = service.SubmitInput(input);
    if (!receipt.accepted && !receipt.duplicate) {
        result.error_code = "gateway.input_rejected";
        result.error = receipt.error_code;
        return result;
    }
    // 派发面:dispatched 事实落稳才算取出;落不稳即停(写盘失败不当作
    // 消费成功)。
    const auto pop = service.PopPendingInput();
    if (pop.status == SessionService::PendingPop::Status::WriteFailed) {
        result.error_code = "gateway.input_rejected";
        result.error = "operation.append_failed: 派发事实落不了盘";
        return result;
    }
    const std::string effective_prompt =
        pop.status == SessionService::PendingPop::Status::Ok ? pop.input.text : prompt;

    // 3) turn 身份与绑定:事件适配器 mint turnId → 领域绑定(on_bound,
    //    泵落 occurrence.bound)→ V3 gateway.work.bound(恢复反查的锚)。
    TurnEventAdapter turn_events = service.runtime()->MakeTurnAdapter();
    const std::string turn_id = turn_events.Start();
    result.turn_id = turn_id;
    if (on_bound) {
        on_bound(result.session_id, turn_id);
    }
    {
        trajectory::v3::EventDraft bound_draft;
        bound_draft.kind = trajectory::v3::EventKindV3::GatewayWorkBound;
        bound_draft.turn_id = turn_id;
        bound_draft.payload = nlohmann::json::object({
            {"workId", binding.work_id},
            {"sourceKind", binding.source_kind},
            {"sourceId", binding.source_id},
            {"ownerEpoch", binding.owner_epoch},
            {"attempt", binding.attempt},
            {"inputRef", receipt.input_id},
        });
        const auto bound_receipt =
            v3_writer->AppendEvent(std::move(bound_draft), trajectory::v3::Durability::PowerLoss);
        if (bound_receipt.status != trajectory::v3::WriteReceipt::Status::Committed) {
            result.error_code = "gateway.launch_failed";
            result.error = "gateway.work.bound 落不了账: " + bound_receipt.error_code;
            return result;
        }
    }

    // 4) hook 四点(与渠道路同一 runtime 派发点,不另接一套)。
    hooks::HookDispatcher* dispatcher = options_.hook_dispatcher;
    BindMiddlewareSessionWriter(dispatcher, &DefaultHookServiceCenter(),
                                v3_writer);
    std::string effective_text = effective_prompt;
    const MiddlewareHookContext middleware_context = [&] {
        MiddlewareHookContext context;
        context.turn_id = turn_id;
        context.origin = "human";
        context.purpose = "automation";
        context.delivery_mode = "direct";
        return context;
    }();
    const PreUserGate pre_user = RunPreUserMiddleware(dispatcher, effective_text, middleware_context);
    if (pre_user.blocked) {
        result.error_code = "gateway.turn_failed";
        result.error = "PreUser 钩子阻断本轮: " + pre_user.block_reason;
        return result;
    }
    if (pre_user.dispatched && pre_user.rewritten) {
        effective_text = pre_user.prompt;
    }
    const PostUserAppend post_user =
        RunPostUserMiddleware(dispatcher, effective_text, middleware_context);
    if (post_user.blocked) {
        result.error_code = "gateway.turn_failed";
        result.error = "PostUser 钩子阻断本轮: " + post_user.block_reason;
        return result;
    }
    api::Message user_message{api::Role::User, {api::TextBlock{effective_text}}};
    for (const std::string& append : post_user.context_appends) {
        user_message.content.push_back(
            api::TextBlock{"[PostUser 钩子附加上下文,非用户手敲]\n" + append});
    }

    // 5) 执行:Agent + TurnWiring + 轮桥 + 工具栅栏(AgentChannelEngine
    //    的遗留缺口在此补齐:ToolTraceHub 挂桥,真实工具 Action 落账)。
    auto trajectory_bridge = ledger->NewTurnBridge({"", options_.wire_name, "gateway"});
    if (trajectory_bridge != nullptr) {
        trajectory_bridge->BeginTurn(turn_id, "scheduled_host");
        trajectory_bridge->RecordInput(user_message);
    }
    agent::AgentProfile profile;
    profile.request.model = options_.model;
    profile.runtime.max_steps_per_turn = options_.max_steps_per_turn;
    profile.runtime.max_wall_secs = options_.max_wall_secs;
    profile.runtime.max_total_tokens = options_.max_total_tokens;
    profile = ApplyChannelToolPolicy(std::move(profile), options_.tools);
    agent::Agent loop_agent(backend_, registry_, std::move(profile));

    agent::TurnWiring wiring;
    wiring.events = &turn_events;
    wiring.boundary_recorder = trajectory_bridge.get();
    wiring.turn_id = turn_id;
    // 工具 artifact 落位与 one_shot 同款:会话档内容寻址,随账本持久。
    const std::filesystem::path artifacts_dir = ledger->session_dir() / "artifacts" / "sha256";
    wiring.tool_artifact_dir = artifacts_dir.generic_string();
    const channel::ToolRoutePolicy& tools_policy = options_.tools;
    wiring.on_tool_confirm = [&tools_policy](const std::string& /*tool_use_id*/,
                                             const std::string& name,
                                             const nlohmann::json& /*input*/) {
        return ChannelConfirmAllows(tools_policy, name);
    };
    wiring.on_tool_denial_text = [](const std::string& /*tool_use_id*/,
                                    const std::string& name) {
        return "无人值守任务没有审批渠道,工具 " + name +
               " 未在允许名单明确放行,已拒绝执行。";
    };
    if (HasPreRequestMiddleware(dispatcher)) {
        wiring.on_pre_request_hooks = [dispatcher](const std::string& step_id,
                                                   const std::string& turn_id_,
                                                   const nlohmann::json& frozen_request_snapshot,
                                                   const PreRequestBudget& budget) {
            MiddlewareHookContext context;
            context.turn_id = turn_id_;
            context.step_id = step_id;
            context.purpose = "automation";
            const PreRequestStages stages =
                RunPreRequestMiddleware(dispatcher, frozen_request_snapshot, budget, context);
            if (!stages.dispatched || stages.decision == "allow") {
                return std::string();
            }
            return "PreRequest 钩子拦下本次请求[" + stages.decision + "]: " + stages.reason;
        };
    }
    ToolTraceHub trace_hub(ProcessIdAuthority());
    trace_hub.AttachTrajectory(trajectory_bridge.get());
    trace_hub.Install(loop_agent, wiring, service.runtime()->thread_id(), turn_id);

    const auto outcome = agent::AgentLoop::Run(loop_agent, user_message, wiring, cancel);

    // 6) 收口:turn 终态如实(成败/取消;预算耗尽/取消不是错误,分型
    //    如实进 reason)。
    if (trajectory_bridge != nullptr) {
        const bool cancelled = outcome.has_value() && outcome->cancelled;
        const bool ok = outcome.has_value() && !cancelled;
        std::string reason = "done";
        if (!outcome.has_value()) {
            reason = outcome.error();
        } else if (cancelled) {
            reason = "cancelled";
        } else if (outcome->hit_turn_limit || outcome->hit_step_limit ||
                   outcome->hit_time_budget || outcome->hit_token_budget) {
            reason = "budget_exhausted";
        }
        trajectory_bridge->EndTurn(ok, cancelled, reason);
    }
    if (!outcome.has_value()) {
        result.error_code = "gateway.turn_failed";
        result.error = outcome.error();
        return result;
    }
    if (outcome->cancelled) {
        result.error_code = "gateway.turn_failed";
        result.error = "cancelled";
        return result;
    }

    // 7) reply selection:从已提交事实唯一定位(读回本场 V3 流——与恢复
    //    路同一份 PlanReplySelection,不靠内存 history 猜)。
    const auto v3_stream = trajectory::v3::FindV3SessionStream(ledger->session_dir());
    if (!v3_stream.has_value()) {
        result.error_code = "gateway.reply_unavailable";
        result.error = "V3 会话流定位失败(session_dir 无法解析)";
        return result;
    }
    const auto ledger_read = trajectory::v3::ReadV3Ledger(*v3_stream);
    if (!ledger_read.has_value()) {
        result.error_code = "gateway.reply_unavailable";
        result.error = "V3 流读回失败: " + ledger_read.error();
        return result;
    }
    ReplySelectionPlan plan = PlanReplySelection(*ledger_read, turn_id);
    if (!plan.ok) {
        result.error_code = "gateway.reply_unavailable";
        result.error = plan.error;
        return result;
    }
    if (SelectionAlreadyCommitted(*ledger_read, plan.selection_id)) {
        // 幂等:同一 work 的选择已提交(理论上只有恢复路会走到),不重复
        // 落,正文照常交回。
        result.ok = true;
        result.reply_text = plan.text;
        result.selection_id = plan.selection_id;
        return result;
    }
    // 故障注入窗 1:生成结束、reply selection 未提交——到这为止账上只有
    // 终态与 assistant,恢复器应能凭冻结策略补齐全程不调模型。
    if (options_.fault_injection) {
        if (const std::string fault = options_.fault_injection(
                Options::FaultPoint::AfterGeneration);
            !fault.empty()) {
            result.error_code = "gateway.fault_injected";
            result.error = fault;
            return result;
        }
    }
    const auto commit = CommitReplySelection(v3_writer, options_.replies_dir, plan,
                                             result.session_id);
    if (!commit.committed) {
        result.error_code = "gateway.reply_unavailable";
        result.error = commit.error;
        return result;
    }
    // 故障注入窗 2:selection 已落稳、outbox 未投影——恢复器从已提交选择
    // 补出同一 delivery。
    if (options_.fault_injection) {
        if (const std::string fault = options_.fault_injection(
                Options::FaultPoint::AfterSelectionCommitted);
            !fault.empty()) {
            result.error_code = "gateway.fault_injected";
            result.error = fault;
            return result;
        }
    }
    // 8) 封口(occurrence 一场一次,跑完即收;恢复器续卷只补事实行)。
    const auto close = service.Close("gateway_automation");
    if (!close.error_code.empty()) {
        // 收口失败如实带出;执行事实已在账上,不冒充成功也不丢结果。
        result.ok = true;
        result.reply_text = plan.text;
        result.selection_id = plan.selection_id;
        result.error = "session close 未净(" + close.error_code + ");结果保留";
        return result;
    }
    result.ok = true;
    result.reply_text = plan.text;
    result.selection_id = plan.selection_id;
    return result;
}

}  // namespace lubancode::runtime
