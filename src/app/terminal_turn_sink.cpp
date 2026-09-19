// TerminalTurnSink 的实现(骨架拆解批二余款 + 按代理状态投影单 P1)。
// 闭包原文自批二两轨并行时 BuildCallbacks 的显示半边逐句搬来,水的来路
// 换成事件流;P1 把每枚事件的处置拆成"收账(ApplyEvent,恒跑)→ 按当前
// 页投影绘制(DrawEvent)"两半,语句次序照旧,画的样子一字不改。

#include "app/terminal_turn_sink.hpp"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "api/types.hpp"
#include "app/agent_view_registry.hpp"
#include "cli/agent_view_state.hpp"
#include "cli/console_input.hpp"  // UpdateStatusLineContext
#include "cli/context_tracker.hpp"
#include "cli/format_utils.hpp"  // BuildCacheNote
#include "cli/live_transcript.hpp"
#include "cli/tool_display.hpp"
#include "cli/ui_trace.hpp"
#include "runtime/turn_collector.hpp"
#include "runtime/turn_runtime.hpp"  // TurnUsageStats
#include "runtime/turn_view.hpp"     // TurnItemViewState
#include "skills/workflow_recorder.hpp"
#include "tools/tool.hpp"

namespace lubancode::app {

void TerminalTurnSink::Emit(const runtime::ServerEvent& event) {
    // P0 证据基建:接收关卡——从路事件也记(标 sub: 前缀),跳过发生在
    // 记账之后,事后能对账"谁的事件在什么时候进过 UI 边界"。
    if (cli::ui_trace::Enabled()) {
        const bool subordinate = event.payload.value("subordinate", false);
        cli::ui_trace::NoteReceived(
            ingredients_.view_registry != nullptr ? ingredients_.view_registry->MainKey()
                                                  : cli::AgentViewKey{},
            ingredients_.view_registry != nullptr ? ingredients_.view_registry->view_epoch() : 0,
            (subordinate ? "sub:" : "") + TraceKindOf(event), event.envelope.seq);
    }
    // 从路(子代理/PTC stub)整枚跳过:主屏不刷嵌套回合,画面规矩不变。
    // 从路事件可能来自子代理任务线程,跳过发生在投递之前,队列与画笔都不沾。
    if (event.payload.value("subordinate", false)) {
        return;
    }
    // 条 1(画面隔网):流内事件只投队列(产生线程不碰终端),控制路
    // 事件就地画(画前泵排干 pending,次序与老路一致)。分类见
    // IsStreamOrigin——SSE 回调能产生的就那几种,后续批网络真分家时,
    // 投递路的范围顺着那只口子扩。
    if (IsStreamOrigin(event)) {
        ui_pump_.PostDelta(event);
        return;
    }
    ui_pump_.DispatchInline(event);
}

bool TerminalTurnSink::IsStreamOrigin(const runtime::ServerEvent& event) {
    if (event.kind == runtime::ServerEventKind::ItemDelta) {
        // 正文/思考 delta:SSE 洪峰的正主,只投不画。
        return event.item_kind == runtime::ItemKind::Text || event.item_kind == runtime::ItemKind::Thinking;
    }
    if (event.kind == runtime::ServerEventKind::ItemStarted ||
        event.kind == runtime::ServerEventKind::ItemCompleted) {
        // 服务端内置工具(Responses 的 web_search 一类)的起止也在流内
        // 回调里产生;没有同步交互钉在它们身后,照投不误。
        return event.item_kind == runtime::ItemKind::Tool && event.payload.value("builtin", false);
    }
    return false;
}

void TerminalTurnSink::StopUiPump() { ui_pump_.StopAndDrain(); }

std::string TerminalTurnSink::TraceKindOf(const runtime::ServerEvent& event) {
    using runtime::ServerEventKind;
    switch (event.kind) {
        case ServerEventKind::ItemDelta:
            return event.item_kind == runtime::ItemKind::Thinking ? "thinking_delta" : "text_delta";
        case ServerEventKind::ItemStarted:
            return "item_started";
        case ServerEventKind::ItemCompleted:
            return "item_completed";
        case ServerEventKind::UsageUpdated:
            return "usage";
        case ServerEventKind::ModelStepStarted:
            return "model_step";
        case ServerEventKind::ToolBatchStarted:
            return "tool_batch_started";
        case ServerEventKind::ToolBatchFinished:
            return "tool_batch_finished";
        case ServerEventKind::TurnStarted:
            return "turn_started";
        case ServerEventKind::TurnCompleted:
            return "turn_completed";
        default:
            return runtime::ToString(event.kind);
    }
}

// 泵消费侧/就地路的总口(P1:先记账,再通知画屏——单子 §四.3)。
void TerminalTurnSink::HandleEvent(const runtime::ServerEvent& event) {
    // ItemCompleted(Tool) 的配对信息先取后收:收账半边会把 open_tools_
    // 摘掉,绘制半边还要用(tool_use_id/name)。
    std::optional<OpenTool> completed_tool;
    if (event.kind == runtime::ServerEventKind::ItemCompleted && event.item_kind == runtime::ItemKind::Tool) {
        const auto it = open_tools_.find(event.item_id);
        if (it != open_tools_.end()) {
            completed_tool = it->second;
        }
    }
    AgentViewRegistry* registry = ingredients_.view_registry;
    if (registry == nullptr) {
        // 旧路(单发/单测/未装登记簿):收账直走、绘制恒开,行为与从前
        // 一字不差。P0 取证也盖这条路(无投影时的混屏正是要在它身上抓)。
        std::uint64_t revision = 0;
        ApplyEvent(event, revision);
        DrawEvent(event, completed_tool);
        if (cli::ui_trace::Enabled()) {
            cli::ui_trace::NoteCommitted(cli::AgentViewKey{}, 0, TraceKindOf(event), revision, "sink");
        }
        return;
    }
    // 收账恒跑:main 离屏期间视图账/工具配对/统计照常更新(单子 §七 P1
    // 验收),修订号每笔 +1。
    std::uint64_t revision = 0;
    ApplyEvent(event, revision);
    // 绘制闸:当前页是不是 main、这笔修订号过没过打印水位。被拒的内容
    // 都在登记簿的 ledge 里,切回重铺按水位接续,不漏不重。
    const bool draw = registry->MainShouldDraw(revision);
    ingredients_.display->SetPageVisible(draw);
    ingredients_.body_tracker->SetPageVisible(draw);
    // 绘制半边照常调:闸关时 ToolDisplay/StreamBodyTracker 内部走"只记账
    // 不上屏"的支路(transcript 条目、快照、状态一个不少),屏上零字节。
    // 不许按闸整枚跳过——那会把 ToolDisplay 的条目账一并截掉。
    DrawEvent(event, completed_tool);
    if (cli::ui_trace::Enabled()) {
        // P0 证据基建:帧提交关卡。被闸掉的绘制不记 commit——事后对账,
        // "离屏期间有 apply 无 commit"正是投影生效的直接证据。
        if (draw) {
            cli::ui_trace::NoteCommitted(registry->MainKey(), registry->view_epoch(), TraceKindOf(event),
                                         revision, "sink");
        } else {
            cli::ui_trace::NoteApplied(registry->MainKey(), registry->view_epoch(),
                                       "gated:" + TraceKindOf(event), revision);
        }
    }
}

// 收账半边:视图账(经登记簿进锁,revision 带回本笔修订号)、工具配对
// (open_tools_)、usage/统计、录制。不碰 ToolDisplay/StreamBodyTracker。
void TerminalTurnSink::ApplyEvent(const runtime::ServerEvent& event, std::uint64_t& revision) {
    AgentViewRegistry* registry = ingredients_.view_registry;
    // 本笔事件的"账本变异"整体进登记簿的锁:一把锁里改 collector 与
    // open_tools_,修订号恰 +1。无登记簿时直走(旧路)。
    const auto apply = [&](auto&& body) {
        if (registry != nullptr) {
            revision = registry->ApplyToMainTurn(body);
        } else {
            body();
        }
    };
    switch (event.kind) {
        case runtime::ServerEventKind::ItemDelta: {
            const std::string& text = event.text;
            if (event.item_kind == runtime::ItemKind::Text ||
                event.item_kind == runtime::ItemKind::Thinking) {
                const bool thinking = event.item_kind == runtime::ItemKind::Thinking;
                apply([&] {
                    if (ingredients_.view_collector != nullptr) {
                        ingredients_.view_collector->OnTextDelta(text, thinking);
                    }
                });
            }
            break;
        }
        case runtime::ServerEventKind::ItemStarted: {
            if (event.item_kind != runtime::ItemKind::Tool) {
                break;
            }
            const std::string tool_use_id = event.payload.value("tool_use_id", std::string());
            const std::string name = event.payload.value("tool_name", std::string());
            const nlohmann::json input =
                event.payload.contains("input") ? event.payload["input"] : nlohmann::json::object();
            const bool builtin = event.payload.value("builtin", false);
            apply([&] {
                open_tools_[event.item_id] = OpenTool{tool_use_id, name};
                if (!builtin) {
                    // recorder:录一遍生成技能的监听挂点;装了 trace hub 的轮次
                    // 吃 hub 投影(canonical),这里不旁听。
                    if (ingredients_.recorder != nullptr && !ingredients_.trace_projection_installed) {
                        ingredients_.recorder->RecordToolCall(name, input);
                    }
                    if (ingredients_.view_collector != nullptr) {
                        ingredients_.view_collector->OnToolStarted(tool_use_id, name, input);
                    }
                }
            });
            break;
        }
        case runtime::ServerEventKind::ItemCompleted: {
            if (event.item_kind != runtime::ItemKind::Tool) {
                break;  // 思考/正文收口无账可记(绘制半边管画面)
            }
            const auto it = open_tools_.find(event.item_id);
            if (it == open_tools_.end()) {
                break;  // 迟到/陌生终态:丢弃不误伤(与 ToolDisplay 同规矩)
            }
            const OpenTool tool = it->second;
            const std::string result_text = event.payload.value("result", std::string());
            const bool is_error = event.payload.value("is_error", false);
            const bool builtin = event.payload.value("builtin", false);
            apply([&] {
                open_tools_.erase(event.item_id);
                if (event.outcome == runtime::Outcome::Cancelled) {
                    // 适配器 Finish 的 Cancelled 兜底补账:只进事件流,不碰
                    // 画面(老路上这类补账压根没有显示回调可走)。
                    return;
                }
                if (builtin) {
                    return;  // 服务端内置工具:不进视图账/录制(老路同款)
                }
                if (ingredients_.recorder != nullptr && !ingredients_.trace_projection_installed) {
                    ingredients_.recorder->RecordToolResult(tool.name, is_error, result_text);
                }
                if (ingredients_.view_collector != nullptr) {
                    // ESC 后补的合成结果(is_error 且 cancel 已置)按
                    // Interrupted 记,不冒充跑过又失败;真失败照 Failed。
                    std::optional<runtime::TurnItemViewState> forced;
                    if (is_error && ingredients_.cancel_flag != nullptr &&
                        ingredients_.cancel_flag->load()) {
                        forced = runtime::TurnItemViewState::Interrupted;
                    }
                    ingredients_.view_collector->OnToolFinished(tool.tool_use_id, result_text, is_error,
                                                                forced);
                }
            });
            break;
        }
        case runtime::ServerEventKind::UsageUpdated: {
            api::UsageReport report;
            report.usage.input_tokens = event.payload.value("input_tokens", std::int64_t{0});
            report.usage.output_tokens = event.payload.value("output_tokens", std::int64_t{0});
            report.usage.cache_read_tokens = event.payload.value("cache_read_tokens", std::int64_t{0});
            report.usage.cache_creation_tokens = event.payload.value("cache_creation_tokens", std::int64_t{0});
            report.usage.output_reasoning_tokens = event.payload.value("reasoning_tokens", std::int64_t{0});
            report.step_index = event.payload.value("step_index", 0);
            report.provider_response_id = event.payload.value("provider_response_id", std::string());
            report.reported_by_provider = event.payload.value("reported_by_provider", false);
            // 缓存读/写明报位(缓存用量按 Wire 归一单 C2):新键优先,旧事件
            // 只有合并位时退回——读=写=合并位(旧口径分不开,如实如此)。
            report.cache_read_reported_by_provider =
                event.payload.value("cache_read_reported_by_provider",
                                    event.payload.value("cache_reported_by_provider", false));
            report.cache_creation_reported_by_provider =
                event.payload.value("cache_creation_reported_by_provider",
                                    event.payload.value("cache_reported_by_provider", false));
            report.usage_anomaly = event.payload.value("usage_anomaly", std::string());
            report.model = event.payload.value("model", std::string());
            report.cache_epoch = event.payload.value("cache_epoch", 1);
            report.epoch_break_reason = event.payload.value("epoch_break_reason", std::string());
            report.prefix_append_only = event.payload.value("prefix_append_only", true);
            // 问题 9 的每请求诊断账:从事件 payload 还原(键由
            // TurnEventAdapter::OnUsage 填,只含 hash/长度/枚举,不含正文)。
            report.epoch_first_request = event.payload.value("epoch_first_request", false);
            report.system_hash = event.payload.value("system_hash", std::string());
            report.tools_hash = event.payload.value("tools_hash", std::string());
            report.prefix_hash = event.payload.value("prefix_hash", std::string());
            report.stable_prefix_messages =
                event.payload.value("stable_prefix_messages", static_cast<std::size_t>(0));
            report.total_messages = event.payload.value("total_messages", static_cast<std::size_t>(0));
            report.wire_common_prefix_bytes = event.payload.value("wire_common_prefix_bytes", std::int64_t{-1});
            // 四层生命周期单 P1:Step 身份/尝试/耗时从事件 payload 还原
            //(键由 TurnEventAdapter::OnUsage 填;旧事件行没有这些键,缺省
            // 空/0,与"未接线"同貌,不造号)。
            report.step_id = event.payload.value("step_id", std::string());
            report.turn_id = event.payload.value("turn_id", std::string());
            report.attempts = event.payload.value("attempts", 0);
            report.api_duration_ms = event.payload.value("api_duration_ms", std::int64_t{0});
            report.stop_reason = event.payload.value("stop_reason", std::string());
            const bool reported = event.payload.value("reported", report.reported());
            apply([&] {
                if (ingredients_.view_collector != nullptr) {
                    ingredients_.view_collector->OnUsage(report);
                }
            });
            if (ingredients_.usage_stats != nullptr) {
                ingredients_.usage_stats->Add(report);
            }
            // ContextTracker 只认"最近一次请求"的真实用量,整个覆盖,不跟着
            // usage_stats 一起累加——语义区别见 cli/context_tracker.hpp 文件头。
            // ApplyUsage 兼管"provider 没回 usage"(四项全零)的语义:不清零、
            // 只把现有数字标成旧值;ESC/HTTP 错误路径压根走不到这里,不会
            // 把旧数伪装成本次新值。turn_id/step_index(问题 5)取自事件:
            // 外层用户轮次号 + 该轮内请求序,逐请求缓存表靠它们分组。
            // 诊断账(问题 9)同笔随行:epoch/追加律/稳定前缀/miss 分型,
            // 从上面还原的 report 抄进 CacheDiagnostics 递给 tracker。
            if (ingredients_.context_tracker != nullptr) {
                lubancode::cli::ContextTracker::CacheDiagnostics diag;
                diag.present = true;
                diag.cache_epoch = report.cache_epoch;
                diag.epoch_break_reason = report.epoch_break_reason;
                diag.prefix_append_only = report.prefix_append_only;
                diag.epoch_first_request = report.epoch_first_request;
                diag.system_hash = report.system_hash;
                diag.tools_hash = report.tools_hash;
                diag.prefix_hash = report.prefix_hash;
                diag.stable_prefix_messages = report.stable_prefix_messages;
                diag.total_messages = report.total_messages;
                diag.wire_common_prefix_bytes = report.wire_common_prefix_bytes;
                // C2 报告位随同一笔递进:usage/读/写明报位与异常位从事件
                // payload 还原(上面 report 已抄好),tracker 不再靠数字猜。
                lubancode::cli::ContextTracker::UsageReportFlags flags;
                flags.known = true;
                flags.usage_reported = report.reported_by_provider;
                flags.cache_read_reported = report.cache_read_reported_by_provider;
                flags.cache_creation_reported = report.cache_creation_reported_by_provider;
                flags.anomalous = !report.usage_anomaly.empty();
                ingredients_.context_tracker->ApplyUsage(report.usage, event.turn_id, report.step_index, diag,
                                                         flags);
                // usage 一到就把 context/tokens 两段发布给状态行数据源——
                // 只改数据不落笔(锁与重画事务在 cli::UpdateStatusLineContext
                // 里),回合内状态栏跟着前进,不必等整轮收口回外层循环重建
                // 快照;外层重建与这里读的是同一只 tracker,同一笔数,不存
                // 在先新后旧。子代理的 usage 走台账 sink 那份钩子,不进这里、
                // 不碰 tracker——主 context 不被独立子代理的上下文虚抬。
                cli::UpdateStatusLineContext(
                    ingredients_.context_tracker->UsagePercent(),
                    static_cast<std::int64_t>(ingredients_.context_tracker->current_tokens()),
                    static_cast<std::int64_t>(ingredients_.context_tracker->window_tokens()),
                    !ingredients_.context_tracker->usage_stale(),
                    // 缓存注记(缓存诊断单):cached_tokens 有则摆本场命中与
                    // 命中率,没回就写"未报告"——同一个 0 不糊。
                    cli::BuildCacheNote(*ingredients_.context_tracker, reported));
            }
            break;
        }
        case runtime::ServerEventKind::ModelStepStarted: {
            const int step_index = event.payload.value("step_index", 0);
            apply([&] {
                if (ingredients_.view_collector != nullptr) {
                    ingredients_.view_collector->OnModelStepStarted(step_index);
                }
            });
            break;
        }
        case runtime::ServerEventKind::ToolBatchStarted: {
            std::vector<std::string> ordered_ids;
            if (event.payload.contains("ordered_tool_use_ids") &&
                event.payload["ordered_tool_use_ids"].is_array()) {
                ordered_ids = event.payload["ordered_tool_use_ids"].get<std::vector<std::string>>();
            }
            const int step_index = event.payload.value("step_index", 0);
            const int batch_index = event.payload.value("batch_index", 0);
            apply([&] {
                if (ingredients_.view_collector != nullptr) {
                    ingredients_.view_collector->OnToolBatchStarted(step_index, batch_index, ordered_ids);
                }
            });
            break;
        }
        case runtime::ServerEventKind::ToolBatchFinished: {
            const bool interrupted = event.payload.value("interrupted", false);
            const int batch_index = event.payload.value("batch_index", 0);
            apply([&] {
                if (ingredients_.view_collector != nullptr) {
                    ingredients_.view_collector->OnToolBatchFinished(batch_index, interrupted);
                }
            });
            break;
        }
        default:
            break;  // thread/turn 层事件不归画面管,账也不记
    }
}

// 绘制半边:ToolDisplay/StreamBodyTracker 的原有调用,一个不少。画不画由
// 事先置好的动态绘制闸决定(SetPageVisible);被闸掉时这些方法内部走
// "只记账不上屏"的静默支路,条目/快照/状态照走。completed_tool 是
// HandleEvent 在收账前取好的工具配对信息(open_tools_ 已被收账摘掉)。
void TerminalTurnSink::DrawEvent(const runtime::ServerEvent& event,
                                 const std::optional<OpenTool>& completed_tool) {
    switch (event.kind) {
        case runtime::ServerEventKind::ItemDelta: {
            const std::string& text = event.text;
            if (event.item_kind == runtime::ItemKind::Text) {
                if (ingredients_.display->HasActiveThinking()) {
                    ingredients_.display->OnThinkingDone();
                    // 下一段正文前垫一空行,别粘在思考条目上
                    ingredients_.body_tracker->OnToolBlockDone();
                }
                ingredients_.body_tracker->OnDelta(text);
            } else if (event.item_kind == runtime::ItemKind::Thinking) {
                if (!ingredients_.display->HasActiveThinking()) {
                    ingredients_.body_tracker->OnBlockBreak();
                }
                ingredients_.display->OnThinkingDelta(text);
            }
            break;
        }
        case runtime::ServerEventKind::ItemStarted: {
            if (event.item_kind != runtime::ItemKind::Tool) {
                break;  // 正文/思考的懒起条不画(首枚 delta 才动笔,老规矩)
            }
            const std::string tool_use_id = event.payload.value("tool_use_id", std::string());
            const std::string name = event.payload.value("tool_name", std::string());
            const nlohmann::json input =
                event.payload.contains("input") ? event.payload["input"] : nlohmann::json::object();
            ingredients_.display->OnThinkingDone();  // 思考块若有,先收尾
            ingredients_.body_tracker->OnBlockBreak();
            ingredients_.display->OnToolStart(tool_use_id, name, input);
            break;
        }
        case runtime::ServerEventKind::ItemCompleted: {
            if (event.item_kind == runtime::ItemKind::Tool) {
                OnToolCompleted(event, completed_tool);
            } else if (event.item_kind == runtime::ItemKind::Thinking) {
                // wire 明确收口的思考块(content_block_stop 一类):立刻自
                // 折叠成一行。首枚 TextDelta/ToolStart/UsageUpdated 里的
                // OnThinkingDone 是幂等兜底,补没收到显式收口的那几家 wire。
                ingredients_.display->OnThinkingDone();
            }
            // 正文条目的收尾(ItemCompleted Succeeded)不画:老路上没有
            // 这只回调,画面由下一枚 item 的开画或收口重画接管。
            break;
        }
        case runtime::ServerEventKind::UsageUpdated: {
            // 请求结束:思考块若无后续文本/工具接上(只思考不回答的极端
            // 情况),在这里收尾。有后续时 OnThinkingDone 是幂等空操作。
            ingredients_.display->OnThinkingDone();
            break;
        }
        case runtime::ServerEventKind::ToolBatchFinished: {
            if (event.payload.value("interrupted", false)) {
                // 已露脸却尚未执行的确认条目按 Skipped 收口
                ingredients_.display->OnBatchSkipped();
            }
            break;
        }
        default:
            break;
    }
}

void TerminalTurnSink::OnToolCompleted(const runtime::ServerEvent& event,
                                        const std::optional<OpenTool>& completed_tool) {
    // Cancelled 的兜底补账与陌生终态在收账半边已处理(不画);这里只管
    // 画终态卡。
    if (event.outcome == runtime::Outcome::Cancelled || !completed_tool.has_value()) {
        return;
    }
    const OpenTool tool = *completed_tool;
    const std::string result_text = event.payload.value("result", std::string());
    const bool is_error = event.payload.value("is_error", false);
    if (event.payload.value("builtin", false)) {
        // 服务端内置工具:只画一张卡,不进视图账/录制(与老路
        // on_builtin_tool_done 闭包同画面)。终态参数回填条目标题(done 才
        // 补全 query 的兼容端就靠它)。
        const nlohmann::json final_input =
            event.payload.contains("input") ? event.payload["input"] : nlohmann::json::object();
        ingredients_.display->OnBuiltinToolDone(tool.tool_use_id, tool.name, final_input,
                                                tools::Tool::Result{result_text, is_error});
        ingredients_.body_tracker->OnToolBlockDone();
        return;
    }
    ingredients_.display->OnToolDone(tool.tool_use_id, tool.name, tools::Tool::Result{result_text, is_error});
    ingredients_.body_tracker->OnToolBlockDone();
}

}  // namespace lubancode::app
