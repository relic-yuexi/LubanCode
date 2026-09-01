// 子代理面板 presenter 实现(合同见 agent_panel_presenter.hpp)。函数体自
// interactive_session 的 BuildAgentPanelEntries/BuildAgentTaskTranscriptLines
// 与匿名区三只状态词函数原文搬家(改道:台账走参数、theme 走成员、查看态
// 展开档走参数),行为一字不差——注释一并随行。

#include "app/agent_panel_presenter.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

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

// 紧凑时长(监督器单 P1-1 的坞行口径):"45s" / "1m02s" / "11m52s" /
// "1h02m"。负数/零给 "0s"。不含 ANSI,窄宽截断交给坞行的既有裁剪。
std::string FormatDurationCompact(std::int64_t ms) {
    if (ms < 0) {
        ms = 0;
    }
    const std::int64_t total_seconds = ms / 1000;
    if (total_seconds < 60) {
        return std::to_string(total_seconds) + "s";
    }
    if (total_seconds < 3600) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%dm%02llds", static_cast<int>(total_seconds / 60),
                      static_cast<long long>(total_seconds % 60));
        return buf;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%dh%02dm", static_cast<int>(total_seconds / 3600),
                  static_cast<long long>((total_seconds % 3600) / 60));
    return buf;
}

// ms 龄;从未发生过(时刻为零)回 -1。
std::int64_t AgeMsOf(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point at) {
    if (at.time_since_epoch().count() == 0) {
        return -1;
    }
    const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - at).count();
    return delta < 0 ? 0 : delta;
}

// 监督小截(监督器单 P1-1 §十,Dock 每行只添一小截):重连次数/静默龄。
// 尺子与监督拍同一套(单子 §7.1:各相位各用各的钟)——流式/等首字节看
// 传输静默,等下一轮/等孩子看执行静默;跑工具的静默由活度短语
// ("工具 x · Ns")自己报,这里不重复。Healthy/已收场不出段。
std::string SupervisionSegment(const agent::AgentProgressClock& progress,
                               const lubancode::tools::AgentTaskActivity* activity,
                               std::chrono::steady_clock::time_point now) {
    using H = agent::AgentHealthState;
    using S = agent::AgentSupervisionStage;
    if (progress.health == H::Healthy || progress.health == H::Terminal) {
        return std::string();
    }
    std::vector<std::string> parts;
    if (progress.health == H::Recovering) {
        // "重连 2/3":自动重试次数/上限(单子 §十样例)。上限是请求恢复链
        // 的常量(api::kMaxRequestAttempts),这里不引 api 头,按同一数值写死
        // 并注释拴住——两处不同步时以 api 侧为准。
        parts.push_back(trf("agent_supervision.recovering", progress.retry_count, 3));
    } else if (progress.health == H::Degraded) {
        parts.push_back(tr("agent_supervision.degraded"));
    }
    // 静默龄:与监督拍同钟。跑工具时活度短语已带工具龄,不重复报。
    const bool tool_age_already_shown =
        activity != nullptr && activity->stage == lubancode::tools::AgentTaskActivity::Stage::Tool;
    if (!tool_age_already_shown) {
        std::int64_t silence_ms = -1;
        switch (progress.stage) {
            case S::Queued:
            case S::Preparing:
            case S::Completing:
            case S::Terminal:
                break;  // 收口相位本来就该安静,不报静默
            case S::AwaitingFirstByte:
            case S::StreamingThinking:
            case S::StreamingText:
            case S::AwaitingToolInputComplete:
            case S::Recovering:
                silence_ms = AgeMsOf(now, progress.last_transport_at);
                break;
            case S::RunningTool:
            case S::AwaitingNextModelTurn:
            case S::WaitingChildren:
                silence_ms = AgeMsOf(now, progress.last_execution_at);
                break;
        }
        if (silence_ms < 0 && progress.stage != S::RunningTool) {
            // 没有可量的钟(还没收过传输/执行):用任务耗时兜底——"出生至今
            // 没动静"也是静默。
            silence_ms = AgeMsOf(now, progress.request_started_at);
        }
        if (silence_ms >= 0) {
            parts.push_back(trf("agent_supervision.silent", FormatDurationCompact(silence_ms)));
        }
    }
    std::string joined;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            joined += " · ";
        }
        joined += parts[i];
    }
    return joined;
}

