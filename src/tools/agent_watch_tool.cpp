// agent_watch_tool.hpp 的实现:typed 校验、lineage 鉴权、有界快照与长等。
#include "tools/agent_watch_tool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cli/i18n.hpp"
#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

namespace lubancode::tools {

namespace {

// 单子 §9.2 的上限。schema 里也声明了,execute 侧再守一道(钩子改参/旧
// 调用方不经 schema 时这里是最后一道闸)。
constexpr std::size_t kMaxTaskIds = 16;
constexpr int kMaxWaitMs = 30000;
constexpr std::size_t kMaxEventsPerTask = 50;

const char* TaskStateTag(AgentTaskState state) {
    switch (state) {
        case AgentTaskState::Running:
            return "running";
        case AgentTaskState::WaitingChildren:
            return "waiting_children";
        case AgentTaskState::Completing:
            return "completing";
        case AgentTaskState::Done:
            return "done";
        case AgentTaskState::Failed:
            return "failed";
        case AgentTaskState::Cancelled:
            return "cancelled";
        case AgentTaskState::BudgetExhausted:
            return "budget_exhausted";
    }
    return "unknown";
}

const char* TaskEventKindTag(AgentTaskEventKind kind) {
    switch (kind) {
        case AgentTaskEventKind::UserMessage:
            return "user_message";
        case AgentTaskEventKind::AssistantText:
            return "assistant_text";
        case AgentTaskEventKind::AssistantReasoning:
            return "assistant_reasoning";
        case AgentTaskEventKind::ToolStart:
            return "tool_start";
        case AgentTaskEventKind::ToolResult:
            return "tool_result";
        case AgentTaskEventKind::SteeringMessage:
            return "steering_message";
        case AgentTaskEventKind::CompactCheckpoint:
            return "compact_checkpoint";
        case AgentTaskEventKind::Completion:
            return "completion";
        case AgentTaskEventKind::Failure:
            return "failure";
    }
    return "unknown";
}

// ms 龄:从未发生过(时刻为零)回 -1,调用方折 null。负时长钳 0。
std::int64_t AgeMs(const std::chrono::steady_clock::time_point now,
                   const std::chrono::steady_clock::time_point at) {
    if (at.time_since_epoch().count() == 0) {
        return -1;
    }
    const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - at).count();
    return delta < 0 ? 0 : delta;
}

nlohmann::json AgeOrNull(const std::chrono::steady_clock::time_point now,
                         const std::chrono::steady_clock::time_point at) {
    const std::int64_t age = AgeMs(now, at);
    return age < 0 ? nlohmann::json() : nlohmann::json(age);
}

}  // namespace

std::string AgentWatchTool::name() const {
    return "agent_watch";
}

std::string AgentWatchTool::description() const {
    // 文案在 src/prompts/tools/<语言>/agent_watch.md,兜底是这里原文。描述里
    // 明写合同:状态变化才再等,不要短周期轮询(单子 P1-0 验收项)。
    return ToolText(
        "agent_watch", "description",
        "查看子代理任务的监督快照(状态/阶段/健康/静默龄/重试数),可等任务发生变化再返回。只读:不停任务、"
        "不传话(传话用 agent_message,停止用面板 x)。用法:先不带 wait_ms 查一次拿 revision;要等就带上"
        "上次的 revision 作 after_revision 与 wait_ms(最多 30 秒)——修订前进、任务进终态、用户介入或取消都会"
        "提前唤醒。状态变化才再等,不要短周期轮询:不要循环用 wait_ms=500 之类短等待反复打卡,等待期间宿主"
        "零开销,等到变化即可。task_ids 不填:main 看全部运行中的根任务,子代理只看自己的直接子任务;最多"
        "16 只。include=events 额外回该任务 after_revision 之后的至多 50 枚结构化事件(只有类型与工具名,不含"
        "正文);diagnostic 只给 main,给诊断计数与稳定错误码,同样不含正文与思考。");
}

