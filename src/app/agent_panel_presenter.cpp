// 子代理面板 presenter 实现(合同见 agent_panel_presenter.hpp)。函数体自
// interactive_session 的 BuildAgentPanelEntries/BuildAgentTaskTranscriptLines
// 与匿名区三只状态词函数原文搬家(改道:台账走参数、theme 走成员、查看态
// 展开档走参数),行为一字不差——注释一并随行。

#include "app/agent_panel_presenter.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "cli/agent_panel.hpp"
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "cli/markdown.hpp"
#include "cli/theme.hpp"
#include "cli/transcript.hpp"
#include "tools/agent_tool.hpp"

namespace lubancode::app {

using lubancode::cli::tr;
using lubancode::cli::trf;

namespace {

// 面板短因文案(规格"现场三"):失败须分得出接口错/工具错/空结论——导航
// 坞只放短因,完整错误进 transcript(Enter 切进该会话再看)。
std::string OutcomeReasonText(lubancode::tools::TaskOutcomeReason reason) {
    using R = lubancode::tools::TaskOutcomeReason;
    switch (reason) {
        case R::ApiError:
            return tr("agent_status.reason_api_error");
        case R::StepLimitExhausted:
            return tr("agent_status.reason_step_limit");
        case R::MaxContext:
            return tr("agent_status.reason_max_context");
        case R::NoFinalText:
            return tr("agent_status.reason_no_final_text");
        case R::ToolError:
            return tr("agent_status.reason_tool_error");
        case R::UserStop:
            return tr("agent_status.reason_user_stop");
        case R::WallClockTimeout:
            return tr("agent_status.reason_wall_clock");
        case R::ProtocolError:
            return tr("agent_status.reason_protocol_error");
        case R::None:
            return tr("agent_status.reason_unknown");
    }
    return tr("agent_status.reason_unknown");
}

// 实时活跃短语(规格"子代理活跃度不可见"):运行中的任务按当前阶段换这一
// 条——等首字节(带已等秒数)/思考中·N 字/正文 N 字/工具 名·M 秒。按阶段
// 换,不堆三段;只报计数,不碰思考与正文内容。
std::string AgentActivityWord(const lubancode::tools::AgentTaskActivity& activity,
                              std::chrono::steady_clock::time_point now) {
    using A = lubancode::tools::AgentTaskActivity;
    switch (activity.stage) {
        case A::Stage::WaitingFirstByte: {
            const int seconds = activity.request_started.time_since_epoch().count() == 0
                                    ? 0
                                    : static_cast<int>(
                                          std::chrono::duration_cast<std::chrono::seconds>(now -
                                                                                            activity.request_started)
                                              .count());
            return trf("agent_activity.waiting", seconds);
        }
        case A::Stage::Thinking:
            return trf("agent_activity.thinking", activity.reasoning_chars);
        case A::Stage::Text:
            return trf("agent_activity.text", activity.text_chars);
        case A::Stage::Tool: {
            const int seconds = activity.tool_started.time_since_epoch().count() == 0
                                    ? 0
                                    : static_cast<int>(
                                          std::chrono::duration_cast<std::chrono::seconds>(now -
                                                                                            activity.tool_started)
                                              .count());
            return trf("agent_activity.tool", activity.tool_name, seconds);
        }
        case A::Stage::None:
            break;
    }
    return std::string();
}

// 状态短话(规格"现场三"):导航坞行与查看态统计行共用的一套拼装——
// 运行中优先出实时活跃短语(治"死秒表"),没进流的空档退回"运行中";终态
// 带短因。一处写死,两处口径永远一致。
std::string AgentStateWord(lubancode::tools::AgentTaskState state, int steps_used, int step_limit,
                           lubancode::tools::TaskOutcomeReason outcome_reason,
                           const lubancode::tools::AgentTaskActivity* activity,
                           std::chrono::steady_clock::time_point now) {
    using S = lubancode::tools::AgentTaskState;
    if (state == S::Running) {
        std::string word;
        if (activity != nullptr) {
            word = AgentActivityWord(*activity, now);
        }
        if (word.empty()) {
            word = tr("agent_status.state_running");
        }
        if (step_limit > 0) {
            word += trf("agent_status.budget_suffix", steps_used, step_limit);
        }
        return word;
    }
    switch (state) {
        case S::Done:
            return tr("agent_status.state_done");
        case S::Cancelled:
            return trf("agent_status.state_stopped_reason", tr("agent_status.reason_user_stop"));
        case S::BudgetExhausted:
            return trf("agent_status.state_exhausted", steps_used, step_limit);
        case S::Failed:
        case S::Running:
            return trf("agent_status.state_failed_reason", OutcomeReasonText(outcome_reason));
    }
    return trf("agent_status.state_failed_reason", OutcomeReasonText(outcome_reason));
}

}  // namespace

AgentPanelPresenter::AgentPanelPresenter(const lubancode::cli::Theme& theme) : theme_(theme) {}

// 后台子代理面板:轻量全量列表(0.28.x 起不截 8 只,详情另走
// BuildAgentTaskDetail;统一台账后前台任务也在同一份列表里)。摘要行口径
// 沿用 agent_status.* 那套 i18n,空闲与流式两处 painter 一个格式。
std::vector<lubancode::cli::AgentPanelEntry> AgentPanelPresenter::Entries(
    lubancode::tools::AgentTool* agent_tool) {
    std::vector<lubancode::cli::AgentPanelEntry> out;
    if (agent_tool == nullptr) {
        return out;
    }
    const std::uint64_t revision = agent_tool->TaskRevision();
    if (revision != cached_revision_) {
        cached_tasks_ = agent_tool->TaskSummaries();
        cached_revision_ = revision;
    }
    const auto now = std::chrono::steady_clock::now();
    for (const auto& task : cached_tasks_) {
        lubancode::cli::AgentPanelEntry entry;
        entry.task_id = task.id;
        entry.name = task.agent_type + " #" + std::to_string(task.id);
        entry.running = task.state == lubancode::tools::AgentTaskState::Running;
        entry.failed = task.state == lubancode::tools::AgentTaskState::Failed ||
                       task.state == lubancode::tools::AgentTaskState::BudgetExhausted;
        entry.cancelled = task.state == lubancode::tools::AgentTaskState::Cancelled;
        // 活动坞退场账(规格"现场一"新规矩):done 且结果已交回 main、或被
        // 用户中止——从导航坞退场;失败/耗尽留短错。台账(TaskDetail)照查,
        // 这里只标退场,不清任何数据。done 未投递是过渡态,留在坞里等投递。
        entry.done_delivered = task.state == lubancode::tools::AgentTaskState::Done && task.delivered;
        const auto end = entry.running ? now : task.end_time;
        const double seconds = std::chrono::duration<double>(end - task.start_time).count();
        const std::int64_t tokens = task.total_input_tokens() + task.output_tokens;
        // tokens 三态(规格根因三):未报告且已跑过步数就写"未报告",不画 0
        // ——0 会误导成"服务端一枚 token 都没跑",实则可能烧满了输出预算。
        // 运行中的一律未报告即写"未报告":首步流中 steps_used 还是 0,但
        // 请求已发出、token 正在烧,"0 tokens"正是用户看着诡异的假信号。
        const std::string token_text =
            task.usage_reported || (!entry.running && task.steps_used == 0)
                ? lubancode::cli::FormatTokenCount(tokens)
                : tr("agent_status.tokens_not_reported");
        // 状态短话(规格"现场三/四"+活跃度单):导航坞只放短因——完成/失败 ·
        // 接口报错/耗尽 · 40/40 轮/停下 · 用户中止;完整错误进 transcript
        // (Enter 查看)。运行中优先出实时活跃短语(思考中·N 字/工具 名·M 秒),
        // 长思考任务的坞行秒级跳动,不再是死秒表。正数预算派出即可见:运行中
        // 带"N/M 步",不等撞墙才揭晓。
        const std::string state_word =
            AgentStateWord(task.state, task.steps_used, task.step_limit, task.outcome_reason,
                           task.state == lubancode::tools::AgentTaskState::Running ? &task.activity : nullptr, now);
        entry.state = trf("agent_status.summary", state_word, task.tool_call_count, token_text,
                          lubancode::cli::FormatSeconds(seconds));  // 列表行只认真正短 title;旧任务没有 title 就显示"未命名子代理 #N"
        // ——绝不回退到 prompt 前若干字(prompt 只在详情里出现)。
        entry.title = task.title.empty() ? trf("agent_panel.untitled", task.id) : task.title;
        entry.content_revision = task.content_revision;
        if (task.pending_message_count > 0) {
            // 有话已排给这只代理、还没在轮次边界送达——列表行尾巴明写,
            // 详情里再列原文,不让"已排给 subagent #N"只活在提交那一瞬。
            entry.state += " · " + trf("agent_panel.pending_note", task.pending_message_count);
        }
        out.push_back(std::move(entry));
    }
    return out;
}

// 查看态的会话视口行(规格"现场三"):子代理与 main 同款会话——消息账
// (TaskEvents,按时间追加)逐事件铺开:用户消息、助手正文、思考折叠块、
// 工具卡、介入、压缩检查点、终局。渲染组件全部复用 main 的:正文走
// RenderMarkdown(/resume 同款),工具卡与思考走 FormatTranscriptItem 的
// SubTool/Thinking 条目(同一套折叠/宽字符/截断规矩),绝不在这里手搓
// 第二套显示器。任务不在台账(被清理/演示假代理)时给一行占位;旧版派出
// 的任务没有事件账,退铺结论并明说,不拿"任务说明+工具流水"冒充会话。
std::vector<std::string> AgentPanelPresenter::TaskTranscriptLines(lubancode::tools::AgentTool* agent_tool,
                                                                  int task_id, int width,
                                                                  bool agent_view_expanded) {
    const lubancode::cli::Theme& theme = theme_;
    std::vector<std::string> lines;
    std::optional<lubancode::tools::AgentTaskSnapshot> snapshot;
    if (agent_tool != nullptr) {
        snapshot = agent_tool->TaskDetail(task_id);
    }
    // 头行先从面板条目拿身份与短标题(演示钩子的假代理也认得),台账里
    // 再补一遍明细。
    std::string name = "subagent #" + std::to_string(task_id);
    std::string title;
    for (const auto& entry : Entries(agent_tool)) {
        if (entry.task_id == task_id) {
            name = entry.name;
            title = entry.title;
            break;
        }
    }
    if (!snapshot.has_value()) {
        lines.push_back(lubancode::cli::TruncateUtf8ToDisplayWidth(
            "── " + name + (title.empty() ? std::string() : " · " + title) + " ──", std::max(0, width - 1)));
        lines.push_back("  " + tr("agent_panel.detail_gone"));
        return lines;
    }
    if (snapshot->title.empty() == false && title.empty()) {
        title = snapshot->title;
    }
    lines.push_back(lubancode::cli::TruncateUtf8ToDisplayWidth(
        trf("agent_panel.view_header", name,
            title.empty() ? trf("agent_panel.untitled", snapshot->id) : title),
        std::max(0, width - 1)));
    lines.push_back(std::string("  [") +
                    tr(snapshot->foreground ? "agent_panel.source_foreground" : "agent_panel.source_background") +
                    "]");
    // 统计与当前状态行(规格"现场三"+活跃度单):与导航坞行同一套口径(共用
    // AgentStateWord + agent_status.summary)——agent 视图像 main 一样有
    // 账可查,不只剩一张 tool-use 流水单。运行中的任务用时现算;首字节耗时
    // (当前这轮请求从发出到首个流事件)一并写在这行,长任务分得清"在想"
    // 还是"没来"。
    {
        const bool running = snapshot->state == lubancode::tools::AgentTaskState::Running;
        const auto now = std::chrono::steady_clock::now();
        const auto end = running ? now : snapshot->end_time;
        const double seconds = end > snapshot->start_time
                                   ? std::chrono::duration<double>(end - snapshot->start_time).count()
                                   : 0.0;
        const std::int64_t tokens = snapshot->total_input_tokens() + snapshot->output_tokens;
        // tokens 三态(规格根因三):与导航坞行同一套口径——未报告且已跑过
        // 步数写"未报告",一步没跑才是真 0。
        const std::string token_text =
            snapshot->usage_reported || snapshot->steps_used == 0
                ? lubancode::cli::FormatTokenCount(tokens)
                : tr("agent_status.tokens_not_reported");
        std::string stats_line =
            "  " + theme.stats +
            trf("agent_status.summary",
                AgentStateWord(snapshot->state, snapshot->steps_used, snapshot->step_limit,
                               snapshot->outcome.reason,
                               running ? &snapshot->activity : nullptr, now),
                static_cast<int>(snapshot->tool_calls.size()), token_text,
                lubancode::cli::FormatSeconds(seconds));
        if (running && snapshot->activity.first_byte_ms >= 0) {
            stats_line += " · " + trf("agent_activity.first_byte", snapshot->activity.first_byte_ms);
        }
        lines.push_back(stats_line + theme.reset);
    }

    const std::vector<lubancode::tools::AgentTaskEvent> events =
        agent_tool != nullptr ? agent_tool->TaskEvents(task_id)
                              : std::vector<lubancode::tools::AgentTaskEvent>{};
    if (events.empty()) {
        // 旧版派出的任务没有消息账:明说,再铺仅存的结论(不拼工具流水
        // 冒充会话,规格"不做"第三条)。
        lines.push_back("  " + tr("agent_panel.events_unavailable"));
        const std::string& result = snapshot->result.empty() ? snapshot->live_output : snapshot->result;
        if (!result.empty()) {
            for (const auto& line : lubancode::cli::RenderMarkdown(result, theme, width)) {
                lines.push_back(line);
            }
        }
        return lines;
    }

    // ---- 复用 main 的渲染组件(规格"现场三":共用 renderer,不共用 history) ----
    int next_item_id = 1;
    const auto push_rendered = [&](const std::string& rendered) {
        std::string rest = rendered;
        std::size_t cut = 0;
        do {
            cut = rest.find('\n');
            std::string chunk = cut == std::string::npos ? rest : rest.substr(0, cut);
            if (cut != std::string::npos) {
                rest.erase(0, cut + 1);
            }
            if (chunk.empty() && cut == std::string::npos) {
                break;
            }
            lines.push_back(chunk);
        } while (cut != std::string::npos);
    };
    const auto push_markdown = [&](const std::string& header, const std::string& text) {
        lines.push_back(header);
        for (const auto& line : lubancode::cli::RenderMarkdown(text, theme, width)) {
            lines.push_back(line);
        }
    };
    const auto tool_item = [&](const lubancode::tools::AgentTaskEvent& start,
                               const lubancode::tools::AgentTaskEvent* done) {
        return lubancode::cli::MakeAgentTaskToolItem(
            next_item_id++, start.tool_name, start.input_json, done != nullptr,
            done != nullptr && done->is_error, done != nullptr ? done->result : std::string());
    };

    // 工具卡配对:ToolStart 等 ToolResult 成一张终态卡;流尾没等到的画
    // Running 卡。中间穿插的正文/思考已由账面时序保证不乱。
    std::optional<lubancode::tools::AgentTaskEvent> pending_tool_start;
    for (const auto& event : events) {
        switch (event.kind) {
            case lubancode::tools::AgentTaskEventKind::UserMessage:
                push_markdown(theme.confirm + "> " + tr("cmd.resume.history.user") + theme.reset, event.text);
                break;
            case lubancode::tools::AgentTaskEventKind::SteeringMessage:
                push_markdown(theme.confirm + "> " + tr("agent_panel.event_steering") + theme.reset, event.text);
                break;
            case lubancode::tools::AgentTaskEventKind::AssistantText:
                // 流式正文尾巴(追加需求):查看态就是这只代理此刻的实时会话
                // ——已流出的正文按渲染版铺开,重铺拍自然带出增量。
                push_markdown(theme.banner + "● " + tr("cmd.resume.history.assistant") + theme.reset, event.text);
                break;
            case lubancode::tools::AgentTaskEventKind::AssistantReasoning: {
                // 流式思考尾巴(追加需求"查看态实时思考流"):工厂折成与 main
                // 流式思考同款折叠规矩的条目——Running 头行「思考中 · N 字」
                // 随重铺拍跳动;Ctrl+O 展开看长文(FormatTranscriptItem 的
                // thinking_live 分支自带"约一屏后截断收口")。
                const lubancode::cli::TranscriptItem item =
                    lubancode::cli::MakeAgentTaskThinkingItem(next_item_id++, event.text, event.streaming);
                push_rendered(lubancode::cli::FormatTranscriptItem(item, theme, width,
                                                                   /*expanded=*/event.streaming && agent_view_expanded));
                break;
            }
            case lubancode::tools::AgentTaskEventKind::ToolStart:
                if (pending_tool_start.has_value()) {
                    // 上一张卡没等到结果(异常路径):先画 Running 卡,不吞。
                    push_rendered(
                        lubancode::cli::FormatTranscriptItem(tool_item(*pending_tool_start, nullptr), theme, width,
                                                             /*expanded=*/false));
                }
                pending_tool_start = event;
                break;
            case lubancode::tools::AgentTaskEventKind::ToolResult: {
                const lubancode::tools::AgentTaskEvent* done = &event;
                if (pending_tool_start.has_value()) {
                    push_rendered(lubancode::cli::FormatTranscriptItem(
                        tool_item(*pending_tool_start, done), theme, width, /*expanded=*/false));
                    pending_tool_start.reset();
                } else {
                    // 没配上 start(旧账边缘):单画一张只有结果的卡。
                    lubancode::tools::AgentTaskEvent pseudo = event;
                    pseudo.input_json.clear();
                    push_rendered(lubancode::cli::FormatTranscriptItem(tool_item(pseudo, done), theme, width,
                                                                       /*expanded=*/false));
                }
                break;
            }
            case lubancode::tools::AgentTaskEventKind::CompactCheckpoint:
                lines.push_back(theme.stats + tr("cmd.resume.history.compact") + theme.reset);
                break;
            case lubancode::tools::AgentTaskEventKind::Completion:
                lines.push_back(theme.banner + "✓ " + tr("agent_status.state_done") + theme.reset);
                for (const auto& line : lubancode::cli::RenderMarkdown(event.text, theme, width)) {
                    lines.push_back(line);
                }
                break;
            case lubancode::tools::AgentTaskEventKind::Failure:
                lines.push_back(theme.error + "× " + tr("agent_panel.event_failed") + theme.reset);
                for (const auto& line : lubancode::cli::RenderMarkdown(event.text, theme, width)) {
                    lines.push_back(line);
                }
                break;
        }
    }
    if (pending_tool_start.has_value()) {
        push_rendered(lubancode::cli::FormatTranscriptItem(tool_item(*pending_tool_start, nullptr), theme, width,
                                                           /*expanded=*/false));
    }
    // 未送达的介入消息:排在账尾,等轮次边界注入。
    if (agent_tool != nullptr) {
        const auto pending = agent_tool->PendingTaskMessages(task_id);
        if (!pending.empty()) {
            lines.push_back(theme.stats + trf("agent_panel.detail_pending_head", pending.size()) + theme.reset);
            for (const auto& message : pending) {
                lines.push_back("  * " + lubancode::cli::TruncateUtf8ToDisplayWidth(message, std::max(0, width - 5)));
            }
        }
    }
    return lines;
}

}  // namespace lubancode::app