// 面板短因文案(规格"现场三"):失败须分得出接口错/工具错/空结论——导航
// 坞只放短因,完整错误进 transcript(Enter 切进该会话再看)。
std::string OutcomeReasonText(lubancode::tools::TaskOutcomeReason reason) {
    using R = lubancode::tools::TaskOutcomeReason;
    switch (reason) {
        case R::ApiError:
            return tr("agent_status.reason_api_error");
        case R::StepLimitExhausted:
            return tr("agent_status.reason_step_limit");
        case R::TimeBudgetExhausted:
            return tr("agent_status.reason_time_budget");
        case R::TokenBudgetExhausted:
            return tr("agent_status.reason_token_budget");
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

// 状态短话(规格"现场三"+P0-4 状态机+P1-1 Dock 等子任务态):导航坞行与
// 查看态统计行共用的一套拼装——运行中优先出实时活跃短语(治"死秒表"),
// 没进流的空档退回"运行中";停止信号已发还没收口时(面板 x /停全部)压上
// "停止中"前缀,不当死 Running(子代理 x 停止失效单的可见回执);终态带
// 短因。WaitingChildren/Completing 是 P0-4 补的活态(自己已写完、正等/正
// 收口孩子)——从前这两态漏在 switch 外,会掉进末尾的"失败 · 未注明原因"
// 假象(单子 §13.1"等子任务态"落点);现在与 Running 同一支路处理,只是
// 不发活跃短语(没有请求在飞),换一句专属短话。一处写死,两处口径永远
// 一致。
std::string AgentStateWord(lubancode::tools::AgentTaskState state, int steps_used, int step_limit,
                           lubancode::tools::TaskOutcomeReason outcome_reason,
                           const lubancode::tools::AgentTaskActivity* activity,
                           std::chrono::steady_clock::time_point now, bool stop_requested = false,
                           std::size_t waiting_children = 0,
                           const agent::AgentProgressClock* progress = nullptr) {
    using S = lubancode::tools::AgentTaskState;
    if (state == S::Running || state == S::WaitingChildren || state == S::Completing) {
        std::string word;
        if (stop_requested) {
            word = tr("agent_status.state_stopping");
        } else if (state == S::WaitingChildren) {
            word = trf("agent_status.state_waiting_children", static_cast<int>(waiting_children));
        } else if (state == S::Completing) {
            word = tr("agent_status.state_completing");
        } else if (activity != nullptr) {
            word = AgentActivityWord(*activity, now);
        }
        if (word.empty()) {
            word = tr("agent_status.state_running");
        }
        // 监督小截(P1-1):阶段静默/重连次数,排在活度短语之后、步数账之前
        // ——"#7 镜像流 重连 2/3 · 静默 1m02s · 总 11m52s"的那一小截。
        if (progress != nullptr) {
            const std::string supervision = SupervisionSegment(*progress, activity, now);
            if (!supervision.empty()) {
                word += " · " + supervision;
            }
        }
        // 步数常驻可见(真机实测 P2-1:Dock 要"已用步数、上限、累计 token、
        // 最后一次工具"四样都看得到):有上限带 N/M,没上限跑过步数也带 N。
        if (step_limit > 0) {
            word += trf("agent_status.budget_suffix", steps_used, step_limit);
        } else if (steps_used > 0) {
            word += trf("agent_status.steps_suffix", steps_used);
        }
        // 最后一次工具:工具跑着时活度短语已带名字;不在跑的空档(等首字节/
        // 思考/正文)补"上次 <工具>",长任务分得清卡在哪枚工具后头。
        if (activity != nullptr && activity->stage != lubancode::tools::AgentTaskActivity::Stage::Tool &&
            !activity->last_tool_name.empty()) {
            word += trf("agent_status.last_tool", activity->last_tool_name);
        }
        return word;
    }
    switch (state) {
        case S::Done:
            return tr("agent_status.state_done");
        case S::Cancelled:
            return trf("agent_status.state_stopped_reason", tr("agent_status.reason_user_stop"));
        case S::BudgetExhausted:
            // 断的是时间/token 线时步数上限多半是 0,不画"N/0 步"的假账——
            // 短因写明线别,步数与预算明细在详情行看(P2-6)。
            if (step_limit > 0) {
                return trf("agent_status.state_exhausted", steps_used, step_limit);
            }
            return trf("agent_status.state_stopped_reason", OutcomeReasonText(outcome_reason));
        case S::Failed:
        case S::Running:
        case S::WaitingChildren:
        case S::Completing:
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
        // 活态判定(P1-1 Dock 等子任务态):WaitingChildren/Completing 与
        // Running 同算"在跑"——否则这两态会被折进闲置汇总、状态词也会掉进
        // "失败 · 未注明原因"的假象(单子 §13.1)。
        entry.running = lubancode::tools::IsAliveTaskState(task.state);
        entry.failed = task.state == lubancode::tools::AgentTaskState::Failed ||
                       task.state == lubancode::tools::AgentTaskState::BudgetExhausted;
        entry.cancelled = task.state == lubancode::tools::AgentTaskState::Cancelled;
        // Dock 画树(单子 §13.1):lineage 投影直读 TaskSummaries,不另养账。
        entry.parent_task_id = task.parent_task_id;
        entry.depth = task.depth;
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
        const std::size_t waiting_children = task.state == lubancode::tools::AgentTaskState::WaitingChildren
                                                 ? agent_tool->ledger().AliveChildCount(task.id)
                                                 : 0;
        const std::string state_word =
            AgentStateWord(task.state, task.steps_used, task.step_limit, task.outcome_reason,
                           task.state == lubancode::tools::AgentTaskState::Running ? &task.activity : nullptr, now,
                           task.stop_requested, waiting_children, &task.progress);
        // 监督色辅助(P1-1):颜色只作辅助,行文本已带语义;plain 主题渲染处
        // 自动退默认淡色。
        if (entry.running) {
            using H = agent::AgentHealthState;
            switch (task.progress.health) {
                case H::Quiet:
                case H::SuspectTransport:
                case H::SuspectTool:
                case H::SuspectAgent:
                    entry.health_tint = lubancode::cli::AgentHealthTint::Quiet;
                    break;
                case H::Recovering:
                    entry.health_tint = lubancode::cli::AgentHealthTint::Recovering;
                    break;
                case H::Degraded:
                    entry.health_tint = lubancode::cli::AgentHealthTint::Degraded;
                    break;
                case H::Healthy:
                case H::Terminal:
                    break;
            }
        }
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
    // 活执行合同只存原始派工说明；cwd 与角色来自宿主运行账，不从正文猜。
    {
        const auto section = [&lines, &theme](const char* head, const std::string& body) {
            if (body.empty()) {
                return;
            }
            std::string flat;
            for (const char c : body) {
                flat.push_back(c == '\n' || c == '\r' || c == '\t' ? ' ' : c);
            }
            lines.push_back("  " + theme.stats + std::string("[") + head + "] " +
                            lubancode::cli::TruncateUtf8ToDisplayWidth(flat, 200) + theme.reset);
        };
        section("派工说明", snapshot->spec != nullptr ? snapshot->spec->instructions : snapshot->prompt);
        section("运行目录", snapshot->effective_cwd);
        section("角色", snapshot->agent_type);
        // 父子身份首屏可见(单子 §13.2):直接父/根/深度一行。
        if (snapshot->parent_task_id != 0 || snapshot->depth > 1) {
            lines.push_back("  " + theme.stats + "[lineage] 父任务 #" + std::to_string(snapshot->parent_task_id) +
                            " · 根 #" + std::to_string(snapshot->root_task_id) + " · 深度 " +
                            std::to_string(snapshot->depth) + theme.reset);
        }
    }
    // 统计与当前状态行(规格"现场三"+活跃度单):与导航坞行同一套口径(共用
    // AgentStateWord + agent_status.summary)——agent 视图像 main 一样有
    // 账可查,不只剩一张 tool-use 流水单。运行中的任务用时现算;首字节耗时
    // (当前这轮请求从发出到首个流事件)一并写在这行,长任务分得清"在想"
    // 还是"没来"。
    {
        // alive(P1-1 Dock 等子任务态):WaitingChildren/Completing 也还在
        // 占资源、用时仍在走——end_time 还没落(任务没收尾),用 now 现算。
        // running 单指严格 Running(有活跃请求飞着):只有它才画首字节/活跃
        // 短语,WaitingChildren/Completing 换专属短话(见 AgentStateWord)。
        const bool alive = lubancode::tools::IsAliveTaskState(snapshot->state);
        const bool running = snapshot->state == lubancode::tools::AgentTaskState::Running;
        const auto now = std::chrono::steady_clock::now();
        const auto end = alive ? now : snapshot->end_time;
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
        const std::size_t waiting_children = snapshot->state == lubancode::tools::AgentTaskState::WaitingChildren &&
                                                     agent_tool != nullptr
                                                 ? agent_tool->ledger().AliveChildCount(snapshot->id)
                                                 : 0;
        // 监督账(P1-1):统计行的监督小截与查看态的监督折叠行共用这一份
        // 现值(真账在 TaskRecord.progress)。
        const agent::AgentProgressClock progress =
            agent_tool != nullptr ? agent_tool->ledger().ProgressOf(snapshot->id) : agent::AgentProgressClock{};
        std::string stats_line =
            "  " + theme.stats +
            trf("agent_status.summary",
                AgentStateWord(snapshot->state, snapshot->steps_used, snapshot->step_limit,
                               snapshot->outcome.reason,
                               running ? &snapshot->activity : nullptr, now, snapshot->stop_requested,
                               waiting_children, &progress),
                static_cast<int>(snapshot->tool_calls.size()), token_text,
                lubancode::cli::FormatSeconds(seconds));
        if (running && snapshot->activity.first_byte_ms >= 0) {
            stats_line += " · " + trf("agent_activity.first_byte", snapshot->activity.first_byte_ms);
        }
        lines.push_back(stats_line + theme.reset);
        // 预算明细行(真机实测 P2-6:Dock 要能看出"为什么还在跑"):派出时
        // 设了哪几根线、各用到哪了——步数/时间/token 三样,只列设了的。
        {
            std::vector<std::string> parts;
            if (snapshot->step_limit > 0) {
                parts.push_back(std::to_string(snapshot->steps_used) + "/" + std::to_string(snapshot->step_limit) +
                                " 步");
            }
            if (snapshot->wall_limit_secs > 0) {
                parts.push_back(lubancode::cli::FormatSeconds(seconds) + "/" +
                                std::to_string(snapshot->wall_limit_secs) + "s");
            }
            if (snapshot->token_limit > 0) {
                parts.push_back(lubancode::cli::FormatTokenCount(tokens) + "/" +
                                lubancode::cli::FormatTokenCount(snapshot->token_limit));
            }
            if (!parts.empty()) {
                std::string joined = parts[0];
                for (std::size_t i = 1; i < parts.size(); ++i) {
                    joined += " · " + parts[i];
                }
                lines.push_back("  " + theme.stats + trf("agent_panel.budget_head", joined) + theme.reset);
            }
        }
        // 监督折叠行(监督器单 P1-1 §十):一行小截,不堆诊断报告——最后
        // 传输/最后完整提交/最后实质进展/当前请求尝试/最近错误码/下一动作
        // 与截止(墙钟是唯一有明确截止的硬线;软线归监督拍,这里只报账)。
        // 终态任务不出这行(账已收进 outcome)。
        if (alive) {
            const auto age_text = [&now](std::chrono::steady_clock::time_point at) {
                const std::int64_t age = AgeMsOf(now, at);
                return age < 0 ? tr("agent_supervision.age_never") : FormatDurationCompact(age);
            };
            std::string wall_text = tr("agent_supervision.wall_none");
            if (snapshot->wall_limit_secs > 0) {
                const auto wall_deadline =
                    snapshot->start_time + std::chrono::seconds(snapshot->wall_limit_secs);
                wall_text = wall_deadline > now ? FormatDurationCompact(std::chrono::duration_cast<
                                                    std::chrono::milliseconds>(wall_deadline - now)
                                                    .count())
                                                : tr("agent_supervision.wall_over");
            }
            const std::string reason_text =
                progress.last_reason_code.empty() ? tr("agent_supervision.no_reason") : progress.last_reason_code;
            lines.push_back("  " + theme.stats +
                            trf("agent_supervision.detail", age_text(progress.last_transport_at),
                                age_text(progress.last_execution_at), age_text(progress.last_meaningful_progress_at),
                                progress.request_attempt, reason_text, wall_text) +
                            theme.reset);
        }
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