nlohmann::json AgentWatchTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";
    schema["additionalProperties"] = false;

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json task_ids_items = nlohmann::json::object();
    task_ids_items["type"] = "integer";
    task_ids_items["minimum"] = 1;
    nlohmann::json task_ids_prop = nlohmann::json::object();
    task_ids_prop["type"] = "array";
    task_ids_prop["items"] = std::move(task_ids_items);
    task_ids_prop["maxItems"] = static_cast<int>(kMaxTaskIds);
    task_ids_prop["uniqueItems"] = true;
    task_ids_prop["description"] = ToolText(
        "agent_watch", "param.task_ids",
        "要查看的任务号(名册里的 #N)。不填 = main 看全部运行中根任务、子代理看自己的直接子任务。最多 16 只。");
    properties["task_ids"] = std::move(task_ids_prop);

    nlohmann::json after_revision_prop = nlohmann::json::object();
    after_revision_prop["type"] = "integer";
    after_revision_prop["minimum"] = 0;
    after_revision_prop["description"] = ToolText(
        "agent_watch", "param.after_revision",
        "上次调用返回的 revision。本次快照若与它相同且 wait_ms>0,就睡到任务变化或超时;不同则立即返回。");
    properties["after_revision"] = std::move(after_revision_prop);

    nlohmann::json wait_ms_prop = nlohmann::json::object();
    wait_ms_prop["type"] = "integer";
    wait_ms_prop["minimum"] = 0;
    wait_ms_prop["maximum"] = kMaxWaitMs;
    wait_ms_prop["description"] = ToolText(
        "agent_watch", "param.wait_ms",
        "最多等多久(毫秒,上限 30000)。0 = 只取快照不等。等待零开销且有界:用户介入、任务取消、会话收场都会"
        "提前唤醒;不要用短 wait_ms 循环轮询。");
    properties["wait_ms"] = std::move(wait_ms_prop);

    nlohmann::json include_prop = nlohmann::json::object();
    include_prop["type"] = "string";
    include_prop["enum"] = std::vector<std::string>{"summary", "events", "diagnostic"};
    include_prop["description"] = ToolText(
        "agent_watch", "param.include",
        "输出档位:summary(缺省,状态短快照)/events(另带结构化事件流)/diagnostic(只给 main,诊断计数"
        "与稳定错误码)。");
    properties["include"] = std::move(include_prop);

    schema["properties"] = std::move(properties);
    return schema;
}

