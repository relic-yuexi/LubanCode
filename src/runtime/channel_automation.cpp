// ChannelAutomationBridge 实现(QQ 接入单 Q5)。装配合同见头文件。
#include "runtime/channel_automation.hpp"

#include <chrono>
#include <utility>

#include "channel/qq/qq_proto.hpp"  // ParseLooseInt64(模型数值字段宽松解析)

namespace lubancode::runtime {

namespace {

std::int64_t DefaultNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// description 长度帽:提醒正文不是文章,超帽明拒不截断(截断会 silently
// 改变用户意图)。
constexpr std::size_t kDescriptionCapBytes = 4000;

// once 时间的合理上限(与 interval 上界同量级:10 年)。再远的"提醒"多半
// 是模型算错了日期,明拒让模型与用户核对。
constexpr std::int64_t kMaxOnceLeadMs = 3650LL * 24 * 3600 * 1000;

// job 执行正文的自包含包装:隔离场没有聊天史,任务规格必须自带全部上下文。
std::string ComposeJobPrompt(const std::string& description, const std::string& channel_id,
                             const std::string& account_id, const std::string& conversation_id,
                             const std::string& sender_id) {
    std::string prompt = "这是用户经 " + channel_id + "/" + account_id + " 会话 " + conversation_id +
                         " 设置的定时任务(创建者 " + sender_id + ")。\n" +
                         "任务描述:" + description +
                         "\n"
                         "到点执行:按任务描述完成工作或组织提醒内容,输出直接发给该用户的最终正文"
                         "(简短明确的中文;不要客套,不要调用任何投递工具——宿主会把你的最终回复"
                         "投回原会话)。";
    return prompt;
}

// job 投影 -> 调度规格(automation_store 内部 SpecOfJob 不导出,桥侧同式
// 重建——字段一一对应,不发明第二套语义)。
gateway::ScheduleSpec SpecOfJob(const gateway::AutomationJob& job) {
    gateway::ScheduleSpec spec;
    spec.kind = job.schedule_kind;
    spec.due_at_ms = job.due_at_ms;
    spec.interval_seconds = job.interval_seconds;
    spec.anchor_ms = job.anchor_ms;
    spec.cron_expr = job.cron_expr;
    spec.timezone = job.timezone;
    spec.misfire = job.misfire;
    return spec;
}

bool OwnedBy(const gateway::AutomationJob& job, const ChannelAutomationBridge::TurnContext& ctx) {
    return job.owner_channel == ctx.channel_id && job.owner_account == ctx.account_id &&
           job.owner_sender == ctx.sender_id;
}

}  // namespace

ChannelAutomationBridge::ChannelAutomationBridge(gateway::AutomationStore* store,
                                                 std::function<std::int64_t()> now_ms)
    : store_(store), now_ms_(std::move(now_ms)) {}

void ChannelAutomationBridge::set_store(gateway::AutomationStore* store) {
    store_ = store;
}

ChannelAutomationBridge::TurnScope::TurnScope(ChannelAutomationBridge& bridge,
                                              const TurnContext& context)
    : bridge_(bridge) {
    bridge_.current_ = context;
}

ChannelAutomationBridge::TurnScope::~TurnScope() {
    bridge_.current_.reset();
}

const ChannelAutomationBridge::TurnContext* ChannelAutomationBridge::Current() const {
    return current_.has_value() ? &*current_ : nullptr;
}

ChannelAutomationBridge::ToolOutcome ChannelAutomationBridge::CreateReminder(
    const nlohmann::json& input) {
    ToolOutcome outcome;
    if (store_ == nullptr) {
        outcome.error_code = "channel_job.domain_unavailable";
        outcome.error = "automation 域不在(任务账未装配),暂时不能设置定时任务。";
        return outcome;
    }
    const TurnContext* ctx = Current();
    if (ctx == nullptr) {
        outcome.error_code = "channel_job.not_in_channel_turn";
        outcome.error = "该工具只在渠道会话里可用(本地会话没有渠道身份)。";
        return outcome;
    }
    if (!input.is_object()) {
        outcome.error_code = "channel_job.bad_input";
        outcome.error = "入参必须是 JSON 对象。";
        return outcome;
    }
    // 交付面当前只支持单聊(SendReply 按 direct 编码;QQ 首版群聊关闭)。
    // 群聊建单归群聊开放批次,不在这里假装能投递。
    if (ctx->conversation_kind != channel::ConversationKind::Direct) {
        outcome.error_code = "channel_job.direct_only";
        outcome.error = "当前只在单聊会话里支持设置定时任务(结果要投回本会话)。";
        return outcome;
    }
    // description:必填非空,超帽明拒。
    std::string description;
    if (input.contains("description") && input["description"].is_string()) {
        description = input["description"].get<std::string>();
    }
    if (description.empty()) {
        outcome.error_code = "channel_job.bad_input";
        outcome.error = "description 为空:请向用户确认提醒内容后再设置。";
        return outcome;
    }
    if (description.size() > kDescriptionCapBytes) {
        outcome.error_code = "channel_job.bad_input";
        outcome.error = "description 超长(上限 4000 字节),请精简任务描述。";
        return outcome;
    }
    // 时间规格:at_ms(once)与 cron(周期)二选一,都得显式。
    const bool has_at = input.contains("at_ms");
    const bool has_cron = input.contains("cron") && input["cron"].is_string() &&
                          !input["cron"].get<std::string>().empty();
    if (has_at == has_cron) {
        outcome.error_code = "channel_job.bad_input";
        outcome.error = "时间规格须且只须给一项:at_ms(单次,UTC 毫秒)或 cron(周期,五字段)。"
                        "缺时间或歧义时先向用户问清,不要猜。";
        return outcome;
    }
    std::string timezone = "UTC";
    if (input.contains("timezone") && input["timezone"].is_string() &&
        !input["timezone"].get<std::string>().empty()) {
        timezone = input["timezone"].get<std::string>();
    }
    gateway::AutomationStore::JobSpec spec;
    spec.prompt = ComposeJobPrompt(description, ctx->channel_id, ctx->account_id,
                                   ctx->conversation_id, ctx->sender_id);
    spec.timezone = timezone;
    spec.owner_channel = ctx->channel_id;
    spec.owner_account = ctx->account_id;
    spec.owner_sender = ctx->sender_id;
    spec.delivery_channel = ctx->channel_id;
    spec.delivery_account = ctx->account_id;
    spec.delivery_conversation = ctx->conversation_id;
    if (has_at) {
        const auto at_ms = channel::qq::ParseLooseInt64(input["at_ms"]);
        if (!at_ms.has_value()) {
            outcome.error_code = "channel_job.bad_input";
            outcome.error = "at_ms 不是合法整数(UTC 毫秒)。";
            return outcome;
        }
        // 过期明拒(§11.2:当晚时间已过要明确询问,不静默改明天)。
        if (*at_ms <= ctx->received_at_ms) {
            outcome.error_code = "channel_job.time_in_past";
            outcome.error = "at_ms 不晚于来信时间:时间已过,请向用户确认新的时间,不要自行顺延。";
            return outcome;
        }
        if (*at_ms - ctx->received_at_ms > kMaxOnceLeadMs) {
            outcome.error_code = "channel_job.bad_input";
            outcome.error = "at_ms 距今超过十年,多半是日期算错了,请与用户核对。";
            return outcome;
        }
        spec.kind = gateway::ScheduleKind::Once;
        spec.due_at_ms = *at_ms;
    } else {
        spec.kind = gateway::ScheduleKind::Cron;
        spec.cron_expr = input["cron"].get<std::string>();
    }
    // 创建幂等键(§11.2:原始来信 ID + 稳定操作身份):同信重发/同轮重调
    // 同键,同载荷回原回执,异载荷 conflict 如实回。
    const std::string idempotency_key = "chanjob-create:" + ctx->channel_id + ":" +
                                        ctx->account_id + ":msg:" + ctx->message_id;
    const std::int64_t now_ms = now_ms_ ? now_ms_() : DefaultNowMs();
    const auto receipt = store_->CreateJob(spec, now_ms, idempotency_key);
    if (!receipt.accepted && !receipt.duplicate) {
        outcome.error_code = receipt.error_code;
        outcome.error = receipt.error_code == "automation.revision_conflict"
                            ? "同一条消息里已经设置过一笔不同规格的任务(幂等冲突),请向用户说明。"
                            : "任务账落不了(" + receipt.error_code + ")。";
        return outcome;
    }
    // 回执带完整计划(§11.2:回复列完整日期、时间、时区和 jobId)。
    nlohmann::json payload = nlohmann::json::object();
    payload["jobId"] = receipt.job_id;
    payload["kind"] = gateway::ToString(spec.kind);
    payload["timezone"] = timezone;
    payload["duplicate"] = receipt.duplicate;
    gateway::ScheduleSpec schedule;
    schedule.kind = spec.kind;
    schedule.due_at_ms = spec.due_at_ms;
    schedule.interval_seconds = spec.interval_seconds;
    schedule.cron_expr = spec.cron_expr;
    schedule.timezone = spec.timezone;
    schedule.misfire = spec.misfire;
    schedule.anchor_ms = now_ms;  // interval 锚点语义与 store 建账一致
    const auto next = gateway::FirstSlotAfter(schedule, now_ms);
    if (next.found) {
        payload["nextFireMs"] = next.utc_ms;
    }
    if (spec.kind == gateway::ScheduleKind::Once) {
        payload["dueAtMs"] = spec.due_at_ms;
    }
    outcome.ok = true;
    outcome.duplicate = receipt.duplicate;
    outcome.payload = std::move(payload);
    return outcome;
}

ChannelAutomationBridge::ToolOutcome ChannelAutomationBridge::ListReminders() {
    ToolOutcome outcome;
    if (store_ == nullptr) {
        outcome.error_code = "channel_job.domain_unavailable";
        outcome.error = "automation 域不在(任务账未装配),暂时查不了定时任务。";
        return outcome;
    }
    const TurnContext* ctx = Current();
    if (ctx == nullptr) {
        outcome.error_code = "channel_job.not_in_channel_turn";
        outcome.error = "该工具只在渠道会话里可用(本地会话没有渠道身份)。";
        return outcome;
    }
    const std::int64_t now_ms = now_ms_ ? now_ms_() : DefaultNowMs();
    nlohmann::json jobs = nlohmann::json::array();
    for (const auto& job : store_->ListJobs()) {
        if (!OwnedBy(job, *ctx)) {
            continue;  // 只看归属自己的(跨用户不泄露,§11.2)
        }
        nlohmann::json item = nlohmann::json::object();
        item["jobId"] = job.job_id;
        item["state"] = gateway::ToString(job.state);
        item["kind"] = gateway::ToString(job.schedule_kind);
        item["timezone"] = job.timezone;
        item["revision"] = job.revision;
        item["prompt"] = job.prompt;
        if (job.state == gateway::AutomationJobState::Active) {
            gateway::ScheduleSpec schedule = SpecOfJob(job);
            const std::int64_t from = schedule.kind == gateway::ScheduleKind::Once
                                          ? 0
                                          : (job.schedule_cursor_ms > now_ms ? job.schedule_cursor_ms
                                                                             : now_ms);
            const auto next = gateway::FirstSlotAfter(schedule, from);
            if (next.found) {
                item["nextFireMs"] = next.utc_ms;
            }
        }
        jobs.push_back(std::move(item));
    }
    nlohmann::json payload = nlohmann::json::object();
    payload["jobs"] = std::move(jobs);
    outcome.ok = true;
    outcome.payload = std::move(payload);
    return outcome;
}

ChannelAutomationBridge::ToolOutcome ChannelAutomationBridge::CancelReminder(
    const nlohmann::json& input) {
    ToolOutcome outcome;
    if (store_ == nullptr) {
        outcome.error_code = "channel_job.domain_unavailable";
        outcome.error = "automation 域不在(任务账未装配),暂时取消不了定时任务。";
        return outcome;
    }
    const TurnContext* ctx = Current();
    if (ctx == nullptr) {
        outcome.error_code = "channel_job.not_in_channel_turn";
        outcome.error = "该工具只在渠道会话里可用(本地会话没有渠道身份)。";
        return outcome;
    }
    std::string job_id;
    if (input.is_object() && input.contains("jobId") && input["jobId"].is_string()) {
        job_id = input["jobId"].get<std::string>();
    }
    if (job_id.empty()) {
        outcome.error_code = "channel_job.bad_input";
        outcome.error = "jobId 为空:取消前先用 list_reminders 查用户自己的任务。";
        return outcome;
    }
    const auto job = store_->FindJob(job_id);
    if (!job.has_value() || !OwnedBy(*job, *ctx)) {
        // 不泄露他人任务的存在(§11.2 "跨用户不泄露任务")。
        outcome.error_code = "channel_job.not_your_job";
        outcome.error = "没有找到属于当前用户的任务 " + job_id + "。可先用 list_reminders 核对。";
        return outcome;
    }
    const std::string idempotency_key = "chanjob-cancel:" + ctx->channel_id + ":" +
                                        ctx->account_id + ":msg:" + ctx->message_id + ":" + job_id;
    const std::int64_t now_ms = now_ms_ ? now_ms_() : DefaultNowMs();
    const auto receipt = store_->CancelJob(job_id, job->revision, now_ms, idempotency_key);
    if (!receipt.accepted && !receipt.duplicate) {
        outcome.error_code = receipt.error_code;
        outcome.error = receipt.error_code == "automation.revision_conflict"
                            ? "任务刚被改过(revision 变了),请重查后再取消。"
                            : "取消落不了账(" + receipt.error_code + ")。";
        return outcome;
    }
    nlohmann::json payload = nlohmann::json::object();
    payload["jobId"] = job_id;
    payload["state"] = "cancelled";
    payload["duplicate"] = receipt.duplicate;
    outcome.ok = true;
    outcome.duplicate = receipt.duplicate;
    outcome.payload = std::move(payload);
    return outcome;
}

// ---------------------------------------------------------------------------
// 三枚工具(薄壳:校验与归属闸全在桥上)
// ---------------------------------------------------------------------------

namespace {

using ToolOutcome = ChannelAutomationBridge::ToolOutcome;

tools::Tool::Result AsToolResult(const ToolOutcome& outcome) {
    if (!outcome.ok) {
        return tools::Tool::Result::Error("[" + outcome.error_code + "] " + outcome.error);
    }
    nlohmann::json payload = outcome.payload;
    if (payload.is_object()) {
        payload["ok"] = true;
    }
    return tools::Tool::Result::Text(payload.dump());
}

class CreateReminderTool final : public tools::Tool {
public:
    explicit CreateReminderTool(std::shared_ptr<ChannelAutomationBridge> bridge)
        : bridge_(std::move(bridge)) {}
    std::string name() const override { return kChannelCreateReminderTool; }
    std::string description() const override {
        return "为当前聊天用户设置一笔定时任务/提醒。时间解析由你完成:把用户的自然语言时间"
               "(含时区)折算成 at_ms(单次,UTC 毫秒)或五字段 cron(周期),并显式传 timezone"
               "(IANA 名,如 Asia/Shanghai)。时间已过或含糊时先问用户,不要猜。创建成功回执带"
               " jobId 与下次触发时间,向用户复述完整日期、时间、时区。";
    }
    nlohmann::json input_schema() const override {
        return nlohmann::json::object({
            {"type", "object"},
            {"properties",
             nlohmann::json::object({
                 {"description", nlohmann::json::object({{"type", "string"},
                                                          {"description", "任务/提醒内容"}})},
                 {"at_ms", nlohmann::json::object({{"type", "integer"},
                                                   {"description", "单次触发时刻(UTC 毫秒)"}})},
                 {"cron",
                  nlohmann::json::object({{"type", "string"},
                                          {"description", "周期表达式(分 时 日 月 周,五字段)"}})},
                 {"timezone",
                  nlohmann::json::object({{"type", "string"}, {"description", "IANA 时区名"}})},
             })},
            {"required", nlohmann::json::array({"description"})},
        });
    }
    tools::Tool::Result execute(const nlohmann::json& input) override {
        return AsToolResult(bridge_->CreateReminder(input));
    }

private:
    std::shared_ptr<ChannelAutomationBridge> bridge_;
};

class ListRemindersTool final : public tools::Tool {
public:
    explicit ListRemindersTool(std::shared_ptr<ChannelAutomationBridge> bridge)
        : bridge_(std::move(bridge)) {}
    std::string name() const override { return kChannelListRemindersTool; }
    std::string description() const override {
        return "列出当前聊天用户自己的全部定时任务(jobId/状态/计划/下次触发);只包含归属该"
               "用户的任务。用户问\"我有哪些提醒\"时用它。";
    }
    nlohmann::json input_schema() const override {
        return nlohmann::json::object({{"type", "object"}, {"properties", nlohmann::json::object()}});
    }
    tools::Tool::Result execute(const nlohmann::json& /*input*/) override {
        return AsToolResult(bridge_->ListReminders());
    }

private:
    std::shared_ptr<ChannelAutomationBridge> bridge_;
};

class CancelReminderTool final : public tools::Tool {
public:
    explicit CancelReminderTool(std::shared_ptr<ChannelAutomationBridge> bridge)
        : bridge_(std::move(bridge)) {}
    std::string name() const override { return kChannelCancelReminderTool; }
    std::string description() const override {
        return "取消当前聊天用户自己的一笔定时任务(按 jobId)。只能取消归属该用户的任务;先"
               "用 list_reminders 拿到 jobId。";
    }
    nlohmann::json input_schema() const override {
        return nlohmann::json::object({
            {"type", "object"},
            {"properties",
             nlohmann::json::object({
                 {"jobId", nlohmann::json::object({{"type", "string"},
                                                    {"description", "任务 id(list_reminders 查得)"}})},
             })},
            {"required", nlohmann::json::array({"jobId"})},
        });
    }
    tools::Tool::Result execute(const nlohmann::json& input) override {
        return AsToolResult(bridge_->CancelReminder(input));
    }

private:
    std::shared_ptr<ChannelAutomationBridge> bridge_;
};

}  // namespace

void RegisterChannelAutomationTools(tools::ToolRegistry& registry,
                                    std::shared_ptr<ChannelAutomationBridge> bridge) {
    if (bridge == nullptr) {
        return;
    }
    if (registry.Find(kChannelCreateReminderTool) == nullptr) {
        tools::ToolRegistration registration;
        registration.tool = std::make_unique<CreateReminderTool>(bridge);
        registration.source_kind = tools::ToolSourceKind::Builtin;
        registration.effect_class = tools::EffectClass::LocalReversible;
        registration.idempotency = tools::Idempotency::Idempotent;
        registry.Register(std::move(registration));
    }
    if (registry.Find(kChannelListRemindersTool) == nullptr) {
        tools::ToolRegistration registration;
        registration.tool = std::make_unique<ListRemindersTool>(bridge);
        registration.source_kind = tools::ToolSourceKind::Builtin;
        registration.effect_class = tools::EffectClass::ReadOnlyLocal;
        registration.idempotency = tools::Idempotency::Idempotent;
        registry.Register(std::move(registration));
    }
    if (registry.Find(kChannelCancelReminderTool) == nullptr) {
        tools::ToolRegistration registration;
        registration.tool = std::make_unique<CancelReminderTool>(bridge);
        registration.source_kind = tools::ToolSourceKind::Builtin;
        registration.effect_class = tools::EffectClass::LocalReversible;
        registration.idempotency = tools::Idempotency::Idempotent;
        registry.Register(std::move(registration));
    }
}

}  // namespace lubancode::runtime
