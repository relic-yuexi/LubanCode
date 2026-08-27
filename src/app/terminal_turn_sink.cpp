// TerminalTurnSink 的实现(骨架拆解批二余款)。闭包原文自批二两轨并行时
// BuildCallbacks 的显示半边逐句搬来,水的来路换成事件流,画法一字不改。

#include "app/terminal_turn_sink.hpp"

#include <optional>
#include <utility>
#include <vector>

#include "api/types.hpp"
#include "cli/console_input.hpp"  // UpdateStatusLineContext
#include "cli/context_tracker.hpp"
#include "cli/format_utils.hpp"  // BuildCacheNote
#include "cli/live_transcript.hpp"
#include "cli/tool_display.hpp"
#include "runtime/turn_collector.hpp"
#include "runtime/turn_runtime.hpp"  // TurnUsageStats
#include "runtime/turn_view.hpp"     // TurnItemViewState
#include "skills/workflow_recorder.hpp"
#include "tools/tool.hpp"

namespace lubancode::app {

void TerminalTurnSink::Emit(const runtime::ServerEvent& event) {
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

// 既有直写渲染路(画面隔网前就是 Emit 的正文,水的来路换成"泵的消费
// 线程或就地路",画法一字不改)。
void TerminalTurnSink::RenderEvent(const runtime::ServerEvent& event) {
    switch (event.kind) {
        case runtime::ServerEventKind::ItemDelta: {
            const std::string& text = event.text;
            if (event.item_kind == runtime::ItemKind::Text) {
                if (ingredients_.view_collector != nullptr) {
                    ingredients_.view_collector->OnTextDelta(text, /*thinking=*/false);
                }
                if (ingredients_.display->HasActiveThinking()) {
                    ingredients_.display->OnThinkingDone();
                    // 下一段正文前垫一空行,别粘在思考条目上
                    ingredients_.body_tracker->OnToolBlockDone();
                }
                ingredients_.body_tracker->OnDelta(text);
            } else if (event.item_kind == runtime::ItemKind::Thinking) {
                if (ingredients_.view_collector != nullptr) {
                    ingredients_.view_collector->OnTextDelta(text, /*thinking=*/true);
                }
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
            const nlohmann::json input = event.payload.contains("input") ? event.payload["input"]
                                                                          : nlohmann::json::object();
            open_tools_[event.item_id] = OpenTool{tool_use_id, name};
            const bool builtin = event.payload.value("builtin", false);
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
            ingredients_.display->OnThinkingDone();  // 思考块若有,先收尾
            ingredients_.body_tracker->OnBlockBreak();
            ingredients_.display->OnToolStart(tool_use_id, name, input);
            break;
        }
        case runtime::ServerEventKind::ItemCompleted: {
            if (event.item_kind == runtime::ItemKind::Tool) {
                OnToolCompleted(event);
            }
            // 正文/思考条目的收尾(ItemCompleted Succeeded)不画:老路上
            // 没有这只回调,画面由下一枚 item 的开画或收口重画接管。
            break;
        }
        case runtime::ServerEventKind::UsageUpdated: {
            // 请求结束:思考块若无后续文本/工具接上(只思考不回答的极端
            // 情况),在这里收尾。有后续时 OnThinkingDone 是幂等空操作。
            ingredients_.display->OnThinkingDone();
            api::UsageReport report;
            report.usage.input_tokens = event.payload.value("input_tokens", std::int64_t{0});
            report.usage.output_tokens = event.payload.value("output_tokens", std::int64_t{0});
            report.usage.cache_read_tokens = event.payload.value("cache_read_tokens", std::int64_t{0});
            report.usage.cache_creation_tokens = event.payload.value("cache_creation_tokens", std::int64_t{0});
            report.usage.output_reasoning_tokens = event.payload.value("reasoning_tokens", std::int64_t{0});
            report.step_index = event.payload.value("step_index", 0);
            report.request_id = event.payload.value("request_id", std::string());
            report.model = event.payload.value("model", std::string());
            report.cache_epoch = event.payload.value("cache_epoch", 1);
            report.epoch_break_reason = event.payload.value("epoch_break_reason", std::string());
            report.prefix_append_only = event.payload.value("prefix_append_only", true);
            const bool reported = event.payload.value("reported", report.reported());
            if (ingredients_.view_collector != nullptr) {
                ingredients_.view_collector->OnUsage(report);
            }
            if (ingredients_.usage_stats != nullptr) {
                ingredients_.usage_stats->Add(report);
            }
            // ContextTracker 只认"最近一次请求"的真实用量,整个覆盖,不跟着
            // usage_stats 一起累加——语义区别见 cli/context_tracker.hpp 文件头。
            // ApplyUsage 兼管"provider 没回 usage"(四项全零)的语义:不清零、
            // 只把现有数字标成旧值;ESC/HTTP 错误路径压根走不到这里,不会
            // 把旧数伪装成本次新值。
            if (ingredients_.context_tracker != nullptr) {
                ingredients_.context_tracker->ApplyUsage(report.usage);
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
            if (ingredients_.view_collector != nullptr) {
                ingredients_.view_collector->OnModelStepStarted(event.payload.value("step_index", 0));
            }
            break;
        }
        case runtime::ServerEventKind::ToolBatchStarted: {
            if (ingredients_.view_collector != nullptr) {
                std::vector<std::string> ordered_ids;
                if (event.payload.contains("ordered_tool_use_ids") &&
                    event.payload["ordered_tool_use_ids"].is_array()) {
                    ordered_ids = event.payload["ordered_tool_use_ids"].get<std::vector<std::string>>();
                }
                ingredients_.view_collector->OnToolBatchStarted(
                    event.payload.value("step_index", 0), event.payload.value("batch_index", 0), ordered_ids);
            }
            break;
        }
        case runtime::ServerEventKind::ToolBatchFinished: {
            const bool interrupted = event.payload.value("interrupted", false);
            if (ingredients_.view_collector != nullptr) {
                ingredients_.view_collector->OnToolBatchFinished(event.payload.value("batch_index", 0), interrupted);
            }
            if (interrupted) {
                // 已露脸却尚未执行的确认条目按 Skipped 收口
                ingredients_.display->OnBatchSkipped();
            }
            break;
        }
        default:
            break;  // thread/turn 层事件不归画面管
    }
}

void TerminalTurnSink::OnToolCompleted(const runtime::ServerEvent& event) {
    // 适配器 Finish 的 Cancelled 兜底补账:只进事件流,不碰画面(老路上这
    // 类补账压根没有显示回调可走)。
    if (event.outcome == runtime::Outcome::Cancelled) {
        open_tools_.erase(event.item_id);
        return;
    }
    const auto it = open_tools_.find(event.item_id);
    if (it == open_tools_.end()) {
        return;  // 迟到/陌生终态:丢弃不误伤(与 ToolDisplay 同规矩)
    }
    const OpenTool tool = it->second;
    open_tools_.erase(it);
    const std::string result_text = event.payload.value("result", std::string());
    const bool is_error = event.payload.value("is_error", false);
    if (event.payload.value("builtin", false)) {
        // 服务端内置工具:只画一张卡,不进视图账/录制(与老路
        // on_builtin_tool_done 闭包同画面)。终态参数回填条目标题(done 才
        // 补全 query 的兼容端就靠它)。
        const nlohmann::json final_input = event.payload.contains("input") ? event.payload["input"]
                                                                            : nlohmann::json::object();
        ingredients_.display->OnBuiltinToolDone(tool.tool_use_id, tool.name, final_input,
                                                tools::Tool::Result{result_text, is_error});
        ingredients_.body_tracker->OnToolBlockDone();
        return;
    }
    if (ingredients_.recorder != nullptr && !ingredients_.trace_projection_installed) {
        ingredients_.recorder->RecordToolResult(tool.name, is_error, result_text);
    }
    if (ingredients_.view_collector != nullptr) {
        // ESC 后补的合成结果(is_error 且 cancel 已置)按 Interrupted 记,
        // 不冒充跑过又失败;真失败照 Failed。
        std::optional<runtime::TurnItemViewState> forced;
        if (is_error && ingredients_.cancel_flag != nullptr && ingredients_.cancel_flag->load()) {
            forced = runtime::TurnItemViewState::Interrupted;
        }
        ingredients_.view_collector->OnToolFinished(tool.tool_use_id, result_text, is_error, forced);
    }
    ingredients_.display->OnToolDone(tool.tool_use_id, tool.name, tools::Tool::Result{result_text, is_error});
    ingredients_.body_tracker->OnToolBlockDone();
}

}  // namespace lubancode::app