Tool::Result AgentWatchTool::execute(const nlohmann::json& input, const ToolExecutionContext& context) {
    if (agent_tool_ == nullptr) {
        return {lubancode::cli::tr("agent_watch.unavailable"), true};
    }
    TaskLedger& ledger = agent_tool_->ledger();

    // ---- typed 校验(单子 §9.2;additionalProperties=false 由这里执法)----
    if (!input.is_object()) {
        return {lubancode::cli::tr("agent_watch.invalid"), true};
    }
    for (const auto& item : input.items()) {
        if (item.key() != "task_ids" && item.key() != "after_revision" && item.key() != "wait_ms" &&
            item.key() != "include") {
            return {lubancode::cli::trf("agent_watch.unknown_key", item.key()), true};
        }
    }
    std::vector<int> task_ids;
    if (const auto it = input.find("task_ids"); it != input.end() && !it->is_null()) {
        if (!it->is_array()) {
            return {lubancode::cli::tr("agent_watch.task_ids_invalid"), true};
        }
        if (it->size() > kMaxTaskIds) {
            return {lubancode::cli::trf("agent_watch.too_many_tasks", static_cast<int>(kMaxTaskIds)), true};
        }
        for (const auto& id : *it) {
            if (!id.is_number_integer()) {
                return {lubancode::cli::tr("agent_watch.task_ids_invalid"), true};
            }
            task_ids.push_back(id.get<int>());
        }
    }
    std::uint64_t after_revision = 0;
    if (const auto it = input.find("after_revision"); it != input.end() && !it->is_null()) {
        if (!it->is_number_integer()) {
            return {lubancode::cli::tr("agent_watch.after_revision_invalid"), true};
        }
        const std::int64_t raw = it->get<std::int64_t>();
        if (raw < 0) {
            return {lubancode::cli::tr("agent_watch.after_revision_invalid"), true};
        }
        after_revision = static_cast<std::uint64_t>(raw);
    }
    int wait_ms = 0;
    if (const auto it = input.find("wait_ms"); it != input.end() && !it->is_null()) {
        if (!it->is_number_integer()) {
            return {lubancode::cli::tr("agent_watch.wait_ms_invalid"), true};
        }
        wait_ms = it->get<int>();
        if (wait_ms < 0) {
            return {lubancode::cli::tr("agent_watch.wait_ms_invalid"), true};
        }
        wait_ms = std::min(wait_ms, kMaxWaitMs);  // 超 30 秒的请求钳到上限,不报错
    }
    bool want_events = false;
    bool want_diagnostic = false;
    if (const auto it = input.find("include"); it != input.end() && !it->is_null()) {
        if (!it->is_string()) {
            return {lubancode::cli::tr("agent_watch.include_invalid"), true};
        }
        const std::string include = it->get<std::string>();
        if (include == "events") {
            want_events = true;
        } else if (include == "diagnostic") {
            if (caller_task_id_ != 0) {
                // 单子 §9.2:diagnostic 只给 main——子代理要了就稳定拒绝,
                // 不降档冒充(降档会让模型以为拿到了诊断账)。
                return {lubancode::cli::tr("agent_watch.diagnostic_denied"), true};
            }
            want_diagnostic = true;
        } else if (include != "summary") {
            return {lubancode::cli::tr("agent_watch.include_invalid"), true};
        }
    }

    // ---- 目标集:显式点名走 Detail,空缺省按 lineage 折默认集 -------------
    std::vector<AgentTaskSnapshot> targets;
    if (!task_ids.empty()) {
        targets.reserve(task_ids.size());
        for (const int id : task_ids) {
            const auto snapshot = ledger.Detail(id);
            if (!snapshot.has_value()) {
                return {lubancode::cli::trf("agent_watch.not_found", id), true};
            }
            // lineage 鉴权(单子 §9.2):main 看整棵树;子代理只看直接孩子
            // ——越 lineage(兄弟/旁系/孙辈/自己)一律稳定拒绝,拒绝文案
            // 不泄露目标的任何细节。
            if (caller_task_id_ != 0 && snapshot->parent_task_id != caller_task_id_) {
                return {lubancode::cli::trf("agent_watch.not_child", id), true};
            }
            targets.push_back(std::move(*snapshot));
        }
    } else {
        for (const auto& summary : ledger.Summaries()) {
            if (!IsAliveTaskState(summary.state)) {
                continue;  // 缺省集只看未终态
            }
            const bool visible = caller_task_id_ == 0 ? summary.parent_task_id == 0
                                                      : summary.parent_task_id == caller_task_id_;
            if (!visible) {
                continue;
            }
            auto detail = ledger.Detail(summary.id);
            if (detail.has_value()) {
                targets.push_back(std::move(*detail));
            }
        }
    }

    // ---- 等待(P1-0 的核心:condition variable,不忙轮询)------------------
    // after_revision 已是最新且要等:睡到监督可见修订前进 / 超时 / 取消旗
    //(父取消经台账 cancel 路径 notify;ESC 经会话侧外部唤醒口)。伪醒只
    // 是再查一遍谓词。无变化时这条线程零 CPU 挂在 cv 上(验收线)。
    bool timed_out = false;
    const std::uint64_t before = ledger.watch_generation();
    if (before == after_revision && wait_ms > 0) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
        const std::atomic<bool>* cancel = context.cancel;
        ledger.WaitForWatchChange(after_revision, deadline, [cancel]() {
            return cancel != nullptr && cancel->load(std::memory_order_acquire);
        });
    }
    const std::uint64_t generation = ledger.watch_generation();
    timed_out = generation == after_revision;

    // ---- 快照(醒来后现取,不拿等待前的旧账)------------------------------
    const auto now = std::chrono::steady_clock::now();
    nlohmann::json out = nlohmann::json::object();
    out["revision"] = generation;
    out["timed_out"] = timed_out;
    nlohmann::json tasks = nlohmann::json::array();
    for (const auto& target : targets) {
        // 终态转换可能发生在等待期间:重取一次详情,状态不撒谎。
        const auto fresh = ledger.Detail(target.id);
        const AgentTaskSnapshot& snapshot = fresh.has_value() ? *fresh : target;
        const agent::AgentProgressClock progress = ledger.ProgressOf(snapshot.id);
        const bool alive = IsAliveTaskState(snapshot.state);
        const auto end = alive ? now : snapshot.end_time;
        std::int64_t elapsed_ms = 0;
        if (snapshot.start_time.time_since_epoch().count() != 0 && end >= snapshot.start_time) {
            elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - snapshot.start_time).count();
        }
        nlohmann::json item = nlohmann::json::object();
        item["task_id"] = snapshot.id;
        item["title"] = FirstLineOf(snapshot.title.empty() ? "未命名" : snapshot.title);
        item["state"] = TaskStateTag(snapshot.state);
        item["health"] = agent::HealthTag(progress.health);
        item["stage"] = agent::SupervisionStageTag(progress.stage);
        item["elapsed_ms"] = elapsed_ms;
        item["last_transport_age_ms"] = AgeOrNull(now, progress.last_transport_at);
        item["last_progress_age_ms"] = AgeOrNull(now, progress.last_meaningful_progress_at);
        item["attempt"] = progress.request_attempt;
        item["retry_count"] = progress.retry_count;
        // 工具:正在跑的带上起跑龄;不在跑给最后一次(与 Dock 同一口径)。
        if (snapshot.activity.stage == AgentTaskActivity::Stage::Tool && !snapshot.activity.tool_name.empty()) {
            nlohmann::json tool = nlohmann::json::object();
            tool["name"] = snapshot.activity.tool_name;
            tool["age_ms"] = AgeMs(now, snapshot.activity.tool_started);
            item["tool"] = std::move(tool);
        } else if (!snapshot.activity.last_tool_name.empty()) {
            nlohmann::json tool = nlohmann::json::object();
            tool["name"] = snapshot.activity.last_tool_name;
            tool["age_ms"] = nullptr;
            item["tool"] = std::move(tool);
        }
        if (!progress.last_reason_code.empty()) {
            item["reason"] = progress.last_reason_code;
        }
        if (snapshot.stop_requested) {
            item["stop_requested"] = true;
        }
        if (want_events) {
            // 结构化事件(单子 §9.2:至多 50 枚,不带正文/思考/完整参数)。
            nlohmann::json events = nlohmann::json::array();
            for (const auto& event : ledger.EventsSince(snapshot.id, after_revision, kMaxEventsPerTask)) {
                nlohmann::json entry = nlohmann::json::object();
                entry["revision"] = event.revision;
                entry["kind"] = TaskEventKindTag(event.kind);
                if (!event.tool_name.empty()) {
                    entry["tool"] = event.tool_name;
                }
                if (event.is_error) {
                    entry["is_error"] = true;
                }
                if (event.streaming) {
                    entry["streaming"] = true;
                }
                events.push_back(std::move(entry));
            }
            item["events"] = std::move(events);
        }
        if (want_diagnostic) {
            // diagnostic 只给 main(上面已拒子代理):计数与稳定码,无正文。
            nlohmann::json diag = nlohmann::json::object();
            diag["transport_idle_ms"] = AgeOrNull(now, progress.last_transport_at);
            diag["execution_idle_ms"] = AgeOrNull(now, progress.last_execution_at);
            diag["progress_idle_ms"] = AgeOrNull(now, progress.last_meaningful_progress_at);
            diag["stale_rounds"] = progress.stale_rounds;
            diag["health_epoch"] = progress.health_epoch;
            diag["request_attempt"] = progress.request_attempt;
            diag["retry_count"] = progress.retry_count;
            if (!progress.last_reason_code.empty()) {
                diag["last_error_code"] = progress.last_reason_code;
            }
            // 下一动作与截止:墙钟是唯一有明确截止的硬线;软线由阶段定,
            // 监督拍 500ms 一轮(单子 §7.1 的尺子在那,不在这重复执法)。
            if (snapshot.wall_limit_secs > 0 && snapshot.start_time.time_since_epoch().count() != 0) {
                const auto wall_deadline = snapshot.start_time + std::chrono::seconds(snapshot.wall_limit_secs);
                if (alive && wall_deadline > now) {
                    diag["wall_remaining_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    wall_deadline - now)
                                                    .count();
                } else {
                    diag["wall_remaining_ms"] = 0;
                }
            }
            item["diagnostic"] = std::move(diag);
        }
        tasks.push_back(std::move(item));
    }
    out["tasks"] = std::move(tasks);
    return Tool::Result::Text(out.dump());
}

Tool::Result AgentWatchTool::execute(const nlohmann::json& input) {
    return execute(input, ToolExecutionContext{});
}

}  // namespace lubancode::tools
