// CommandService 的实现(显示系统剥离单第七步)。
//
// 三条 typed API 的执行体,业务规则原文自 app/commands 的旧模型命令路
// 与 ResumeSession 搬来(清单拼装、条目应用、序号解析、回放接管),剥掉
// 打印与交互问话——那半留在终端适配层。/model 的切换提交如今只剩这一条
// 路:终端菜单选出的 id 与带参直切都经 SetModel 进来。依赖铁律:不 include
// cli/app,不碰标准流。

#include "runtime/command_service.hpp"

#include <algorithm>
#include <utility>

namespace lubancode::runtime {

CommandService::CommandService(Options options) : options_(std::move(options)) {}

CommandService::~CommandService() = default;

// ---- SetModel ---------------------------------------------------------------

ModelQueryResult CommandService::QueryModels() const {
    ModelQueryResult out;
    out.current_model = options_.current_model ? *options_.current_model : std::string();
    out.config_file_path = options_.config_file_path.value_or(std::string());
    if (options_.fetch_models) {
        auto fetched = options_.fetch_models();
        if (fetched.has_value()) {
            for (auto& [id, display] : *fetched) {
                ModelListEntry entry;
                entry.id = id;
                entry.display_name = display;
                entry.current = id == out.current_model;
                out.models.push_back(std::move(entry));
            }
        } else {
            out.fetch_failed = true;
            out.fetch_error = fetched.error();
        }
    } else {
        // 没注取数口:至少报当前项(裸敲也能看"现在是谁")。
        if (!out.current_model.empty()) {
            ModelListEntry entry;
            entry.id = out.current_model;
            entry.display_name = out.current_model;
            entry.current = true;
            out.models.push_back(std::move(entry));
        }
    }
    return out;
}

SetModelResult CommandService::SetModel(const std::string& model_id, bool write_config) {
    SetModelResult out;
    if (model_id.empty()) {
        out.error = "empty_model";
        return out;
    }
    if (options_.current_model == nullptr || options_.config == nullptr) {
        out.error = "not_configured";
        return out;
    }
    out.model = model_id;
    *options_.current_model = model_id;
    options_.config->model = model_id;
    out.switched = true;

    // 模型目录应用(主动切换:目录声明了就用,用户显式配过的不动——两个
    // explicit 都 false,与启动时终端 ApplyModelCatalog 同一规矩)。think/
    // 窗口/模型指令各自落账并写回执,前端照回执排版——runtime 不打印。
    if (options_.model_catalog != nullptr) {
        const config::CatalogApplication application =
            config::ComputeCatalogApplication(*options_.model_catalog, model_id,
                                              /*think_explicitly_configured=*/false,
                                              /*window_explicitly_configured=*/false);
        if (application.think.has_value() && options_.current_think != nullptr) {
            *options_.current_think = *application.think;
            out.think_from_catalog = true;
        }
        if (application.context_window_tokens.has_value()) {
            if (options_.apply_context_window) {
                options_.apply_context_window(*application.context_window_tokens);
            }
            out.applied_context_window = *application.context_window_tokens;
        }
        // base_instructions 永远整体覆盖:切到目录外模型时旧模型的指令自然
        // 被清空,回退现状;回执只在"换成了非空指令"时报真。
        if (options_.current_model_instructions != nullptr &&
            *options_.current_model_instructions != application.base_instructions) {
            out.instructions_replaced = !application.base_instructions.empty();
            *options_.current_model_instructions = application.base_instructions;
        }
    }
    if (options_.current_think != nullptr) {
        out.think = *options_.current_think;  // 应用后的最终档位(没动就是原档)
    }

    // 写回配置文件是显式一笔(终端问过才传 true;GUI 分立按钮)。
    if (write_config && options_.config_file_path.has_value()) {
        const auto written = WriteModelToConfig(model_id);
        if (written.has_value()) {
            out.config_written = true;
        } else {
            out.error = "config_write_failed: " + written.error();
        }
    }
    return out;
}

std::expected<void, std::string> CommandService::WriteModelToConfig(const std::string& model_id) {
    if (!options_.config_file_path.has_value()) {
        return std::unexpected("no_config_file");
    }
    const std::string& target = *options_.config_file_path;
    // 活跃 provider 在场:模型写进 provider 条目——每个 provider 各记
    // 各的,切走再切回来还是这个模型;且活跃端镜像会拿条目的 model
    // 压过顶层字段,只写顶层等于白写。条目不在这份文件里才退回顶层。
    bool wrote_entry = false;
    if (options_.config != nullptr && !options_.config->active_provider.empty()) {
        const auto entry =
            config::UpdateProviderModelInConfigFile(target, options_.config->active_provider, model_id);
        if (!entry.has_value()) {
            return std::unexpected(entry.error());
        }
        wrote_entry = *entry;
    }
    if (!wrote_entry) {
        const auto updated = config::UpdateModelInConfigFile(target, model_id);
        if (!updated.has_value()) {
            return std::unexpected(updated.error());
        }
    }
    return {};
}

// ---- SetRoleModel ------------------------------------------------------------

SetRoleModelResult CommandService::SetRoleModel(const std::string& role_name, const std::string& model_id,
                                                bool write_config) {
    SetRoleModelResult out;
    // 角色名归一:小写、认别名 plan→lao。不认的名字如实报,不猜。
    std::string role;
    for (const char c : role_name) {
        role += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    if (role == "plan") {
        role = "lao";  // lao 角色的对外别名(计划与架构档)
    }
    if (role != "normal" && role != "cheap" && role != "lao") {
        out.error = "unknown_role";
        return out;
    }
    if (model_id.empty()) {
        out.error = "empty_model";
        return out;
    }
    if (options_.config == nullptr) {
        out.error = "not_configured";
        return out;
    }

    // 内存生效一笔:改 shorthand 字段。路由表每次 Route() 现折 config
    // (BuildRoleSpecs 直读这份内存),下一笔后台小活就走新模型;高级段
    // (model_roles.<role>)优先级高于 shorthand,用户配了高级段时内存里
    // 仍以高级段为准——落盘那一笔会改到高级段,重启后两边一致。
    if (role == "normal") {
        options_.config->normal_model = model_id;
    } else if (role == "cheap") {
        options_.config->cheap_model = model_id;
    } else {
        options_.config->lao_model = model_id;
    }
    out.role = role;
    out.model = model_id;
    out.switched = true;

    if (write_config && options_.config_file_path.has_value()) {
        const auto updated =
            config::UpdateRoleModelInConfigFile(*options_.config_file_path, role, model_id);
        if (updated.has_value()) {
            out.config_written = true;
        } else {
            out.error = "config_write_failed: " + updated.error();
        }
    }
    return out;
}

// ---- ResumeThread -------------------------------------------------------------

std::vector<ThreadListEntry> CommandService::ListThreads(std::size_t limit) const {
    std::vector<ThreadListEntry> out;
    if (options_.sessions_dir.empty()) {
        return out;  // 没主目录:空清单,不冒充
    }
    // cwd 过滤不做(远端前端看得见全部场子,自己按 cwd 列组);resume 的
    // 序号以这份清单为准。
    const std::vector<agent::SessionListEntry> entries = agent::ListSessions(options_.sessions_dir, limit);
    out.reserve(entries.size());
    std::size_t index = 1;
    for (const auto& entry : entries) {
        ThreadListEntry item;
        item.id = entry.id;
        item.started_at = entry.started_at;
        item.cwd = entry.cwd;
        item.title = entry.title;
        item.first_user_text = entry.first_user_text;
        item.message_count = entry.message_count;
        item.index = index++;
        out.push_back(std::move(item));
    }
    return out;
}

ResumeResult CommandService::ResumeThread(agent::Agent& loop, SessionRuntime& runtime,
                                          const std::string& thread_ref, const std::string& cwd) {
    ResumeResult out;
    if (runtime.sessions_dir().empty()) {
        out.error = "no_home";
        return out;
    }
    // 目标解析:空串 = 最近一场;纯数字 = 列表序号(倒序,1 起);其余按
    // id 找,不在前列就拼路径兜底(与终端 ResumeSession 同规矩,只是这里
    // 不限"本目录"——远端前端看得见全部场子)。
    const std::vector<agent::SessionListEntry> entries =
        agent::ListSessions(runtime.sessions_dir(), 200, /*cwd_filter=*/std::string());
    std::string id;
    std::string file_path;
    bool all_digits = !thread_ref.empty();
    for (const char c : thread_ref) {
        if (c < '0' || c > '9') {
            all_digits = false;
            break;
        }
    }
    if (thread_ref.empty()) {
        if (entries.empty()) {
            out.error = "none";
            return out;
        }
        id = entries.front().id;
        file_path = entries.front().file_path;
    } else if (all_digits) {
        std::size_t n = 0;
        try {
            n = static_cast<std::size_t>(std::stoul(thread_ref));
        } catch (...) {
            n = 0;
        }
        if (n < 1 || n > entries.size()) {
            out.error = "out_of_range";
            return out;
        }
        id = entries[n - 1].id;
        file_path = entries[n - 1].file_path;
    } else {
        for (const auto& entry : entries) {
            if (entry.id == thread_ref) {
                id = entry.id;
                file_path = entry.file_path;
                break;
            }
        }
        if (id.empty()) {
            id = thread_ref;
            file_path = runtime.sessions_dir() + "/" + thread_ref + ".jsonl";
        }
    }

    const auto content = agent::ReadSessionFileBytes(file_path);
    if (!content.has_value()) {
        out.error = "read_failed";
        return out;
    }
    auto session = agent::ParseSessionFile(*content);
    if (!session.has_value()) {
        out.error = "bad_meta";
        return out;
    }

    loop.ReplaceHistory(session->messages);
    runtime.persisted_count() = session->messages.size();
    (void)runtime.store().ResumeAt(file_path, id);
    runtime.meta() = session->meta;
    runtime.title() = session->title;
    runtime.compact_epoch() = session->compact_epoch;
    (void)cwd;

    out.resumed = true;
    out.id = id;
    out.restored_messages = session->messages.size();
    out.total_lines = session->all_messages.size();
    out.compact_epoch = session->compact_epoch;
    out.title = session->title;
    return out;
}

// ---- 审批/提问回答 -------------------------------------------------------------

CommandService::InteractionAnswerResult CommandService::ResolveApproval(
    InteractionBroker* broker, const std::string& request_id, const ApprovalResponse& response) const {
    InteractionAnswerResult out;
    if (broker == nullptr) {
        out.error_code = "no_broker";
        out.error_message = "这台前端没有悬起审批的实现(终端当场问完)";
        return out;
    }
    InteractionRequestId parsed;
    parsed.value = request_id;
    if (!broker->ResolveApproval(parsed, response)) {
        out.error_code = kStaleRequestId;
        out.error_message = "请求已失效(答完/收口/不认识)";
    } else {
        out.ok = true;
    }
    return out;
}

CommandService::InteractionAnswerResult CommandService::AnswerQuestion(
    InteractionBroker* broker, const std::string& request_id, const QuestionResponse& response) const {
    InteractionAnswerResult out;
    if (broker == nullptr) {
        out.error_code = "no_broker";
        out.error_message = "这台前端没有悬起提问的实现(终端当场问完)";
        return out;
    }
    InteractionRequestId parsed;
    parsed.value = request_id;
    if (!broker->AnswerQuestion(parsed, response)) {
        out.error_code = kStaleRequestId;
        out.error_message = "请求已失效(答完/收口/不认识)";
    } else {
        out.ok = true;
    }
    return out;
}

// ---- /goal + /loop + plan.review(typed 兑现) ---------------------------------

namespace {

// 稳定码折回执:成功给 payload,失败给 code + 人话兜底。
ClientReceipt MakeTypedReceipt(bool ok, const std::string& error_code, const std::string& error_message,
                               nlohmann::json payload = nlohmann::json::object()) {
    ClientReceipt receipt;
    receipt.accepted = ok;
    receipt.error_code = ok ? std::string() : error_code;
    receipt.error_message = ok ? std::string() : error_message;
    receipt.payload = std::move(payload);
    return receipt;
}

// LoopScheduler 的 TaskView -> JSON(loop.list/read 的结构化账;字段名与
// loop 单的 session 事件行同口径 snake_case,前端自己翻)。
nlohmann::json LoopTaskViewToJson(const loop::LoopScheduler::TaskView& view) {
    nlohmann::json j;
    j["task_id"] = view.task.task_id;
    j["prompt"] = view.task.prompt;
    j["interval_ms"] = static_cast<std::int64_t>(view.task.interval.count()) * 1000;
    j["state"] = loop::ToString(view.task.state);
    j["next_due_at_ms"] = view.task.next_due_at_ms;
    j["expires_at_ms"] = view.task.expires_at_ms;
    j["run_count"] = view.task.run_count;
    j["skipped_count"] = view.task.skipped_count;
    j["prompt_source"] = loop::ToString(view.task.prompt_source);
    j["delayed"] = view.delayed;
    if (view.has_current_tick) {
        j["current_tick_id"] = view.current_tick.tick_id;
    }
    if (view.task.state == loop::LoopTaskState::BackingOff) {
        j["backoff_until_ms"] = view.backoff_until_ms;
    }
    return j;
}

}  // namespace

ClientReceipt CommandService::HandleGoalCommand(const ClientCommand& command,
                                                goal::GoalCoordinator* coordinator,
                                                const std::string& workspace_root, std::int64_t now_ms) {
    using K = ClientCommandKind;
    using goal::GoalCommandResult;
    if (coordinator == nullptr) {
        return MakeTypedReceipt(false, "goal.disabled", "goals 功能未装配(features.goals 未开启)");
    }
    // 六枚命令之外的不收(防误投)。
    switch (command.kind) {
        case K::CreateGoal:
        case K::GetGoal:
        case K::EditGoal:
        case K::PauseGoal:
        case K::ResumeGoal:
        case K::ClearGoal:
            break;
        default:
            return MakeTypedReceipt(false, "invalid_request", "不是 goal 命令: " + ToString(command.kind));
    }
    const std::string workspace_identity = workspace_root;  // 首版 identity = root
    GoalCommandResult result;
    switch (command.kind) {
        case K::CreateGoal:
            result = coordinator->Create(command.text, workspace_root, workspace_identity, now_ms);
            break;
        case K::GetGoal: {
            // 查账纯本地输出,不发模型(单子"状态查询不发模型")。
            nlohmann::json status = coordinator->Status(now_ms);
            return MakeTypedReceipt(true, std::string(), std::string(), std::move(status));
        }
        case K::EditGoal: {
            const int expected = command.payload.value("expected_revision", 0);
            result = coordinator->Edit(command.text, expected, now_ms);
            break;
        }
        case K::PauseGoal:
            result = coordinator->Pause(now_ms);
            break;
        case K::ResumeGoal: {
            const int expected = command.payload.value("expected_revision", 0);
            result = coordinator->Resume(expected, now_ms);
            break;
        }
        case K::ClearGoal: {
            // confirm 归调用方(终端确认屏/GUI 对话框),协议不替人决定;
            // 没带 confirm 一律 confirmation_required,不动账。
            if (!command.payload.value("confirm", false)) {
                return MakeTypedReceipt(false, "confirmation_required",
                                        "clear 需要二次确认(payload.confirm = true 才动手)");
            }
            result = coordinator->Clear(now_ms);
            break;
        }
        default:
            break;
    }
    return MakeTypedReceipt(result.ok, result.error_code, result.error_message, result.payload);
}

ClientReceipt CommandService::HandleLoopCommand(const ClientCommand& command,
                                                loop::LoopScheduler* scheduler, const std::string& cwd_identity,
                                                const std::string& session_id, std::int64_t now_ms) {
    using K = ClientCommandKind;
    if (scheduler == nullptr) {
        return MakeTypedReceipt(false, "loop.disabled", "loop 功能未装配(features.loop 未开启)");
    }
    switch (command.kind) {
        case K::CreateLoopTask: {
            // interval_ms 可选(0 = 默认 10m);prompt 在 text(空 = loop.md/
            // 内置源,由泵每拍现读)。
            const int interval_ms = command.payload.value("interval_ms", 0);
            const auto interval = interval_ms > 0
                                      ? std::chrono::seconds(interval_ms / 1000)
                                      : loop::LoopDefaults::kDefaultInterval;
            const auto result = scheduler->Create(command.text, interval, now_ms, cwd_identity, session_id,
                                                  loop::LoopPromptSource::Inline);
            return MakeTypedReceipt(result.ok, result.error_code, result.error_message, result.payload);
        }
        case K::ListLoopTasks: {
            nlohmann::json tasks = nlohmann::json::array();
            for (const auto& view : scheduler->Snapshot(now_ms)) {
                tasks.push_back(LoopTaskViewToJson(view));
            }
            return MakeTypedReceipt(true, std::string(), std::string(),
                                    nlohmann::json{{"tasks", std::move(tasks)}});
        }
        case K::ReadLoopTask: {
            const std::string id = scheduler->ResolveTaskId(command.value);
            const auto view = scheduler->Find(id, now_ms);
            if (!view.has_value()) {
                return MakeTypedReceipt(false, "loop.not_found", "任务不存在: " + command.value);
            }
            return MakeTypedReceipt(true, std::string(), std::string(),
                                    nlohmann::json{{"task", LoopTaskViewToJson(*view)}});
        }
        case K::PauseLoopTask: {
            const std::string id = command.value == "all" ? std::string("all")
                                                          : scheduler->ResolveTaskId(command.value);
            const auto result = scheduler->Pause(id, now_ms, "remote");
            return MakeTypedReceipt(result.ok, result.error_code, result.error_message, result.payload);
        }
        case K::ResumeLoopTask: {
            const std::string id = command.value == "all" ? std::string("all")
                                                          : scheduler->ResolveTaskId(command.value);
            const auto result = scheduler->Resume(id, now_ms);
            return MakeTypedReceipt(result.ok, result.error_code, result.error_message, result.payload);
        }
        case K::CancelLoopTask: {
            const std::string id = command.value == "all" ? std::string("all")
                                                          : scheduler->ResolveTaskId(command.value);
            const auto result = scheduler->Stop(id, now_ms, "remote");
            return MakeTypedReceipt(result.ok, result.error_code, result.error_message, result.payload);
        }
        case K::RunLoopTaskNow: {
            if (command.value == "all") {
                return MakeTypedReceipt(false, "loop.invalid_request", "run 不收 all,点名一只任务");
            }
            const auto result = scheduler->RunNow(scheduler->ResolveTaskId(command.value), now_ms);
            return MakeTypedReceipt(result.ok, result.error_code, result.error_message, result.payload);
        }
        default:
            return MakeTypedReceipt(false, "invalid_request", "不是 loop 命令: " + ToString(command.kind));
    }
}

ClientReceipt CommandService::HandlePlanCommand(const ClientCommand& command, SessionRuntime* runtime) {
    using K = ClientCommandKind;
    if (runtime == nullptr) {
        return MakeTypedReceipt(false, "plan.disabled", "Plan 模式未装配(没有 SessionRuntime)");
    }
    if (command.kind == K::SetCollaborationMode) {
        // value: "plan"/"default";reason 是稳定短码(remote/approved/off)。
        runtime::CollaborationMode mode = runtime::CollaborationMode::Default;
        if (command.value == "plan") {
            mode = runtime::CollaborationMode::Plan;
        } else if (command.value != "default") {
            return MakeTypedReceipt(false, "plan.invalid_mode", "mode 只认 plan/default: " + command.value);
        }
        const std::string reason = command.payload.value("reason", std::string("remote"));
        const std::string permission_before =
            command.payload.value("permission_before_plan", std::string());
        const bool switched = runtime->SetCollaborationMode(mode, reason, permission_before);
        nlohmann::json payload;
        payload["mode"] = command.value;
        payload["switched"] = switched;  // 同档重复切 false,不是错误
        payload["revision"] = runtime->mode_state().revision;
        return MakeTypedReceipt(true, std::string(), std::string(), std::move(payload));
    }
    if (command.kind == K::ReviewPlan) {
        // payload: plan_id/plan_revision/sha256 + decision(approved_confirm/
        // approved_auto/rejected/continued)。批准/拒绝须同时匹配 id/
        // revision/hash;continued 只留痕,不动计划账。
        const auto* plan = runtime->latest_plan();
        if (plan == nullptr) {
            return MakeTypedReceipt(false, "plan.no_plan", "本场还没有计划成品");
        }
        const std::string plan_id = command.payload.value("plan_id", std::string());
        const std::uint64_t revision = command.payload.value("plan_revision", 0);
        const std::string sha256 = command.payload.value("sha256", std::string());
        const std::string decision = command.payload.value("decision", std::string());
        if (decision != "approved_confirm" && decision != "approved_auto" && decision != "rejected" &&
            decision != "continued") {
            return MakeTypedReceipt(false, "plan.invalid_decision",
                                    "decision 只认 approved_confirm/approved_auto/rejected/continued");
        }
        if (decision == "continued") {
            // 继续规划:不动账,只回执(留痕由前端自己记)。
            return MakeTypedReceipt(true, std::string(), std::string(),
                                    nlohmann::json{{"decision", "continued"}});
        }
        if (plan->plan_id != plan_id || plan->revision != revision || plan->content_sha256 != sha256) {
            // 三对不匹配:旧 dialog 的迟到回答(单子:批准须同时匹配)。
            return MakeTypedReceipt(false, "stale_request_id",
                                    "计划已换稿或不是这份(id/revision/hash 对不上)");
        }
        const bool approve = decision != "rejected";
        const auto outcome = runtime->ReviewPlan(plan_id, revision, sha256, approve);
        switch (outcome) {
            case SessionRuntime::PlanReviewOutcome::Stale:
                return MakeTypedReceipt(false, "stale_request_id", "审批落账时计划已换稿");
            case SessionRuntime::PlanReviewOutcome::Approved:
                return MakeTypedReceipt(true, std::string(), std::string(),
                                        nlohmann::json{{"decision", "approved"},
                                                       {"permission_mode",
                                                        decision == "approved_auto" ? "auto" : "confirm"}});
            case SessionRuntime::PlanReviewOutcome::Rejected:
                return MakeTypedReceipt(true, std::string(), std::string(),
                                        nlohmann::json{{"decision", "rejected"}});
        }
        return MakeTypedReceipt(false, "plan.internal", "审批分路落空");
    }
    if (command.kind == K::ReopenPlanReview) {
        // 重开审阅:有稿才受理(前端拿 latest 自己画框;这里只报有没有)。
        const auto* plan = runtime->latest_plan();
        if (plan == nullptr) {
            return MakeTypedReceipt(false, "plan.no_plan", "本场还没有计划成品");
        }
        nlohmann::json payload;
        payload["plan_id"] = plan->plan_id;
        payload["plan_revision"] = plan->revision;
        payload["sha256"] = plan->content_sha256;
        return MakeTypedReceipt(true, std::string(), std::string(), std::move(payload));
    }
    return MakeTypedReceipt(false, "invalid_request", "不是 plan 命令: " + ToString(command.kind));
}

}  // namespace lubancode::runtime
