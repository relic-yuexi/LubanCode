// HeadlessExecutor 实现(常驻总装 V1)。装配合同见头文件。
#include "runtime/headless_executor.hpp"
#include "runtime/headless_progress.hpp"
#include "agent/prompts.hpp"
#include "runtime/time_context.hpp"

#include <fstream>
#include <iterator>
#include <map>
#include <utility>

#include "agent/loop.hpp"
#include "platform/atomic_write.hpp"
#include "platform/sha256.hpp"
#include "runtime/agent_channel_engine.hpp"   // ApplyChannelToolPolicy(共用交集)
#include "runtime/async_tool_runtime.hpp"     // 异步工具 P2:one-shot/gateway 宿主接线
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
    // selectionId 定式(V2 修正):sel-<sessionId>-<turnId>。turnId 只在
    // 场内唯一——周期任务同 profile 多场共存,两场各自 mint turn-1 时
    // 旧式 sel-<turnId> 会撞名(回复原件 replies/<sel>.txt 内容对不上即
    // 断执行,V2 首批实测)。V1 兼容:流上已按旧式提交过的,沿用旧式
    // id——恢复器对 V1 期在途 occurrence 仍派生同一 id,不多送一份。
    plan.selection_id = "sel-" + ledger.session_id + "-" + turn_id;
    if (SelectionAlreadyCommitted(ledger, "sel-" + turn_id)) {
        plan.selection_id = "sel-" + turn_id;
    }
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
                                                const std::string& session_id,
                                                const std::string& delivery_target) {
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
        {"deliveryTarget", delivery_target},
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

std::size_t HeadlessExecutor::live_channel_session_count() const {
    const std::lock_guard<std::mutex> lock(channel_sessions_mutex_);
    return channel_sessions_.size();
}

void HeadlessExecutor::CloseChannelSessions(const std::string& reason) {
    std::vector<std::unique_ptr<SessionService>> to_close;
    {
        const std::lock_guard<std::mutex> lock(channel_sessions_mutex_);
        for (auto& [key, live] : channel_sessions_) {
            if (live != nullptr && live->service != nullptr) {
                to_close.push_back(std::move(live->service));
            }
        }
        channel_sessions_.clear();
    }
    for (auto& service : to_close) {
        if (service != nullptr) {
            (void)service->Close(reason);  // 收口失败如实丢:场文件在,映射账可续
        }
    }
}

HeadlessExecutor::Result HeadlessExecutor::Execute(
    const std::string& prompt, const HeadlessWorkBinding& binding,
    const std::function<void(const std::string&, const std::string&)>& on_bound,
    const std::atomic<bool>* cancel) {
    Result result;

    // 1) 开场:三端同一服务路。launch_cwd/one_shot 留缺省——Gateway 场
    //    是常驻自动任务,不是 CLI 单发;training_policy 走 Metadata 缺省。
    //    身份吃装配层裁决的整份(workspace_identity),不再按
    //    workspace_root 自算 fallback——git 仓库下四级裁决(git 形态)与
    //    fallback 算法(路径形态)算出的 key 不同,已开的房对账即
    //    identity.key_mismatch 隔离(V2 e2e 在源码树 cwd 下撞过:聊天线
    //    四级裁决开房,泵 fallback 进门对不上)。装配层没递
    //    workspace_identity 的旧测试形态照 fallback(workspace_root),
    //    行为不变。
    SessionLaunchRequest launch;
    launch.cwd_utf8 = options_.cwd_utf8;
    launch.lubancode_version = options_.lubancode_version;
    launch.workspaces_root = options_.workspaces_root;
    launch.workspace_identity = options_.workspace_identity.valid()
                                    ? options_.workspace_identity
                                    : workspace::MakeFallbackIdentity(options_.workspace_root);
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
    result.session_id = service.trajectory()->session_id();

    // 2-7) 共用核心(fresh-session 行为保持 V1:跑完即 Close)。
    Result core =
        RunTurnOnService(service, /*agent_override=*/nullptr, prompt, binding,
                         /*purpose=*/"automation",
                         /*turn_actor=*/"scheduled_host",
                         /*per_turn_tools=*/nullptr, /*binding_extra=*/nlohmann::json::object(),
                         /*selection_delivery_target=*/"local:file",
                         on_bound, cancel);
    if (!core.ok) {
        return core;
    }
    result.session_id = core.session_id;
    result.turn_id = core.turn_id;
    result.reply_text = core.reply_text;
    result.selection_id = core.selection_id;
    // 8) 封口(occurrence 一场一次,跑完即收;恢复器续卷只补事实行)。
    const auto close = service.Close("gateway_automation");
    if (!close.error_code.empty()) {
        // 收口失败如实带出;执行事实已在账上,不冒充成功也不丢结果。
        result.ok = true;
        result.reply_text = core.reply_text;
        result.selection_id = core.selection_id;
        result.error = "session close 未净(" + close.error_code + ");结果保留";
        return result;
    }
    result.ok = true;
    return result;
}

HeadlessExecutor::ChannelTurnResult HeadlessExecutor::ExecuteChannelTurn(
    const ChannelTurnRequest& request,
    const std::function<void(const std::string&, const std::string&)>& on_bound,
    const std::atomic<bool>* cancel) {
    ChannelTurnResult result;
    bool resumed = false;
    std::string error_code;
    std::string error;
    LiveChannelSession* live = GetOrOpenChannelSession(
        request.session_key, request.stored_session_id, &resumed, &error_code, &error);
    if (live == nullptr) {
        result.error_code = error_code;
        result.error = error;
        return result;
    }
    result.resumed = resumed;
    const Result core =
        RunTurnOnService(*live->service, live->agent.get(), request.prompt, request.binding,
                         /*purpose=*/"interactive",
                         /*turn_actor=*/"channel_host",
                         request.per_turn_tools, request.binding_extra,
                         request.delivery_target.empty() ? std::string("local:file")
                                                         : request.delivery_target,
                         on_bound, cancel, &request.on_tool_confirm, request.images);
    result.ok = core.ok;
    result.error_code = core.error_code;
    result.error = core.error;
    result.session_id = core.session_id;
    result.turn_id = core.turn_id;
    result.reply_text = core.reply_text;
    result.selection_id = core.selection_id;
    return result;
}

HeadlessExecutor::LiveChannelSession* HeadlessExecutor::GetOrOpenChannelSession(
    const std::string& session_key, const std::string& stored_session_id, bool* resumed,
    std::string* error_code, std::string* error) {
    std::unique_lock<std::mutex> lock(channel_sessions_mutex_);
    *resumed = false;
    // 活场命中:挪尾(LRU)。
    for (auto it = channel_sessions_.begin(); it != channel_sessions_.end(); ++it) {
        if (it->first == session_key) {
            auto live = std::move(it->second);
            channel_sessions_.erase(it);
            channel_sessions_.emplace_back(session_key, std::move(live));
            return channel_sessions_.back().second.get();
        }
    }
    // 未命中:按映射 resume-as-new / fresh(§六第四项)。身份同 Execute:
    //    吃装配层裁决的整份(见上——git 仓库下 fallback 自算会撞
    //    identity.key_mismatch)。
    SessionLaunchRequest launch;
    launch.cwd_utf8 = options_.cwd_utf8;
    launch.lubancode_version = options_.lubancode_version;
    launch.workspaces_root = options_.workspaces_root;
    launch.workspace_identity = options_.workspace_identity.valid()
                                    ? options_.workspace_identity
                                    : workspace::MakeFallbackIdentity(options_.workspace_root);
    if (!stored_session_id.empty()) {
        launch.resume_at_launch = true;
        launch.resume_source_session_id = stored_session_id;
    }
    auto live = std::make_unique<LiveChannelSession>();
    live->service = std::make_unique<SessionService>(launch);
    if (live->service->runtime() == nullptr) {
        *error_code = "gateway.launch_failed";
        *error = live->service->launch_error();
        return nullptr;
    }
    if (!live->service->v3_format()) {
        (void)live->service->Close("gateway_requires_v3");
        *error_code = "gateway.requires_v3";
        *error = "当前配置会开 v2 会话;渠道会话只跑 V3 场";
        return nullptr;
    }
    // 常驻引擎(同场多轮共享 history):resume-as-new 的折叠投影灌回新
    // Agent——上下文不断,连续来信/重启恢复都接得上。
    {
        agent::AgentProfile profile;
        profile.system_prompt = agent::DefaultPersona() + "\n" + options_.skills_prompt;
        profile.request.model = options_.model;
        profile.runtime.max_steps_per_turn = options_.max_steps_per_turn;
        profile.runtime.context_window_tokens = options_.context_window_tokens;
        profile.runtime.max_wall_secs = options_.max_wall_secs;
        profile.runtime.max_total_tokens = options_.max_total_tokens;
        profile.runtime.tool_batch_strategy = options_.tool_batch_strategy;
        profile.runtime.parallel_read_concurrency = options_.parallel_read_concurrency;
        profile = ApplyChannelToolPolicy(std::move(profile), options_.tools);
        live->agent = std::make_unique<agent::Agent>(backend_, registry_, std::move(profile));
    }
    if (!stored_session_id.empty() && live->service->trajectory()->resumed_at_launch()) {
        const std::vector<api::Message> resumed_history =
            live->service->trajectory()->LaunchResumeHistory();
        if (!resumed_history.empty()) {
            live->agent->RestoreSessionHistory(resumed_history);
        }
    }
    const std::string session_id = live->service->trajectory()->session_id();
    // 活场淘汰(超帽:最旧的一场 Close 封口,后续来信经映射账 resume-as-new
    // 续上下文,不丢)。
    if (options_.max_live_channel_sessions > 0 &&
        channel_sessions_.size() >= options_.max_live_channel_sessions) {
        auto oldest = std::move(channel_sessions_.front().second);
        channel_sessions_.erase(channel_sessions_.begin());
        lock.unlock();
        (void)oldest->service->Close("channel_session_evict");
        lock.lock();
    }
    LiveChannelSession* raw = live.get();
    channel_sessions_.emplace_back(session_key, std::move(live));
    lock.unlock();
    // 映射记账(resume-as-new 后场 id 变了,这里更新映射;幂等回调)。
    if (options_.on_session_mapped) {
        options_.on_session_mapped(session_key, session_id);
    }
    *resumed = !stored_session_id.empty();
    return raw;
}

// 共用一轮的核心:受理 → 绑定 → hook → 执行 → 收口 → reply selection。
// 不 Close——automation 调用方跑完封口,渠道路跨轮持有(§六第三项:
// "提炼执行装配为已有渠道会话的一轮",两路同一份装配)。
HeadlessExecutor::Result HeadlessExecutor::RunTurnOnService(
    SessionService& service, agent::Agent* agent_override, const std::string& prompt,
    const HeadlessWorkBinding& binding, const char* purpose, const char* turn_actor,
    const channel::ToolRoutePolicy* per_turn_tools, const nlohmann::json& binding_extra,
    const std::string& selection_delivery_target,
    const std::function<void(const std::string&, const std::string&)>& on_bound,
    const std::atomic<bool>* cancel,
    const std::function<Options::ToolConfirmDecision(const std::string&, const std::string&,
                                                     const nlohmann::json&)>*
        per_turn_confirm, const std::vector<api::ImageBlock>& images) {
    Result result;
    TrajectorySessionLedger* ledger = service.trajectory();
    trajectory::v3::V3Writer* v3_writer = ledger->v3_main_writer();
    result.session_id = ledger->session_id();

    // 受理:幂等键 = workId(同 work 重发不重复执行);原件与账行都
    // 落稳才回 accepted(V0 受理底线)。
    SessionService::InputRequest input;
    input.client_operation_id = binding.work_id;
    input.text = prompt;
    input.images = images;
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
    const auto& effective_images =
        pop.status == SessionService::PendingPop::Status::Ok ? pop.input.images : images;

    // turn 身份与绑定:事件适配器 mint turnId → 领域绑定(on_bound,泵落
    // 领域行)→ V3 gateway.work.bound(恢复反查的锚)。
    TurnEventAdapter turn_events = service.runtime()->MakeTurnAdapter();
    std::shared_ptr<HeadlessProgressReporter> progress;
    if (options_.on_progress) {
        progress = std::make_shared<HeadlessProgressReporter>(options_.on_progress,
            binding.source_kind + " " + binding.source_id + " session=" + result.session_id,
            options_.model);
        turn_events.AttachAlongside([progress](const ServerEvent& event) { progress->Observe(event); });
    }
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
        if (binding_extra.is_object() && !binding_extra.empty()) {
            bound_draft.payload["channel"] = binding_extra;  // 渠道审计载荷(Q2)
        }
        const auto bound_receipt =
            v3_writer->AppendEvent(std::move(bound_draft), trajectory::v3::Durability::PowerLoss);
        if (bound_receipt.status != trajectory::v3::WriteReceipt::Status::Committed) {
            result.error_code = "gateway.launch_failed";
            result.error = "gateway.work.bound 落不了账: " + bound_receipt.error_code;
            return result;
        }
    }

    // hook 四点(与渠道路同一 runtime 派发点,不另接一套)。
    hooks::HookDispatcher* dispatcher = options_.hook_dispatcher;
    BindMiddlewareSessionWriter(dispatcher, &DefaultHookServiceCenter(),
                                v3_writer);
    std::string effective_text = effective_prompt;
    const MiddlewareHookContext middleware_context = [&] {
        MiddlewareHookContext context;
        context.turn_id = turn_id;
        context.origin = "human";
        context.purpose = purpose;
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
    for (const auto& image : effective_images) user_message.content.push_back(image);
    for (const std::string& append : post_user.context_appends) {
        user_message.content.push_back(
            api::TextBlock{"[PostUser 钩子附加上下文,非用户手敲]\n" + append});
    }

    // 执行:Agent + TurnWiring + 轮桥 + 工具栅栏(AgentChannelEngine
    // 的遗留缺口在此补齐:ToolTraceHub 挂桥,真实工具 Action 落账)。
    auto trajectory_bridge = ledger->NewTurnBridge({"", options_.wire_name, "gateway"});
    if (trajectory_bridge != nullptr) {
        trajectory_bridge->BeginTurn(turn_id, turn_actor);
        trajectory_bridge->RecordInput(user_message);
    }
    agent::AgentProfile profile;
    profile.system_prompt = agent::DefaultPersona() + "\n" + options_.skills_prompt;
    profile.request.model = options_.model;
    profile.runtime.max_steps_per_turn = options_.max_steps_per_turn;
    profile.runtime.context_window_tokens = options_.context_window_tokens;
    profile.runtime.max_wall_secs = options_.max_wall_secs;
    profile.runtime.max_total_tokens = options_.max_total_tokens;
    profile.runtime.tool_batch_strategy = options_.tool_batch_strategy;
    profile.runtime.parallel_read_concurrency = options_.parallel_read_concurrency;
    profile = ApplyChannelToolPolicy(std::move(profile), options_.tools);
    // 引擎来源:渠道路用调用方随场缓存的(同场多轮共享 history);automation
    // 现建(每执行一场 fresh V3 + fresh 引擎,V1 行为不变)。
    std::unique_ptr<agent::Agent> fresh_agent;
    if (agent_override == nullptr) {
        fresh_agent = std::make_unique<agent::Agent>(backend_, registry_, std::move(profile));
    }
    agent::Agent& loop_agent = agent_override != nullptr ? *agent_override : *fresh_agent;
    // 缓存 Agent 跨轮活着，显示闭包只活本轮；离场恢复旧接线。
    struct RestoreWiring {
        agent::Agent& target;
        agent::AgentWiring previous;
        ~RestoreWiring() { target.SetWiring(std::move(previous)); }
    } restore{loop_agent, loop_agent.wiring()};
    if (progress) {
        auto observed_wiring = loop_agent.wiring();
        const auto previous_pressure = observed_wiring.on_context_pressure;
        observed_wiring.on_context_pressure = [progress, previous_pressure](const agent::ContextPressure& p) {
            if (previous_pressure) previous_pressure(p);
            progress->Context(p);
        };
        loop_agent.SetWiring(std::move(observed_wiring));
        progress->Note("上下文窗口来源：" + std::string(options_.context_window_tokens ? "运行配置" : "引擎兜底") +
                       "；压缩/截断按实际事件报告");
    }
    // system 只放稳定环境；需要时间时调用工具，不逐轮注入动态值。
    // (前缀缓存守恒单 §五 A 同款口径:目录字段是开场冻结的基线,称"会话
    // 启动目录",不冒充当前值。)
    loop_agent.SetSystemPrompt(
        agent::DefaultPersona() + "\n" + options_.skills_prompt +
        "\n# 运行环境\n\n- 会话启动目录: " + options_.cwd_utf8 +
        "\n需要当前日期或时间时调用 get_current_time，不要猜测，也不要沿用历史轮次的时间。"
        "\n相对提醒使用 create_reminder.delay_seconds，由宿主计算，禁止猜测或试探时间戳。"
        "\n需要读文件、执行命令时调用实际工具，以工具回执为准，不要编造结果。\n");

    agent::TurnWiring wiring;
    wiring.events = &turn_events;
    wiring.boundary_recorder = trajectory_bridge.get();
    wiring.turn_id = turn_id;
    if (progress) {
        wiring.on_request_attempt = [progress](const api::ModelRequestAttempt& attempt,
                                              api::RequestAttemptPhase phase) {
            if (phase == api::RequestAttemptPhase::Retrying || phase == api::RequestAttemptPhase::Exhausted)
                progress->Note(std::string(phase == api::RequestAttemptPhase::Retrying ? "请求重试" : "请求失败") +
                    " attempt=" + std::to_string(attempt.attempt) + " code=" +
                    HeadlessProgressReporter::Preview(attempt.error_code));
        };
    }
    // 工具 artifact 落位与 one_shot 同款:会话档内容寻址,随账本持久。
    const std::filesystem::path artifacts_dir = ledger->session_dir() / "artifacts" / "sha256";
    wiring.tool_artifact_dir = artifacts_dir.generic_string();
    // 本轮有效策略:渠道路递逐轮冻结账(执行前重验准入后的五层交集),
    // 没递回落会话级 options_.tools(automation 恒走后者,V1 行为零变化)。
    const channel::ToolRoutePolicy& tools_policy =
        per_turn_tools != nullptr ? *per_turn_tools : options_.tools;
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
    // 审批注入口(W2,助理任务):宿主递了确认回调时,needs_confirm 工具
    // 的确认经它走("问页面");拒绝文案由回调带回(超时拒绝不冒充用户
    // 拒绝)。同步泵串行执行,denial 明细按 tool_use_id 查表无并发。
    // Q6 渠道远端审批:per_turn_confirm(泵按 WorkItem 冻结上下文拼的
    // 回调)压过 Options 注入口——渠道轮的审批问渠道按钮,W2 注入口
    // 管本机助理任务,两不混。回调与审批口同线程先后调用,denials 表
    // 单线程读写无并发。
    if (per_turn_confirm != nullptr && *per_turn_confirm != nullptr) {
        const auto decide = *per_turn_confirm;
        const auto denials = std::make_shared<std::map<std::string, std::string>>();
        wiring.on_tool_confirm = [decide, denials](const std::string& tool_use_id,
                                                   const std::string& name,
                                                   const nlohmann::json& input) {
            const Options::ToolConfirmDecision decision = decide(tool_use_id, name, input);
            if (!decision.allowed && !decision.denial_text.empty()) {
                (*denials)[tool_use_id] = decision.denial_text;
            }
            return decision.allowed;
        };
        wiring.on_tool_denial_text = [denials](const std::string& tool_use_id,
                                               const std::string& /*name*/) {
            const auto found = denials->find(tool_use_id);
            return found != denials->end() ? found->second
                                           : std::string("用户拒绝执行该工具");
        };
    } else if (options_.on_tool_confirm) {
        const auto decide = options_.on_tool_confirm;
        const auto denials = std::make_shared<std::map<std::string, std::string>>();
        wiring.on_tool_confirm = [decide, denials](const std::string& tool_use_id,
                                                   const std::string& name,
                                                   const nlohmann::json& input) {
            const Options::ToolConfirmDecision decision = decide(tool_use_id, name, input);
            if (!decision.allowed && !decision.denial_text.empty()) {
                (*denials)[tool_use_id] = decision.denial_text;
            }
            return decision.allowed;
        };
        wiring.on_tool_denial_text = [denials](const std::string& tool_use_id,
                                               const std::string& /*name*/) {
            const auto found = denials->find(tool_use_id);
            return found != denials->end() ? found->second
                                           : std::string("用户拒绝执行该工具");
        };
    }
    // 逐轮收窄闸(§16.2 第三层;渠道路与 AgentChannelEngine 同款):本轮
    // 冻结策略比会话级暴露面窄时,在执行口拦下——不折会话级 tool_filter
    //(前缀缓存),被滤的工具连模型都看不见,这里只拦"看得见但本轮禁"。
    if (per_turn_tools != nullptr) {
        wiring.on_pre_tool_use_hook =
            [&tools_policy](const std::string& /*tool_use_id*/, const std::string& name,
                            const nlohmann::json& /*input*/) {
                runtime::ToolHookDecision decision;
                if (!tools_policy.Allows(name)) {
                    decision.decision = runtime::ToolHookDecision::Decision::Deny;
                    decision.reason = "工具 " + name +
                                      " 不在本轮渠道工具上限的允许名单内(执行前重验冻结"
                                      "的五层交集没列它,或进了任一层 deny)。";
                }
                return decision;
            };
    }
    if (HasPreRequestMiddleware(dispatcher)) {
        wiring.on_pre_request_hooks = [dispatcher, purpose](const std::string& step_id,
                                                            const std::string& turn_id_,
                                                            const nlohmann::json& frozen_request_snapshot,
                                                            const PreRequestBudget& budget) {
            MiddlewareHookContext context;
            context.turn_id = turn_id_;
            context.step_id = step_id;
            context.purpose = purpose;
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
    if (progress) {
        const auto previous_trace = wiring.on_tool_trace;
        wiring.on_tool_trace = [progress, previous_trace, &trace_hub](const agent::ToolTraceEvent& event) {
            if (previous_trace) previous_trace(event);
            if (event.kind == agent::ToolTraceEventKind::ExecutionStarted) {
                progress->Note(std::string(trace_hub.IsExecutionBlocked(event.execution_id)
                    ? "执行前栅栏拒绝 " : "开始执行工具 ") +
                    HeadlessProgressReporter::Preview(event.tool_name));
            }
        };
    }

    // 异步工具 P2(one-shot/gateway 宿主接线):会话级异步运行时挂进
    // SessionService 的 SessionRuntime(零策略 dormant,行为与从前一字不
    // 差);每轮钉桥 + 闸门/规划进 wiring。
    if (AttachDefaultAsyncToolRuntime(*service.runtime(), options_.wire_name)) {
        AsyncToolRuntime* async_runtime = service.runtime()->async_tool_runtime();
        if (async_runtime != nullptr && trajectory_bridge != nullptr) {
            async_runtime->InstallTurnBridge(trajectory_bridge.get());
            async_runtime->NoteModelIdentity(std::string(), options_.model);
            wiring.tool_batch_gate = async_runtime->gate();
            wiring.delivery_planner = async_runtime->planner();
        }
    }

    const auto outcome = agent::AgentLoop::Run(loop_agent, user_message, wiring, cancel);

    // 收口:turn 终态如实(成败/取消;预算耗尽/取消不是错误,分型
    // 如实进 reason)。
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
    turn_events.Finish(!outcome.has_value() ? Outcome::Failed :
                       outcome->cancelled ? Outcome::Cancelled : Outcome::Succeeded,
                       outcome.has_value() ? std::string() : outcome.error());
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

    // reply selection:从已提交事实唯一定位(读回本场 V3 流——与恢复
    // 路同一份 PlanReplySelection,不靠内存 history 猜)。
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
                                             result.session_id, selection_delivery_target);
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
    result.ok = true;
    result.reply_text = plan.text;
    result.selection_id = plan.selection_id;
    return result;
}

}  // namespace lubancode::runtime
