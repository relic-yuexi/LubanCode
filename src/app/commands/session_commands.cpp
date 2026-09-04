// session_commands.hpp 的实现:上下文/压缩/会话存档命令的函数体。
#include "app/commands/session_commands.hpp"

#include "app/commands/command_registry.hpp"     // SlashDispatchContext(分派注册制)
#include "app/wirings/record_session_wiring.hpp"  // 录制接线器(会话终章)
#include "cli/record_command.hpp"                 // /record 的 presenter(cli 层)
#include "tools/agent_tool.hpp"                   // 归档/删除的后台忙查

#include <cstdlib>
#include <ctime>
#include <chrono>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "agent/compact.hpp"
#include "agent/artifact_store.hpp"
#include "agent/agent.hpp"  // Agent:ReplaceHistory/History(批四自立门户)
#include "agent/context.hpp"
#include "agent/context_events.hpp"
#include "config/config.hpp"
#include "cli/spinner.hpp"
#include "cli/transcript.hpp"
#include "platform/console.hpp"

#include <filesystem>
#include <optional>
#include <string>

#include "agent/loop.hpp"
#include <nlohmann/json.hpp>
#include "app/commands/settings_commands.hpp"
#include "app/runtime_profile.hpp"
#include "cli/console_input.hpp"
#include "cli/terminal_port.hpp"
#include "cli/format_utils.hpp"
#include "hooks/dispatcher.hpp"
#include "hooks/hash.hpp"  // Sha256Hex:P0-2 compact 状态指纹
#include "tools/path_utils.hpp"
#include "tools/tool_search.hpp"
#include "cli/context_tracker.hpp"
#include "cli/i18n.hpp"
#include "cli/queue_model.hpp"  // SessionSteeringQueue/QueueItemId:clear 的排队账申报
#include "cli/session_picker.hpp"
#include "cli/session_picker_panel.hpp"
#include "cli/theme.hpp"
#include "runtime/worktree.hpp"
#include "platform/paths.hpp"
#include "runtime/session_command_service.hpp"
// P0-3 轨迹:clear 八步换账 / resume 七步 / export 读 ReplayState 的账本口。
#include "runtime/trajectory_session.hpp"

namespace lubancode::app {


using lubancode::platform::CurrentDirUtf8;
using lubancode::cli::TermOut;
using lubancode::cli::TermErr;
using lubancode::cli::tr;
using lubancode::cli::trf;

// 字节账与 token 估算都收口到 agent/context.hpp 的统一口径(/context 的
// "字符数"分类明细按字节报,压缩报告按统一 token 口径报)。
std::size_t EstimateHistoryChars(const std::vector<lubancode::api::Message>& history) {
    return lubancode::agent::EstimateHistoryBytes(history);
}
std::size_t EstimateHistoryTokens(const std::vector<lubancode::api::Message>& history) {
    return lubancode::agent::EstimateHistoryTokens(history);
}

// 压力口径估算(P1-1 口径统一,Compact 四分区单阶段 0 起为手工/自动两路
// 共用的一只):把任意一份 history 过一遍 L1 无损结构压缩(临时 memo/stats,
// 不落盘、不定形),再按统一 token 口径估。触发线、/context 的对话历史、
// 压缩前后账都拿这一把尺——拿未压缩全量估,重复工具结果全被虚算,压完的
// "瘦"也是假瘦,反涨断言两边就不可比了。
std::size_t PressureEstimateTokens(lubancode::agent::Agent& loop,
                                   const std::vector<lubancode::api::Message>& history) {
    lubancode::agent::ResultViewMemo scratch_memo;
    lubancode::agent::StructuralCompressionStats scratch_stats;
    return lubancode::agent::EstimateHistoryTokens(lubancode::agent::CompressWorkingView(
        history, loop.context().structural_options(), scratch_stats, scratch_memo, /*store=*/nullptr));
}

// ---- /context 校准行(token 估算校准单)的三个小格式器 ------------------
// trf 不接浮点(见 i18n.hpp),比率/系数/偏差的字符串在这里自己拼好。

std::string FormatTokensPerByte(double tokens_per_byte) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << tokens_per_byte;
    return out.str();
}

std::string FormatCalibrationCoefficient(double coefficient) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << coefficient;
    return out.str();
}

// 偏差符号按"默认尺相对真实"读:+25% = 默认尺比真实高 25%(估算要往下
// 修);-20% = 默认尺比真实低 20%。正负的含义写进翻译模板,这里只出符号数。
std::string FormatEstimateDeviation(int deviation_percent) {
    if (deviation_percent == 0) {
        return "±0%";
    }
    std::ostringstream out;
    out << (deviation_percent > 0 ? "+" : "-") << std::abs(deviation_percent) << "%";
    return out.str();
}

// P0-2 轨迹:compact 前后的 effective history 指纹(compact.applied 的
// old/new state hash)。投影标记用——角色序 + 各块正文拼串再 hash,不是
// 密码学真值;同一份历史两次算必然同值(确定性重放的锚点)。
std::string HistoryStateHash(const std::vector<lubancode::api::Message>& history) {
    std::string buffer;
    for (const auto& message : history) {
        buffer += message.role == lubancode::api::Role::User
                      ? std::string("U")
                      : (message.role == lubancode::api::Role::Assistant ? std::string("A") : std::string("?"));
        buffer += std::to_string(message.content.size());
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<lubancode::api::TextBlock>(&block)) {
                buffer += text->text;
            } else if (const auto* thinking = std::get_if<lubancode::api::ThinkingBlock>(&block)) {
                buffer += thinking->text;
            } else if (const auto* call = std::get_if<lubancode::api::ToolUseBlock>(&block)) {
                buffer += call->id + call->name + call->input.dump();
            } else if (const auto* result = std::get_if<lubancode::api::ToolResultBlock>(&block)) {
                buffer += result->tool_use_id + result->content;
            }
        }
    }
    return lubancode::hooks::Sha256Hex(buffer);
}

// 反涨闸(0.26.84 治三,阶段 0 收口):压缩后的新史(压力口径)不比旧史小
// 时拒收换账——存档添在没瘦的热区头上,真机 70.8k 压成 73.7k 还标"校验通
// 过"。手工 /compact 与自动 TryRunCompact 共用这一只;拒收时旧 history 一
// 字不动。返回 true = 拒收(调用方就地收场)。
bool RejectGrownCompactHistory(const lubancode::cli::Theme& theme, std::size_t before_tokens,
                               std::size_t new_tokens) {
    if (new_tokens < before_tokens) {
        return false;
    }
    lubancode::cli::TermOut()
        << theme.error << trf("compact.grew_rejected", lubancode::cli::FormatTokenCount(before_tokens),
                              lubancode::cli::FormatTokenCount(new_tokens))
        << theme.reset << tr("compact.grew_rejected_tail") << "\n";
    return true;
}

// /context 命令:不带参数打分类占用分析(系统提示/工具定义/对话历史三类
// 字符数估 token + 条形图,拼装规则全在 FormatContextBreakdown,这里只管
// 收集与打印);带参数(256k/512k/1m/裸数字)临时改窗口大小,只本会话
// 生效,不改配置文件。sys_chars/tools_chars/history_chars 由调用方在会话
// 现场收集(裸敲才用得上,带参数分支忽略),缓存命中/窗口/实测占用都从
// context_tracker 拿。
void HandleContextCommand(const std::string& args, lubancode::cli::ContextTracker& context_tracker,
                           std::size_t sys_tokens, std::size_t tools_tokens, std::size_t history_tokens,
                           const lubancode::cli::Theme& theme, int cache_epoch,
                           const lubancode::agent::AgentRuntimeProfile* main_profile,
                           const lubancode::agent::ModelUsageLedger* usage_ledger,
                           const lubancode::agent::ContextArtifactStore* artifact_store,
                           const ContextLayersReport* layers,
                           const lubancode::agent::ModelRouteTable* roles_table,
                           int compact_partition_count,
                           const DeferredToolModeSummary* deferred_tool_summary,
                           const lubancode::agent::TokenCalibrationStatus* token_calibration) {
    if (args.empty()) {
        const auto lines = lubancode::cli::FormatContextBreakdown(
            sys_tokens, tools_tokens, history_tokens, context_tracker.last_cache_read_tokens(),
            context_tracker.window_tokens(), context_tracker.current_tokens(), theme,
            /*bar_width=*/16, context_tracker.last_cache_hit_percent());
        // 占用卡片(核心,第一组):系统/工具/历史条形图 + 已用/触发线/剩余。
        // FormatContextBreakdown 自带表头"上下文占用分析(窗口 {0})"。
        for (const auto& line : lines) {
            TermOut() << line << "\n";
        }
        // 接近上限提醒跟着占用卡片走。
        if (context_tracker.ShouldAutoCompact()) {
            TermOut() << tr("cmd.context.compact_hint") << "\n";
        }

        // 缓存卡片(第二组):前缀 epoch 与最近一次请求的命中率。
        // 命中跌下去时,用户看得出是主动换了哪根梁(epoch 断因在回合统计
        // 行/逐步流水账里),不再笼统赖服务端。没实测过就明说。
        if (context_tracker.last_total_input_tokens() > 0) {
            TermOut() << "\n── " << trf("cmd.context.group.cache") << " ──\n";
            const int hit_percent = context_tracker.last_cache_hit_percent();
            TermOut() << "  " << trf("cmd.context.epoch", cache_epoch,
                                     lubancode::cli::FormatTokenCount(context_tracker.last_cache_read_tokens()),
                                     lubancode::cli::FormatTokenCount(context_tracker.last_total_input_tokens()),
                                     hit_percent >= 0 ? std::to_string(hit_percent) : std::string("?"))
                      << "\n";
            // 会话累计总账:Σ命中 / Σ输入。跟单轮口径分开,并明确标注
            // "会话累计"——它回答"整个 session 发了多少输入、多少走了
            // 缓存读",不是"每轮都这么多"。
            if (context_tracker.session_input_total() > 0) {
                const int session_percent = context_tracker.session_cache_hit_percent();
                TermOut() << "  "
                          << trf("cmd.context.cache_session",
                                 lubancode::cli::FormatTokenCount(context_tracker.session_cache_read_total()),
                                 lubancode::cli::FormatTokenCount(context_tracker.session_input_total()),
                                 session_percent >= 0 ? std::to_string(session_percent) : std::string("?"))
                          << "\n";
            }
            // 逐请求命中率趋势(问题 5):一行是一次模型请求(不是用户轮),
            // 按外层用户轮次分组,上限/总数/未回报全说破——拼装在
            // BuildCacheRequestHistoryLines(纯函数,单测钉)。
            for (const std::string& line : lubancode::cli::BuildCacheRequestHistoryLines(context_tracker)) {
                TermOut() << line << "\n";
            }
        }

        // 口径说明(挂在占用卡片末尾,不再散落):状态栏与这里读的是同一只
        // tracker,都是"最近一次主请求的占用",不是会话累计花销,也不含
        // 独立子代理的 token。最近一次请求没带回 usage 时再补一行旧值提醒。
        {
            TermOut() << "\n" << tr("cmd.context.note.semantics") << "\n";
            if (context_tracker.usage_stale()) {
                TermOut() << tr("cmd.context.note.stale") << "\n";
            }
        }

        // token 估算校准行(token 估算校准单):上面三类估算数字的定盘星
        //——多少对样本、tokens/byte 比率、默认尺偏差几何。样本不足两对
        // 显示"未校准",估算全按默认口径,如实说破,不装准。
        if (token_calibration != nullptr) {
            if (token_calibration->calibrated) {
                TermOut() << "  "
                          << trf("cmd.context.calibration", token_calibration->sample_count,
                                 FormatTokensPerByte(token_calibration->tokens_per_byte),
                                 FormatEstimateDeviation(token_calibration->estimate_deviation_percent),
                                 FormatCalibrationCoefficient(token_calibration->coefficient))
                          << "\n";
            } else {
                TermOut() << "  " << tr("cmd.context.calibration_none") << "\n";
            }
        }

        // 结构与回收卡片(第三组):artifact 层、分层占用、回收字节、最近
        // compact——"原文还能去哪找、token 花在哪、何时会压"一张单子。
        if (artifact_store != nullptr || layers != nullptr || deferred_tool_summary != nullptr) {
            TermOut() << "\n── " << trf("cmd.context.group.structure") << " ──\n";
        }
        // deferred_tool_mode(动态工具 PromptCache 守恒单 P0 起;P1 补
        // proxy_reference 档,P3 补 native_reference 档):如实展示当前这一
        // 档。proxy 路提示"发现走 tool_search、调用走 tool_invoke,前缀不断";
        // native 路提示"发现走 provider 服务端搜索、defer_loading 保前缀";
        // legacy 路照旧提示断前缀(cache-hostile 兼容路)。
        if (deferred_tool_summary != nullptr) {
            TermOut() << "  "
                      << trf("cmd.context.deferred_tool_mode", deferred_tool_summary->mode_label,
                             deferred_tool_summary->pending, deferred_tool_summary->total)
                      << "\n";
            if (deferred_tool_summary->enabled) {
                TermOut() << tr(deferred_tool_summary->mode_label == "proxy_reference"
                                    ? "cmd.context.deferred_tool_mode.proxy_hint"
                                    : deferred_tool_summary->mode_label == "native_reference"
                                          ? "cmd.context.deferred_tool_mode.native_hint"
                                          : "cmd.context.deferred_tool_mode.legacy_hint")
                          << "\n";
            }
        }
        if (artifact_store != nullptr && artifact_store->active()) {
            const auto stats = artifact_store->StatsOf();
            if (stats.artifacts > 0) {
                TermOut() << "  " << trf("cmd.context.artifacts", stats.artifacts, stats.total_bytes) << "\n";
            } else {
                TermOut() << "  " << tr("cmd.context.artifacts_none") << "\n";
            }
        }
        if (layers != nullptr) {
            TermOut() << "  " << trf("cmd.context.layers", layers->inline_full_results,
                                     layers->artifact_previews)
                      << "\n";
            if (layers->reclaimable_bytes > 0) {
                TermOut() << "  " << trf("cmd.context.reclaimable", layers->reclaimable_bytes) << "\n";
            }
            if (!layers->last_compact_line.empty()) {
                TermOut() << "  " << trf("cmd.context.last_compact", layers->last_compact_line) << "\n";
            }
        }

        // 预算与角色账卡片(第四组):输出上限、预算总账、开销明细、压缩预算、
        // 分角色 usage 台账——模型分工各归各的账,一眼见底。
        if (main_profile != nullptr || layers != nullptr || usage_ledger != nullptr) {
            TermOut() << "\n── " << trf("cmd.context.group.budget") << " ──\n";
        }
        // 输出上限与来源(规格根因一):本轮每份请求给模型留的输出空间,
        // unset 也说破——"交服务端默认"比一枚看不见的 4096 诚实。
        if (main_profile != nullptr) {
            if (main_profile->max_output_tokens.has_value()) {
                TermOut() << "  " << trf("cmd.context.output_budget", *main_profile->max_output_tokens,
                                         app::OutputBudgetSourceText(main_profile->max_output_tokens_source, false))
                          << "\n";
            } else {
                TermOut() << "  " << tr("cmd.context.output_budget_unset") << "\n";
            }
        }
        // compact turn 策略(§八):compact_partition_count 配成几份、前几份
        // map、末份热区,一行说清——不调模型,纯配置展示。
        if (compact_partition_count > 0) {
            TermOut() << "  "
                      << trf("cmd.context.compact_turns", compact_partition_count,
                             compact_partition_count - 1)
                      << "\n";
        }
        if (layers != nullptr && layers->budget.has_value()) {
            const auto& plan = *layers->budget;
            TermOut() << "  " << trf("cmd.context.budget", plan.window,
                                     plan.compactable_history_budget.has_value()
                                         ? lubancode::cli::FormatTokenCount(*plan.compactable_history_budget)
                                         : std::string("?"),
                                     lubancode::cli::FormatTokenCount(plan.overhead_total()))
                      << "\n";
            TermOut() << "  " << trf("cmd.context.budget_detail", plan.stable_system + plan.model_instructions,
                                     plan.tool_schemas, plan.protected_hot_zone, plan.requested_output_reserve,
                                     plan.compact_prompt_overhead + plan.protocol_headroom,
                                     plan.tokenizer_error_margin)
                      << "\n";
            if (plan.compact_call_input_budget.has_value()) {
                TermOut() << "  " << trf("cmd.context.compact_budget",
                                         lubancode::cli::FormatTokenCount(*plan.compact_call_input_budget),
                                         lubancode::cli::FormatTokenCount(plan.summary_target_budget))
                          << "\n";
            }
            // 下一触发线:窗口的自动压缩线(kAutoCompactThresholdPercent)
            // 与当前占用的差,迟滞/分道在各自层里另有账。
            const std::size_t window = context_tracker.window_tokens();
            if (window > 0) {
                const std::size_t line = window * 80 / 100;  // 与 kAutoCompactThresholdPercent 同档
                const auto used = static_cast<std::int64_t>(context_tracker.current_tokens());
                TermOut() << "  " << trf("cmd.context.next_line", lubancode::cli::FormatTokenCount(line),
                                         used >= 0 ? lubancode::cli::FormatTokenCount(used) : std::string("0"),
                                         used >= static_cast<std::int64_t>(line)
                                                 ? tr("cmd.context.next_line_over")
                                                 : lubancode::cli::FormatTokenCount(
                                                       static_cast<std::int64_t>(line) - used))
                          << "\n";
            }
        }
        // 分角色 usage 台账(模型分工第一期,规格"路由看得见"):普通 turn
        // 归 normal,压缩/抽取/标题的后台采样归 cheap,回退单独留痕。
        // 三角色固定列全(问题 6):零调用角色也露脸,写明"本场未触发 +
        // 默认职责";回落关系与 /model roles 同一份 routes(source 同源)。
        if (usage_ledger != nullptr) {
            const auto role_lines = usage_ledger->ReportLines(roles_table);
            if (!role_lines.empty()) {
                TermOut() << "  " << tr("router.usage.header") << "\n";
                for (const std::string& line : role_lines) {
                    TermOut() << "    " << line << "\n";
                }
            }
            if (!usage_ledger->fallback_notes().empty()) {
                TermOut() << "  " << tr("router.usage.fallback_header") << "\n";
                for (const std::string& note : usage_ledger->fallback_notes()) {
                    TermOut() << "    " << note << "\n";
                }
            }
        }
        return;
    }
    const auto parsed = lubancode::config::ParseContextWindowTokens(args);
    if (!parsed.has_value()) {
        TermOut() << parsed.error() << "\n";
        return;
    }
    context_tracker.set_window_tokens(*parsed);
    TermOut() << trf("cmd.context.window_changed", *parsed) << "\n";
}

// Token 账本单 A1:compact 子请求(map/reduce)的旁路桥工厂。路由解完才
// 知道 provider,各路由点各自烤;trajectory 空(flag 关/单测)给空工厂,
// 一次采样一笔不落,行为与从前一致。
std::function<std::unique_ptr<lubancode::agent::LoopBoundaryRecorder>()> CompactBypassFactory(
    const CompactSessionInputs& in, const std::string& provider) {
    if (in.trajectory == nullptr) {
        return {};
    }
    lubancode::runtime::TrajectorySessionLedger* ledger = in.trajectory;
    const std::string wire = in.trajectory_wire;
    return [ledger, wire, provider]() -> std::unique_ptr<lubancode::agent::LoopBoundaryRecorder> {
        lubancode::runtime::TrajectoryTurnBridge::Identity identity{provider, wire, "host"};
        return ledger->NewBypassBridge(std::move(identity));
    };
}

// /compact 命令:窗口预算 + manifest 守恒校验 + 热区保留,一条路走到底。
// --dry-run 只算结构压缩的可回收量与钉住项,不发请求、不改历史。
CompactCommandResult HandleCompactCommand(const std::string& args, lubancode::agent::Agent& loop,
                                          lubancode::api::Backend& raw_backend,
                                          const lubancode::agent::ModelRoute& compact_route,
                                          const lubancode::cli::Theme& theme, bool spinner_enabled,
                                          const lubancode::agent::CompactOptions& options, int& compact_epoch,
                                          lubancode::agent::BackgroundCallAccounting* accounting) {
    const std::vector<lubancode::api::Message>& history = loop.History();
    if (history.empty()) {
        TermOut() << tr("cmd.compact.empty") << "\n";
        return {};
    }

    // --dry-run [--focus 文本] / --dry-run:只算不动手。
    bool dry_run = false;
    std::string focus = args;
    if (args == "--dry-run") {
        dry_run = true;
        focus.clear();
    } else if (args.rfind("--dry-run ", 0) == 0) {
        dry_run = true;
        focus = args.substr(std::string("--dry-run ").size());
    }
    if (dry_run) {
        lubancode::agent::StructuralCompressionOptions struct_options;
        lubancode::agent::StructuralCompressionStats struct_stats;
        (void)lubancode::agent::CompressWorkingView(history, struct_options, struct_stats);  // 只要账
        const std::size_t hot_from = lubancode::agent::HotZoneStartIndex(history);
        std::vector<lubancode::api::Message> hot(history.begin() + static_cast<std::ptrdiff_t>(hot_from),
                                                 history.end());
        TermOut() << tr("cmd.compact.dryrun.header") << "\n";
        TermOut() << trf("cmd.compact.dryrun.reclaim", struct_stats.reclaimable_bytes(),
                         struct_stats.duplicate_groups, struct_stats.superseded_observations,
                         struct_stats.offloaded_results)
                  << "\n";
        TermOut() << trf("cmd.compact.dryrun.pinned", lubancode::agent::EstimateHistoryTokens(hot),
                         options.required_open_items.size())
                  << "\n";
        // ---- turn 分区计划(Compact 四分区单·阶段 1):纯计算,不调模型 ----
        // 不发请求也能说清"若现在压缩,四份各有哪些 turn、各占多少 token、
        // 哪些 ToolResult 已外置"。计量用会话现场的结构压缩口径(与触发线、
        // 压缩前后账同一把尺),预算诊断用压缩路由自己的窗口。
        lubancode::agent::TurnPartitionBudgets plan_budgets;
        plan_budgets.structural = loop.context().structural_options();
        plan_budgets.compact_model = options.budget;
        const auto plan = lubancode::agent::BuildTurnPartitionPlan(history, options.partition_count, plan_budgets);
        TermOut() << trf("cmd.compact.dryrun.turnplan", plan.turns.size(), plan.partitions.size(),
                         plan.map_calls)
                  << "\n";
        if (plan.has_prior_archive) {
            TermOut() << trf("cmd.compact.dryrun.prior",
                             lubancode::cli::FormatTokenCount(plan.prior_archive_tokens))
                      << "\n";
        }
        for (const auto& partition : plan.partitions) {
            const std::size_t first_turn =
                partition.first_turn < plan.turns.size() ? plan.turns[partition.first_turn].number : 0;
            const std::size_t last_turn =
                partition.last_turn > 0 && partition.last_turn - 1 < plan.turns.size()
                    ? plan.turns[partition.last_turn - 1].number
                    : first_turn;
            TermOut() << trf("cmd.compact.dryrun.partition", partition.label,
                             first_turn, last_turn, lubancode::cli::FormatTokenCount(partition.working_tokens),
                             partition.externalized_results, partition.is_hot ? tr("cmd.compact.dryrun.hot_tag") : std::string())
                      << (partition.over_map_budget ? tr("cmd.compact.dryrun.over_tag") : std::string()) << "\n";
        }
        TermOut() << trf("cmd.compact.dryrun.offload", lubancode::cli::FormatTokenCount(plan.total_raw_tokens),
                         lubancode::cli::FormatTokenCount(plan.total_working_tokens), plan.externalized_results)
                  << "\n";
        if (!plan.compact_input_budget.has_value()) {
            TermOut() << theme.stats << tr("cmd.compact.window_unknown") << theme.reset << "\n";
        } else if (plan.any_turn_over_map_budget) {
            TermOut() << theme.error << trf("cmd.compact.dryrun.over_turn",
                                            lubancode::cli::FormatTokenCount(*plan.compact_input_budget))
                      << theme.reset << "\n";
        } else if (plan.any_partition_over_map_budget) {
            TermOut() << theme.error << trf("cmd.compact.dryrun.over_partition",
                                            lubancode::cli::FormatTokenCount(*plan.compact_input_budget))
                      << theme.reset << "\n";
        }
        if (plan.has_incomplete_tool_exchange) {
            TermOut() << theme.stats << tr("cmd.compact.dryrun.orphan") << theme.reset << "\n";
        }
        if (!plan.WorthCompacting()) {
            TermOut() << theme.error << tr("cmd.compact.dryrun.no_gain") << theme.reset << "\n";
        }
        return {};
    }

    // 压缩前后账走压力口径(P1-1 口径统一):与触发线、自动压缩、反涨闸同
    // 一把尺——拿未压缩全量估,重复工具结果全被虚算,压完的"瘦"是假瘦,
    // 反涨断言两边不可比。反涨闸本身见 RejectGrownCompactHistory。
    const std::size_t before_tokens = PressureEstimateTokens(loop, history);

    lubancode::agent::CompactOptions run_options = options;
    run_options.focus = focus;

    lubancode::cli::Spinner spinner(theme, spinner_enabled);
    // 双账压缩(四分区单·阶段 2-4):前分区各一次 map(严格 JSON
    // TurnGroupSummary),末份热区保原文;prior archive + 全部小结 + 热区
    // 原文一道 final reduce 出 UserContract + WorkState;校验过后才换账。
    // 结构压缩口径带会话现场那份(与触发线、反涨闸同一把尺)。
    const auto result = lubancode::agent::CompactTurnPartitioned(
        raw_backend, compact_route.model, history, run_options, loop.context().structural_options(),
        compact_route.effort, accounting);
    spinner.Stop();

    if (!result.has_value()) {
        TermOut() << theme.error << trf("cmd.compact.failed", result.error().message) << theme.reset << "\n";
        return {};
    }

    const auto& new_history = result->new_history;
    const std::size_t after_tokens = PressureEstimateTokens(loop, new_history);
    // 反涨闸(阶段 0):手工 /compact 与自动 TryRunCompact 共用同一只——新史
    // (压力口径)不比旧史小便拒收换账,旧 history 一字不动、事件不落盘。
    if (RejectGrownCompactHistory(theme, before_tokens, after_tokens)) {
        return {};
    }
    loop.ReplaceHistory(new_history);
    // (P0-6:compact_v2 事件行随 SessionStore 删除;压缩的持久账是
    // trajectory 的 compact.applied,由调用方在换账后落。)
    compact_epoch += 1;

    TermOut() << trf("cmd.compact.result", before_tokens, after_tokens) << "\n";
    if (result->metrics.hierarchical) {
        TermOut() << trf("cmd.compact.hierarchical", result->metrics.chunks, result->metrics.reduce_passes)
                  << "\n";
    }
    if (!options.budget.window_tokens.has_value()) {
        TermOut() << theme.stats << tr("cmd.compact.window_unknown") << theme.reset << "\n";
    }
    TermOut() << trf("cmd.compact.manifest", result->manifest.constraints.size(),
                     result->manifest.open_items.size())
              << "\n";
    return CompactCommandResult{/*applied=*/true, before_tokens, after_tokens,
                                result->manifest.constraints.size(), result->manifest.open_items.size()};
}
// /sessions(P0-2:数据源换 workspace 可重建索引)。默认只列当前
// workspace;`all` 扫 ~/.lubancode/workspaces/*/sessions/(经索引,不为
// 每次列表重放 Journal);`archived` 是归档只读入口。
void PrintSessionsCommand(const lubancode::runtime::TrajectorySessionLedger* ledger,
                          const std::string& args) {
    if (ledger == nullptr) {
        TermOut() << tr("session.no_home") << "\n";
        return;
    }
    if (args == "archived") {
        lubancode::trajectory::SessionIndexQuery query;
        query.archived_only = true;
        query.limit = 0;
        const auto page = ledger->ListWorkspaceSessions(query);
        if (page.entries.empty()) {
            TermOut() << tr("cmd.sessions.archived_none") << "\n";
            return;
        }
        TermOut() << trf("cmd.sessions.archived_header", page.total) << "\n";
        for (const auto& entry : page.entries) {
            const std::string& label = !entry.title.empty() ? entry.title : entry.first_user_text;
            TermOut() << "  " << entry.session_id << "\n"
                      << trf("cmd.sessions.entry",
                              lubancode::trajectory::FormatMillisAsLocalTimestamp(entry.updated_at_ms),
                              entry.message_count,
                              label.empty() ? tr("cmd.sessions.no_text")
                                             : lubancode::tools::TruncateUtf8Chars(label, 40))
                      << "\n";
        }
        TermOut() << tr("cmd.sessions.archived_hint") << "\n";
        return;
    }
    const bool all = args == "all";
    if (!args.empty() && !all) {
        TermOut() << tr("cmd.sessions.usage") << "\n";
        return;
    }
    lubancode::trajectory::SessionIndexQuery query;
    query.all_workspaces = all;
    query.limit = 20;
    const auto page = ledger->ListWorkspaceSessions(query);
    if (page.entries.empty()) {
        if (all) {
            TermOut() << trf("cmd.sessions.none_all", "workspaces") << "\n";
        } else {
            TermOut() << tr("cmd.sessions.none_here") << "\n";
        }
        return;
    }
    TermOut() << trf("cmd.sessions.header", page.entries.size(),
                      all ? tr("cmd.sessions.scope_all") : tr("cmd.sessions.scope_here"))
              << "\n";
    for (std::size_t i = 0; i < page.entries.size(); ++i) {
        const auto& entry = page.entries[i];
        // 标题优先,没设过标题回退首句摘要。
        const std::string& label = !entry.title.empty() ? entry.title : entry.first_user_text;
        TermOut() << "  " << (i + 1) << ") " << entry.session_id << "\n"
                  << trf("cmd.sessions.entry",
                          lubancode::trajectory::FormatMillisAsLocalTimestamp(entry.created_at_ms),
                          entry.message_count,
                          label.empty() ? tr("cmd.sessions.no_text")
                                         : lubancode::tools::TruncateUtf8Chars(label, 40))
                  << "\n";
        // 单发轨迹断档单:one_shot 场照列照标——审计可读,单发语义不续
        //(编号仍可作 /resume 的指认,七步里明拒并说原因)。
        if (entry.run_kind == "one_shot") {
            TermOut() << tr("cmd.sessions.oneshot_line") << "\n";
        }
        if (all) {
            TermOut() << trf("cmd.sessions.dir_line",
                              entry.cwd.empty() ? tr("cmd.sessions.dir_unknown")
                                                 : lubancode::tools::AbbreviateUtf8Middle(entry.cwd, 48))
                       << "\n";
        }
    }
}

// ---------------------------------------------------------------------------
// /resume 裸敲的全屏选择器(SessionPicker):打开时扫一回台账,键盘搜索
// 只筛内存;Filter(Cwd/All)与 Sort(Updated/Created)切换时重查一次
// catalog(指纹缓存,没动的场不重读)。Enter 回 id 交 ResumeSession,
// Esc 原路返回不动盘。
// ---------------------------------------------------------------------------

namespace {

// Ctrl+T 转录浮层取头尾各多少行(大文件按需读,掐中间)。
constexpr std::size_t kTranscriptHalfRows = 60;

// 存档时间串("yyyy-mm-dd HH:MM:SS")-> epoch 秒。本地时间口径(与落档
// 同一时区);认不出的串折 0(相对时间按"很久以前"算,不炸)。
long long SessionTsToEpoch(const std::string& ts) {
    if (ts.size() < 19) {
        return 0;
    }
    std::tm tm_buf{};
    tm_buf.tm_year = std::atoi(ts.substr(0, 4).c_str()) - 1900;
    tm_buf.tm_mon = std::atoi(ts.substr(5, 2).c_str()) - 1;
    tm_buf.tm_mday = std::atoi(ts.substr(8, 2).c_str());
    tm_buf.tm_hour = std::atoi(ts.substr(11, 2).c_str());
    tm_buf.tm_min = std::atoi(ts.substr(14, 2).c_str());
    tm_buf.tm_sec = std::atoi(ts.substr(17, 2).c_str());
    tm_buf.tm_isdst = -1;
#ifdef _WIN32
    return _mkgmtime(&tm_buf);
#else
    return timegm(&tm_buf);
#endif
}


// Ctrl+T 转录浮层的取数:整份读进来,取头尾各 max_half 行(大文件按需
// 读——只在浮层开了且选中 id 变了时被调一回,不是每键读盘)。事件行
// (compact/title/cwd/queue)不进转录,只摆消息行;用户/助手各按首块
// 文本拼一行(工具调用摆 "[工具] 名字"),像一份压缩的流水。
// Ctrl+T 转录浮层的取数(P0-2:从 Journal 折——ledger.MakeTranscriptExcerpt
// 单遍读 main.jsonl 的 input/model.output 首行,头尾各 max_half 行)。
std::vector<std::string> MakeTranscriptExcerpt(const lubancode::runtime::TrajectorySessionLedger* ledger,
                                               const std::string& id, std::size_t max_half) {
    if (ledger == nullptr) {
        return {};
    }
    return ledger->MakeTranscriptExcerpt(id, max_half);
}

}  // namespace

// /resume 裸敲的全屏选择器(SessionPicker;P0-2 数据源换 workspace 索引):
// 默认范围是当前 workspace(同仓子目录/linked worktree 一把钥匙),全部
// 范围扫所有 workspace。Enter 回 id 交 resume 七步,Esc 原路返回不动盘。
std::optional<std::string> PromptResumeTarget(const lubancode::runtime::TrajectorySessionLedger* ledger,
                                              const lubancode::cli::Theme& theme) {
    if (ledger == nullptr) {
        TermOut() << tr("session.no_home") << "\n";
        return std::nullopt;
    }
    if (!lubancode::platform::StdinIsInteractive() || !lubancode::platform::ProbeStdoutConsole().is_console) {
        TermOut() << tr("cmd.resume.usage") << "\n";
        return std::nullopt;
    }

    // 打开时查一回(本 workspace 与全部都空才说"没什么可恢复")。单发轨迹
    // 断档单:选择器排除 one_shot 场——单发语义不续,面板里不摆续不了的场。
    lubancode::trajectory::SessionIndexQuery query;
    query.limit = 0;  // 面板自己管视口,数据一次给全
    query.exclude_one_shot = true;
    if (ledger->ListWorkspaceSessions(query).total == 0) {
        lubancode::trajectory::SessionIndexQuery all_query;
        all_query.all_workspaces = true;
        all_query.limit = 0;
        all_query.exclude_one_shot = true;
        if (ledger->ListWorkspaceSessions(all_query).total == 0) {
            TermOut() << tr("cmd.resume.none") << "\n";
            return std::nullopt;
        }
    }

    // 面板循环:换 Filter/Sort 重查索引(指纹没动的场不重读)再进面板,
    // 选中项按 id 留住。
    lubancode::cli::SessionPickerScope scope = lubancode::cli::SessionPickerScope::Cwd;
    lubancode::cli::SessionPickerSort sort = lubancode::cli::SessionPickerSort::Updated;
    std::string keep_id;  // 换筛选前的选中项,重进面板时守住它
    for (;;) {
        lubancode::trajectory::SessionIndexQuery index_query;
        index_query.all_workspaces = scope == lubancode::cli::SessionPickerScope::All;
        index_query.sort_by_created = sort == lubancode::cli::SessionPickerSort::Created;
        index_query.limit = 0;
        index_query.exclude_one_shot = true;  // 单发场不进续聊面板(不续,审计可读)
        const auto page = ledger->ListWorkspaceSessions(index_query);
        lubancode::cli::SessionPickerFeed feed;
        feed.entries.reserve(page.entries.size());
        const std::string now_key = lubancode::tools::NowTimestamp();
        const long long now = SessionTsToEpoch(now_key);
        for (const auto& entry : page.entries) {
            lubancode::cli::SessionPickerEntry row;
            row.id = entry.session_id;
            row.title = entry.title;
            row.preview = entry.first_user_text;
            row.cwd = entry.cwd;
            row.updated_ago = lubancode::cli::FormatSessionAgo(
                now, SessionTsToEpoch(lubancode::trajectory::FormatMillisAsLocalTimestamp(entry.updated_at_ms)));
            row.created_ago = lubancode::cli::FormatSessionAgo(
                now, SessionTsToEpoch(lubancode::trajectory::FormatMillisAsLocalTimestamp(entry.created_at_ms)));
            row.damaged = entry.damaged;
            row.created_at = lubancode::trajectory::FormatMillisAsLocalTimestamp(entry.created_at_ms);
            row.updated_at = lubancode::trajectory::FormatMillisAsLocalTimestamp(entry.updated_at_ms);
            row.model = entry.model;
            row.message_count = static_cast<std::size_t>(entry.message_count);
            feed.entries.push_back(std::move(row));
        }
        feed.total = page.entries.size();
        feed.now_epoch = now;
        // Ctrl+T 转录浮层:按需读盘(选中 id 变了面板才回调这一回)。
        lubancode::cli::SessionTranscriptProvider transcript = [ledger](const std::string& id) {
            return MakeTranscriptExcerpt(ledger, id, kTranscriptHalfRows);
        };
        const auto result =
            lubancode::cli::RunSessionPickerPanel(feed, theme, scope, sort, keep_id, 12, transcript);
        if (!result.picked_id.has_value()) {
            if (result.scope != scope || result.sort != sort) {
                // 只是换了形状:带着新形状重查再进面板,不算取消;
                // 选中项按 id 留住。
                scope = result.scope;
                sort = result.sort;
                keep_id = result.selected_id;
                continue;
            }
            TermOut() << theme.stats << tr("cmd.resume.cancelled") << theme.reset << "\n";
            return std::nullopt;
        }
        return *result.picked_id;
    }
}

// /resume <编号或id> 和 --continue 共用的执行逻辑。target 是编号(按
// ListSessions 的倒序编号)、会话 id、或空串(--continue:最近一场)。
// 编号和"最近一场"都只在**本目录**(meta.cwd == 当前 cwd)的场子里数;
// 直接给 id 的仍然全局能找(拼路径兜底),跨目录恢复留了这条明路。
// 成功:回放事件 + 成对修补 + ReplaceHistory + 接管文件继续追加,返回 true;
// session_title 同步成存档里最后一条 title 事件(没有就清空)。
// quiet_if_none:--continue 本目录找不到任何存档时不报错、安静开新会话。
// worktree_session(0.27.x,可空):会话档 meta.cwd(含 cwd 事件回放)若指向
// 一间还在的 worktree 房,验明正身后把会话搬回去;房没了回落启动目录并
// 说一声;马甲房(.git 指回主仓那类)拒进并报原因。非空时成功恢复后由
// 调用方做一次目录善后同步(重拼系统提示那条路)。
// (P0-6:ResumeSession——旧 SessionStore 的 /resume 执行体——已删;
// 现行路是 trajectory 的 resume-as-new 七步,在 HandleSlashResume 里。)

// /export [路径]:当前会话导出 Markdown,默认写 sessions/<id>.md。
// 有存档文件就从文件读**全量流水**导出(压缩不丢内容,发生点插一行标注);
// 没有存档文件(没落过盘)退回导内存里这份历史。/title 设过的标题当大标题。
// (P0-6:HandleExportCommand——旧 SessionStore 的 /export 执行体——已删;
// 现行路折叠本场 ReplayState 投影导出,在 HandleSlashExport 里。)

// ---------------------------------------------------------------------------
// 会话命令 handler:原样搬自会话主循环的 slash case,行为一字未改。
// ---------------------------------------------------------------------------

// (P0-6:HandleClearCommand/HandleTitleCommand/HandleResumeCommand 的旧
// 存档实现已删——现行路分别在 HandleSlashClear/HandleSlashTitle/
// HandleSlashResume 的 trajectory 分支里。)

// ---------------------------------------------------------------------------
// 归档与永久删除(第四、五步):引用解析、确认屏、顶层命令与会话内命令。
// 搬与删全经 trajectory 的 SessionAdminOutcome 自由函数,这里不直接碰 filesystem。
// ---------------------------------------------------------------------------

namespace {

// 索引摘要 -> 引用解析候选账(P0-2:候选来自 workspace 索引;file_path
// 位置放 session 目录,消歧只认 id/标题)。默认只列活动会话;
// include_archived 连归档场一起列(unarchive 的目标恰是归档场)。
std::vector<lubancode::tools::SessionRefCandidate> MakeCandidates(
    const std::filesystem::path& workspaces_root, bool include_archived) {
    std::vector<lubancode::tools::SessionRefCandidate> out;
    lubancode::trajectory::SessionIndexQuery query;
    query.all_workspaces = true;
    query.include_archived = include_archived;
    query.limit = 0;
    const auto page = lubancode::trajectory::QueryWorkspaceSessions(workspaces_root, query);
    out.reserve(page.entries.size());
    for (const auto& entry : page.entries) {
        out.push_back(
            lubancode::tools::SessionRefCandidate{entry.session_id, entry.session_dir, entry.title});
    }
    return out;
}

// 短 id(列重名用):日期时刻段。
std::string ShortSessionId(const std::string& id) {
    if (id.size() >= 15 && id[8] == '-') {
        return id.substr(0, 15);
    }
    return id;
}

// 读一行(确认屏输入)。stdin_line 回调优先(测试可钉);null 走 std::cin;
// EOF 给空串 = 取消。
std::string ReadConfirmLine(const std::function<std::string()>& stdin_line) {
    if (stdin_line != nullptr) {
        return stdin_line();
    }
    std::string line;
    if (!std::getline(std::cin, line)) {
        return std::string();  // EOF:按取消
    }
    return line;
}

// 确认答案归一:y/yes(大小写不敏感、剥尾空白)才算确认;空答/别的都取消。
bool ConfirmAnswer(const std::string& answer) {
    std::string lowered;
    for (const char c : answer) {
        lowered += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    while (!lowered.empty() && (lowered.back() == ' ' || lowered.back() == '\r' || lowered.back() == '\n')) {
        lowered.pop_back();
    }
    return lowered == "y" || lowered == "yes";
}

}  // namespace

// 引用解析(P0-2:候选来自 workspace 索引,跨 workspace 也解得出)。
bool ResolveSessionReference(const std::filesystem::path& workspaces_root, const std::string& ref,
                             const std::function<std::string()>& stdin_line, bool include_archived,
                             std::string& out_id, std::string& out_title, std::string& out_message,
                             bool& ambiguous) {
    (void)stdin_line;
    ambiguous = false;
    out_id.clear();
    out_title.clear();
    out_message.clear();
    const auto candidates = MakeCandidates(workspaces_root, include_archived);
    const auto hits = lubancode::tools::ResolveSessionRef(candidates, ref, ambiguous);
    if (!hits.has_value() || hits->empty()) {
        out_message = trf("cmd.session.ref_not_found", ref);
        return false;
    }
    if (ambiguous) {
        // 重名/多义:列短 id,叫用户点明(绝不猜一场)。
        std::string ids;
        for (const auto& hit : *hits) {
            if (!ids.empty()) {
                ids += ", ";
            }
            ids += ShortSessionId(hit.id);
        }
        out_message = trf("cmd.session.ref_ambiguous", ref, ids);
        return false;
    }
    out_id = hits->front().id;
    out_title = hits->front().title;
    return true;
}

// 顶层 `lubancode archive|unarchive|delete <SESSION>`(P0-2:不进会话也
// 能办——workspaces 根由调用方递,搬删走 trajectory 管理自由函数,lifecycle
// intent/result + 状态图 + tombstone 与 app-server 同一条路)。
int HandleSessionManagementCommand(const std::filesystem::path& workspaces_root, int kind,
                                   const std::string& ref, bool force, const lubancode::cli::Theme& theme,
                                   const std::function<std::string()>& stdin_line) {
    (void)theme;
    if (workspaces_root.empty()) {
        TermOut() << tr("session.no_home") << "\n";
        return 1;
    }
    const bool is_delete = kind == 2;
    const bool is_archive = kind == 0;
    if (ref.empty()) {
        TermOut() << tr(is_delete ? "cmd.session.delete.usage" : "cmd.session.archive.usage") << "\n";
        return 1;
    }
    std::string id;
    std::string title;
    std::string message;
    bool ambiguous = false;
    // unarchive/delete 的目标可能在归档里(单子:删除只碰验明的那一份):
    // 候选连归档一起列。archive 只在活动会话里解(已归档的重复归档无意
    // 义,幂等成功)。
    if (!ResolveSessionReference(workspaces_root, ref, stdin_line, !is_archive, id, title, message,
                                 ambiguous)) {
        TermOut() << message << "\n";
        return 1;
    }

    // 目标所在 workspace 与摘要细节(确认屏要写标题/完整 id/cwd)。
    std::string owner_workspace_key;
    std::string cwd;
    std::string first_text;
    {
        lubancode::trajectory::SessionIndexQuery query;
        query.all_workspaces = true;
        query.include_archived = true;
        query.limit = 0;
        for (const auto& entry :
             lubancode::trajectory::QueryWorkspaceSessions(workspaces_root, query).entries) {
            if (entry.session_id == id) {
                owner_workspace_key = entry.workspace_key;
                cwd = entry.cwd;
                first_text = entry.first_user_text;
                if (title.empty()) {
                    title = entry.title;
                }
                break;
            }
        }
    }
    const std::int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
    const std::filesystem::path workspace_dir =
        workspaces_root / lubancode::tools::Utf8ToPath(owner_workspace_key);

    if (is_delete) {
        const std::string label = !title.empty()
                                      ? title
                                      : (!first_text.empty() ? first_text : tr("cmd.sessions.no_text"));
        // 确认屏:标题/完整 id/cwd/"永久删除"。缺省取消;EOF、空答皆取消。
        // --force 只给脚本:跳过确认(帮助里写明不可恢复)。
        bool confirmed = force;
        if (!confirmed) {
            TermOut() << tr("cmd.session.delete.confirm_header") << "\n"
                      << trf("cmd.session.delete.confirm_title",
                             lubancode::tools::TruncateUtf8Chars(label, 60))
                      << "\n"
                      << trf("cmd.session.delete.confirm_id", id) << "\n"
                      << trf("cmd.session.delete.confirm_cwd",
                             cwd.empty() ? tr("cmd.sessions.dir_unknown")
                                         : lubancode::tools::AbbreviateUtf8Middle(cwd, 60))
                      << "\n"
                      << tr("cmd.session.delete.confirm_prompt");
            TermOut().flush();
            confirmed = ConfirmAnswer(ReadConfirmLine(stdin_line));
            if (!confirmed) {
                TermOut() << tr("cmd.session.delete.cancelled") << "\n";
                return 1;
            }
        }
        const auto outcome = lubancode::trajectory::DeleteSessionDir(workspace_dir, id, "user_delete",
                                                                     now_ms);
        if (!outcome.ok()) {
            TermOut() << theme.error << trf("cmd.session.delete.failed", id) << ": "
                      << outcome.error_code << ": " << outcome.message << theme.reset << "\n";
            return 1;
        }
        TermOut() << trf("cmd.session.delete.done", id) << "\n";
        return 0;
    }

    const auto outcome = is_archive
                             ? lubancode::trajectory::ArchiveSessionDir(workspace_dir, id, now_ms)
                             : lubancode::trajectory::UnarchiveSessionDir(workspace_dir, id, now_ms);
    if (!outcome.ok()) {
        TermOut() << theme.error
                  << trf(is_archive ? "cmd.session.archive.failed" : "cmd.session.unarchive.failed", id)
                  << ": " << outcome.error_code << ": " << outcome.message << theme.reset << "\n";
        return 1;
    }
    TermOut() << trf(is_archive ? "cmd.session.archive.done" : "cmd.session.unarchive.done", id) << "\n";
    return 0;
}

// /archive(P0-2:当前场先封口再归档——running 场不进状态图的归档边;
// recorder 收柄由 CloseSession 办,没有旧 SessionStore 的句柄闸)。
bool ArchiveCurrentSession(lubancode::runtime::TrajectorySessionLedger* ledger,
                           const lubancode::cli::Theme& theme) {
    (void)theme;
    if (ledger == nullptr) {
        TermOut() << tr("session.no_home") << "\n";
        return false;
    }
    const std::string id = ledger->session_id();
    if (id.empty()) {
        TermOut() << tr("cmd.archive.not_active") << "\n";
        return false;
    }
    (void)ledger->CloseSession("archive");
    const std::string error = ledger->ArchiveSessionInWorkspace(id);
    if (!error.empty()) {
        TermOut() << theme.error << trf("cmd.session.archive.failed", id) << ": " << error
                  << theme.reset << "\n";
        return false;
    }
    TermOut() << trf("cmd.session.archive.done", id) << "\n";
    return true;
}

// /delete(P0-2:确认屏后封口 + tombstone + 删目录;删的是当前场,退出
// 是调用方的事)。
bool DeleteCurrentSession(lubancode::runtime::TrajectorySessionLedger* ledger,
                          const lubancode::cli::Theme& theme, const std::string& title,
                          const std::string& cwd, const std::function<std::string()>& stdin_line) {
    (void)theme;
    if (ledger == nullptr) {
        TermOut() << tr("session.no_home") << "\n";
        return false;
    }
    const std::string id = ledger->session_id();
    if (id.empty()) {
        TermOut() << tr("cmd.delete.not_active") << "\n";
        return false;
    }
    // 确认屏:标题/完整 id/cwd/"永久删除"。缺省取消;EOF、空答皆取消。
    const std::string label =
        !title.empty() ? title : (!cwd.empty() ? cwd : tr("cmd.sessions.no_text"));
    TermOut() << tr("cmd.session.delete.confirm_header") << "\n"
              << trf("cmd.session.delete.confirm_title", lubancode::tools::TruncateUtf8Chars(label, 60))
              << "\n"
              << trf("cmd.session.delete.confirm_id", id) << "\n"
              << trf("cmd.session.delete.confirm_cwd",
                     cwd.empty() ? tr("cmd.sessions.dir_unknown")
                                 : lubancode::tools::AbbreviateUtf8Middle(cwd, 60))
              << "\n"
              << tr("cmd.session.delete.confirm_prompt");
    TermOut().flush();
    if (!ConfirmAnswer(ReadConfirmLine(stdin_line))) {
        TermOut() << tr("cmd.session.delete.cancelled") << "\n";
        return false;
    }
    (void)ledger->CloseSession("delete");
    const std::string error = ledger->DeleteSessionInWorkspace(id, "user_delete");
    if (!error.empty()) {
        TermOut() << theme.error << trf("cmd.session.delete.failed", id) << ": " << error
                  << theme.reset << "\n";
        return false;
    }
    TermOut() << trf("cmd.session.delete.done", id) << "\n";
    return true;
}

// ---- /context 会话现场收集 与 /compact 会话接线(终端接线收尾单自大类
// 搬出;输出走 TerminalPort)。函数体原文随行,行为一字不差。 ------------

void RunContextCommand(const std::string& args, const ContextEstimateInputs& in,
                       const lubancode::cli::Theme& theme) {
    lubancode::agent::Agent& loop = *in.agent;
    lubancode::cli::ContextTracker& context_tracker = *in.context_tracker;
    // 裸敲才收集三类 token 估算(带参数走切窗口分支,收了也白收)。
    // 口径对齐"实际发出的请求",token 全按统一口径
    // (agent/context.hpp:ASCII 4 字符约 1 token,非 ASCII 每字
    // 约 1.5 token)折算:
    //   系统提示 = AgentLoop 那份拼装结果 + 目录 base_instructions
    //              + 魂(几层 Backend 包装发请求前拼进 system 的);
    //   工具定义 = registry 里"会真进 tools 数组"的工具(延迟
    //              机制开着就按谓词过滤成核心+已挂载)的
    //              名字+描述+schema,外加延迟索引段;
    //   对话历史 = 压力 dry-run 视图(P1-1 口径统一):与下一次真实请求、
    //              自动压缩触发线同一本。拿全量 history 估会把重复工具
    //              结果与超长回包虚算进去,真请求 47k 时显示 189k,用户
    //              看着贴阈值、模型其实远未到。
    std::size_t sys_tokens = 0;
    std::size_t tools_tokens = 0;
    std::size_t history_tokens = 0;
    // token 估算校准(token 估算校准单):三类估算乘会话校准系数——与 loop
    // 的双闸/预检同一只 (provider,model) 桶、同一枚中位系数,/context 打的
    // 数字与触发判定才是同一本账。状态行(样本数/比率/偏差)随占用卡片
    // 交给 HandleContextCommand;没接线或样本不足时系数 1.0,行为照旧。
    lubancode::agent::TokenCalibrator& calibrator = lubancode::agent::DefaultTokenCalibrator();
    const double token_calibration =
        calibrator.Coefficient(loop.provider(), loop.request_profile().model);
    lubancode::agent::TokenCalibrationStatus token_calibration_status;
    if (args.empty()) {
        sys_tokens = lubancode::agent::ApplyTokenCalibration(
            lubancode::agent::EstimateUtf8Tokens(
                lubancode::agent::AssembleSystemPrompt(*in.prompt_options)) +
                lubancode::agent::EstimateUtf8Tokens(*in.model_instructions) +
                lubancode::agent::EstimateUtf8Tokens(*in.soul),
            token_calibration);
        for (const auto& tool : in.registry->All()) {
            if (!(*in.tool_filter)(*tool)) {
                continue;  // 延迟未挂载:不在 tools 数组里,不算
            }
            tools_tokens += lubancode::agent::EstimateUtf8Tokens(tool->name()) +
                            lubancode::agent::EstimateUtf8Tokens(tool->description()) +
                            lubancode::agent::EstimateUtf8Tokens(tool->input_schema().dump());
        }
        if (in.tool_deferral) {
            tools_tokens += lubancode::agent::EstimateUtf8Tokens(
                lubancode::tools::BuildDeferredToolsIndexSegment(*in.registry, *in.loaded_tools));
        }
        tools_tokens = lubancode::agent::ApplyTokenCalibration(tools_tokens, token_calibration);
        history_tokens = lubancode::agent::EstimateHistoryTokens(loop.context().BuildPressureDryRunView(),
                                                                token_calibration);
        token_calibration_status =
            calibrator.StatusOf(loop.provider(), loop.request_profile().model);
    }
    // 分层占用 + 预算总账(第四期,规格"/context"节):视图各层
    // 枚数从决策台账数,预算从统一公式算,/context 打的就是
    // compact 用的同一本账。
    ContextLayersReport layers;
    if (args.empty()) {
        for (const auto& [id, decision] : loop.result_view_memo().decisions) {
            (void)id;
            switch (decision.kind) {
                case lubancode::agent::ResultViewKind::Full:
                case lubancode::agent::ResultViewKind::NewVersion:
                    layers.inline_full_results += 1;
                    break;
                case lubancode::agent::ResultViewKind::Artifact:
                    layers.artifact_previews += 1;
                    break;
                case lubancode::agent::ResultViewKind::DuplicateRef:
                    break;
            }
        }
        layers.reclaimable_bytes = loop.structural_stats().reclaimable_bytes();
        lubancode::agent::ContextBudgetInputs budget_inputs;
        budget_inputs.window_tokens = context_tracker.window_tokens() > 0
                                          ? std::optional<std::size_t>(context_tracker.window_tokens())
                                          : std::nullopt;
        budget_inputs.stable_system_tokens = lubancode::agent::EstimateUtf8Tokens(
            lubancode::agent::AssembleSystemPrompt(*in.prompt_options), token_calibration);
        budget_inputs.model_instructions_tokens =
            lubancode::agent::ApplyTokenCalibration(lubancode::agent::EstimateUtf8Tokens(*in.model_instructions) +
                                                       lubancode::agent::EstimateUtf8Tokens(*in.soul),
                                                   token_calibration);
        budget_inputs.tool_schemas_tokens = tools_tokens;
        budget_inputs.current_user_turn_tokens = lubancode::agent::EstimateHistoryTokens(
            std::vector<lubancode::api::Message>(loop.History().begin() +
                                                     static_cast<std::ptrdiff_t>(
                                                         lubancode::agent::HotZoneStartIndex(loop.History())),
                                                 loop.History().end()),
            token_calibration);
        budget_inputs.protected_hot_zone_tokens = lubancode::agent::kDefaultHotZoneTokens;
        budget_inputs.requested_output_reserve_tokens =
            static_cast<std::size_t>(loop.runtime_profile().max_output_tokens.value_or(
                lubancode::agent::kUnsetOutputReserveEstimateTokens));
        budget_inputs.compact_prompt_overhead_tokens = 512;  // 压缩指令的公开估算档
        layers.budget = lubancode::agent::BuildContextBudgetPlan(budget_inputs);
        layers.last_compact_line = *in.last_compact_line;
    }
    // deferred_tool_mode(动态工具 PromptCache 守恒单 P0 起;P1 补 proxy
    // 档):裸敲才现场扫 registry 数待检索/全部延迟工具枚数,带参数分支
    //(切窗口)不打这行,跟其余三类 token 收集同一个 args.empty() 闸门。
    // proxy 模式下 loaded 集合恒空(tool_search 不再写它),待检索枚数
    // 就是全部延迟工具——如实数,不另造口径。
    DeferredToolModeSummary deferred_tool_summary;
    const DeferredToolModeSummary* deferred_tool_summary_ptr = nullptr;
    if (args.empty() && in.registry != nullptr) {
        deferred_tool_summary.enabled = in.tool_deferral;
        deferred_tool_summary.mode_label =
            in.proxy_reference
                ? lubancode::tools::DeferredToolModeLabel(lubancode::tools::DeferredToolMode::ProxyReference,
                                                          /*deferral_enabled=*/true)
                : in.native_reference
                      ? lubancode::tools::DeferredToolModeLabel(
                            lubancode::tools::DeferredToolMode::NativeReference, /*deferral_enabled=*/true)
                      : lubancode::tools::DeferredToolModeLabel(in.tool_deferral);
        for (const auto& tool : in.registry->All()) {
            if (!tool->deferred()) {
                continue;
            }
            ++deferred_tool_summary.total;
            if (in.loaded_tools == nullptr || in.loaded_tools->count(tool->name()) == 0) {
                ++deferred_tool_summary.pending;
            }
        }
        deferred_tool_summary_ptr = &deferred_tool_summary;
    }
    HandleContextCommand(args, context_tracker, sys_tokens, tools_tokens, history_tokens, theme,
                         loop.cache_epoch(), &loop.runtime_profile(), in.usage_ledger, in.artifact_store, &layers,
                         in.roles_table, in.compact_partition_count, deferred_tool_summary_ptr,
                         &token_calibration_status);
}

void RunCompactCommand(const std::string& args, const CompactSessionInputs& in) {
    auto& out = lubancode::cli::TermOut();
    lubancode::agent::Agent& loop = *in.agent;
    const lubancode::cli::Theme& theme = *in.theme;
    // PreCompact(trigger=manual):钩子可以拦这一压(备份场景)。
    {
        lubancode::hooks::HookDispatcher* dispatcher = lubancode::app::HookRuntime();
        if (dispatcher != nullptr && !dispatcher->Empty() &&
            dispatcher->HasHandlersFor(lubancode::hooks::HookEvent::PreCompact)) {
            lubancode::hooks::HookPayload payload;
            payload.event = lubancode::hooks::HookEvent::PreCompact;
            payload.fields["trigger"] = "manual";
            payload.match_value = "manual";
            const auto merged = dispatcher->Emit(lubancode::hooks::HookEvent::PreCompact, payload);
            if (merged.blocked) {
                out << theme.error << "PreCompact 钩子拦下这次压缩: " << merged.block_reason
                    << theme.reset << "\n";
                return;
            }
        }
    }
    // 压缩路由(模型分工第一期):/compact 走 cheap 角色的有效值
    // (cheap_model 未配置回落 normal;compact_model 旧字段只在
    // 没配 cheap 时顶替压缩)。backend 可能是跨 provider 的另一只
    // client,拿不到就明说,不拿会话模型顶包。
    const auto compact_routed = in.route_compact();
    if (compact_routed.backend == nullptr) {
        out << theme.error << "压缩路由找不到 provider \"" << compact_routed.route.provider
            << "\",本次 /compact 未执行" << theme.reset << "\n";
        return;
    }
    lubancode::agent::BackgroundCallAccounting compact_accounting;
    // P0-2 轨迹:typed 状态机开卷(§14.4)。requested 先 durable,随后才
    // 动手;old epoch 与输入状态指纹随账带出。flag 关的会话零调用。
    const std::string trajectory_state_before =
        in.trajectory != nullptr ? HistoryStateHash(loop.History()) : std::string();
    if (in.trajectory != nullptr) {
        in.trajectory->RecordCompactRequested("manual", *in.session_compact_epoch, trajectory_state_before);
    }
    // Token 账本单 A1:compact 子请求的旁路桥工厂随路由烤进 options。
    lubancode::agent::CompactOptions compact_options = in.build_compact_options();
    compact_options.bypass_recorder = CompactBypassFactory(in, compact_routed.route.provider);
    const auto compact_result =
        HandleCompactCommand(args, loop, *compact_routed.backend, compact_routed.route, theme,
                             in.spinner_enabled, std::move(compact_options), *in.session_compact_epoch,
                             &compact_accounting);
    // 分角色记账 + 状态栏短闪:压缩用了谁、前后多少,一行交代。
    in.record_usage(lubancode::agent::ModelRole::Cheap, compact_routed.route, compact_accounting);
    if (compact_result.applied) {
        out << theme.stats
            << trf("router.compact_flash", lubancode::cli::FormatTokenCount(compact_result.before_tokens),
                   lubancode::cli::FormatTokenCount(compact_result.after_tokens),
                   "cheap:" + compact_routed.route.model)
            << theme.reset << "\n";
        // 最近一次 compact 的台账(/context 展示,第四期)。
        *in.last_compact_line = "cheap:" + compact_routed.route.model + " · " +
                                lubancode::cli::FormatTokenCount(compact_result.before_tokens) + "→" +
                                lubancode::cli::FormatTokenCount(compact_result.after_tokens) + " · " +
                                std::to_string(compact_accounting.duration_ms / 1000) + "." +
                                std::to_string((compact_accounting.duration_ms % 1000) / 100) +
                                "s · 校验通过(双账+守恒)";
    }
    // P0-2 轨迹:applied/failed 按 Event 投影落账;原事件不改、不抄
    //(§14.4:只改变后续 request 的引用,不重写事实)。
    if (in.trajectory != nullptr) {
        if (compact_result.applied) {
            in.trajectory->RecordCompactApplied(trajectory_state_before, HistoryStateHash(loop.History()),
                                                compact_result.before_tokens, compact_result.after_tokens,
                                                *in.session_compact_epoch);
        } else {
            in.trajectory->RecordCompactFailed("compact_not_applied");
        }
    }
    if (compact_result.applied) {
        // PostCompact 审计 + 压缩后的上下文重注入走 SessionStart
        // (source=compact),不靠 PostCompact 硬塞(规格)。
        if (in.emit_session_hook) {
            in.emit_session_hook(lubancode::hooks::HookEvent::PostCompact, nlohmann::json{{"trigger", "manual"}},
                                 "manual");
            in.emit_session_hook(lubancode::hooks::HookEvent::SessionStart, nlohmann::json{{"source", "compact"}},
                                 "compact");
        }
    }
}

// ---- 自动压缩的会话现场路(终端接线收尾单自大类搬出;原文随行) ---------

bool TryRunCompact(bool midturn, const CompactSessionInputs& in) {
    auto& out = lubancode::cli::TermOut();
    lubancode::agent::Agent& loop = *in.agent;
    const lubancode::cli::Theme& theme = *in.theme;
    // 压缩路由(模型分工第一期):cheap 角色的有效值;跨 provider 拿不到
    // backend 就直接走 normal 修一次的路(同一只),失败再报,不静默截史。
    auto compact_routed = in.route_compact();
    // Token 账本单 A1:子请求旁路桥工厂随 cheap 路由烤进 options;回退
    // 修一次时换 normal 路由重烤(见下)。
    lubancode::agent::CompactOptions options = in.build_compact_options();
    options.bypass_recorder = CompactBypassFactory(in, compact_routed.route.provider);
    // 压缩前后账走压力 dry-run 口径(P1-1 口径统一):与触发线、下一次真实
    // 请求同一本——拿未压缩全量估,重复工具结果全被虚算,压完的"瘦"也是
    // 假瘦。/context 的"最近一次压缩"行、compact_v2 的 pre/post_tokens
    // 与状态栏短闪都从这两个数出。换账前对"新历史"也用同一把尺(临时
    // memo 的 dry-run),反涨断言两边才可比。估算与反涨闸都收拢成手工/
    // 自动共用的一只(PressureEstimateTokens / RejectGrownCompactHistory)。
    const std::size_t before_tokens = PressureEstimateTokens(loop, loop.History());

    // 滞回带(P1-1 连环压缩):上次压缩收口后新增不足滞回带,再压一次榨
    // 不出新空间——热区+存档本身就有十几 k 的底。同一 turn 无进展不得连
    // 压两次;跨 turn 的新用户输入自然带来增量,不受这条拦。
    if (in.hysteresis != nullptr && in.hysteresis->armed &&
        lubancode::agent::ShouldSkipCompactForHysteresis(in.hysteresis->last_post_tokens, before_tokens)) {
        out << theme.stats
            << trf("compact.hysteresis_skip", lubancode::cli::FormatTokenCount(before_tokens),
                   lubancode::cli::FormatTokenCount(in.hysteresis->last_post_tokens))
            << theme.reset << tr("compact.hysteresis_skip_tail") << "\n";
        return false;
    }

    // PreCompact(trigger=auto):自动/中途压缩也过一遍门,可拦。
    {
        lubancode::hooks::HookDispatcher* dispatcher = lubancode::app::HookRuntime();
        if (dispatcher != nullptr && !dispatcher->Empty() &&
            dispatcher->HasHandlersFor(lubancode::hooks::HookEvent::PreCompact)) {
            lubancode::hooks::HookPayload payload;
            payload.event = lubancode::hooks::HookEvent::PreCompact;
            payload.fields["trigger"] = "auto";
            payload.match_value = "auto";
            const auto merged = dispatcher->Emit(lubancode::hooks::HookEvent::PreCompact, payload);
            if (merged.blocked) {
                out << theme.error << "PreCompact 钩子拦下这次自动压缩: " << merged.block_reason
                    << theme.reset << "\n";
                return false;  // 压缩被拦不是错误:主流程照走,旧历史不动
            }
        }
    }

    out << theme.stats << tr(midturn ? "compact.midturn_start" : "compact.auto_start") << theme.reset << "\n";
    // P0-2 轨迹:auto/midturn 与 manual 同一状态机,只差 trigger(§14.4)。
    const std::string trajectory_state_before =
        in.trajectory != nullptr ? HistoryStateHash(loop.History()) : std::string();
    if (in.trajectory != nullptr) {
        in.trajectory->RecordCompactRequested(midturn ? "midturn" : "auto", *in.session_compact_epoch,
                                              trajectory_state_before);
    }
    lubancode::cli::Spinner spinner(theme, in.spinner_enabled);
    // 走路由给的 backend(cheap 跨 provider 时是另一只裸 client,理由同
    // /compact:压缩自己把 route.model 写进 request.model)。分层路:装得下
    // 单次摘要,装不下按 episode 分块 map、归并 reduce。
    lubancode::agent::BackgroundCallAccounting compact_accounting;
    lubancode::agent::ModelRole used_role = lubancode::agent::ModelRole::Cheap;
    // 双账压缩(四分区单·阶段 2-4):与手工 /compact 同一条路、同一套校验
    // 与反涨闸;map/reduce 都走压缩路由(cheap)的模型。
    auto result = lubancode::agent::CompactTurnPartitioned(
        compact_routed.backend != nullptr ? *compact_routed.backend : *in.normal_backend,
        compact_routed.route.model, loop.History(), options, loop.context().structural_options(),
        compact_routed.route.effort, &compact_accounting);

    // cheap 失败的回退(规格"失败与安全"):配了独立 cheap 路由(与 normal
    // 不同模型)、而压缩又没成(请求/校验任一环)时,先试 normal 修一次;
    // 仍失败旧史不动。回退要留痕:状态栏打一行,台账记一笔,不悄悄换人。
    if (!result.has_value() && compact_routed.route.model != *in.current_model) {
        const auto repair_routed = in.route_repair();
        const std::string reason = result.error().message;
        in.record_fallback(lubancode::agent::TaskKind::Compact, lubancode::agent::ModelRole::Cheap,
                           lubancode::agent::ModelRole::Normal, reason);
        out << theme.stats
            << trf("router.fallback_flash", "cheap:" + compact_routed.route.model, "normal:" + repair_routed.route.model)
            << theme.reset << "\n";
        if (repair_routed.backend != nullptr) {
            // 回退换 normal 路由:旁路桥的 identity(provider)重烤,不拿
            // cheap 的名头记 normal 的请求。
            lubancode::agent::CompactOptions repair_options = options;
            repair_options.bypass_recorder = CompactBypassFactory(in, repair_routed.route.provider);
            result = lubancode::agent::CompactTurnPartitioned(*repair_routed.backend, repair_routed.route.model,
                                                               loop.History(), repair_options,
                                                               loop.context().structural_options(),
                                                               repair_routed.route.effort, &compact_accounting);
            if (result.has_value()) {
                compact_routed.route = repair_routed.route;
                used_role = lubancode::agent::ModelRole::Normal;
            }
        }
    }
    spinner.Stop();

    // 分角色记账:成功走的哪个角色就记哪笔(回退后是 normal)。
    in.record_usage(used_role, compact_routed.route, compact_accounting);

    if (!result.has_value()) {
        out << theme.error << trf("compact.auto_failed", result.error().message) << theme.reset
            << tr("compact.auto_failed_tail") << "\n";
        if (in.trajectory != nullptr) {
            in.trajectory->RecordCompactFailed(result.error().message);
        }
        // 失败也收滞回账:本 turn 的压缩尝试到此为止——同一段历史反复发
        // 摘要请求,烧的是几分钟一轮的压缩费,循环比失败更伤。字符安全网
        // 仍兜底,回执已在上面给了。
        if (in.hysteresis != nullptr) {
            in.hysteresis->armed = true;
            in.hysteresis->last_post_tokens = before_tokens;
        }
        return false;
    }

    const auto& new_history = result->new_history;
    const std::size_t new_tokens = PressureEstimateTokens(loop, new_history);
    // 反涨断言(P1-1):压缩后的新历史必须明显小于压缩前,否则换账就是
    // 反涨——存档添在没瘦的热区头上,真机 70.8k 压成 73.7k 还标"校验通过"。
    // 闸与手工 /compact 共用(RejectGrownCompactHistory)。拒收:旧历史不动,
    // 滞回账记上,回执讲清"当前轮占大头,压缩收窄不了"。
    if (RejectGrownCompactHistory(theme, before_tokens, new_tokens)) {
        if (in.hysteresis != nullptr) {
            in.hysteresis->armed = true;
            in.hysteresis->last_post_tokens = before_tokens;
        }
        if (in.trajectory != nullptr) {
            in.trajectory->RecordCompactFailed("grown_history_rejected");
        }
        return false;
    }
    loop.ReplaceHistory(new_history);
    const std::size_t after_tokens = new_tokens;  // 同一份历史,同一把尺,不重算
    // 成功换账:滞回账记上收口点。
    if (in.hysteresis != nullptr) {
        in.hysteresis->armed = true;
        in.hysteresis->last_post_tokens = after_tokens;
    }
    // 状态栏短闪:压缩前后与所用角色一行交代(规格"运行提示")。
    out << theme.stats
        << trf("router.compact_flash", lubancode::cli::FormatTokenCount(before_tokens),
               lubancode::cli::FormatTokenCount(after_tokens),
               (used_role == lubancode::agent::ModelRole::Normal ? std::string("normal:") : std::string("cheap:")) +
                   compact_routed.route.model)
        << theme.reset << "\n";
    // 最近一次 compact 的台账(/context"最近一次 compact"一行,规格第四期)。
    *in.last_compact_line =
        (used_role == lubancode::agent::ModelRole::Normal ? std::string("normal:") : std::string("cheap:")) +
        compact_routed.route.model + " · " + lubancode::cli::FormatTokenCount(before_tokens) + "→" +
        lubancode::cli::FormatTokenCount(after_tokens) + " · " +
        std::to_string(compact_accounting.duration_ms / 1000) + "." +
        std::to_string((compact_accounting.duration_ms % 1000) / 100) + "s · 校验通过(双账+守恒)";

    *in.session_compact_epoch += 1;
    // (P0-6:compact_v2 事件行随 SessionStore 删除;持久账是 trajectory 的
    // compact.applied,紧接着落。)

    if (!options.budget.window_tokens.has_value()) {
        out << theme.stats << tr("cmd.compact.window_unknown") << theme.reset << "\n";
    }
    if (result->metrics.hierarchical) {
        out << trf("cmd.compact.hierarchical", result->metrics.chunks, result->metrics.reduce_passes) << "\n";
    }
    out << trf("compact.done_stats", after_tokens, result->manifest.constraints.size(),
               result->manifest.open_items.size())
        << "\n";
    if (midturn) {
        out << tr("compact.midturn_done") << "\n";
    }
    // PostCompact 审计 + 压缩后的上下文重注入走 SessionStart(source=
    // compact)——自动压缩后紧接着的续请求前送达,不拖到下一条用户消息
    // (本函数就活在那个安全点里)。
    if (in.emit_session_hook) {
        in.emit_session_hook(lubancode::hooks::HookEvent::PostCompact, nlohmann::json{{"trigger", "auto"}}, "auto");
        in.emit_session_hook(lubancode::hooks::HookEvent::SessionStart, nlohmann::json{{"source", "compact"}},
                             "compact");
    }
    return true;
}

void HandleContextPressure(const lubancode::agent::ContextPressure& pressure, const CompactSessionInputs& in) {
    auto& out = lubancode::cli::TermOut();
    const lubancode::cli::Theme& theme = *in.theme;
    if (pressure.phase == lubancode::agent::ContextPressure::Phase::PreRequest) {
        // 工具结果已攒完、请求尚未发出——正是不打断工具的安全点。撞线就
        // 在这里收一次历史,不再等下一条外层用户消息。
        if (pressure.projected_overflow) {
            TryRunCompact(/*midturn=*/true, in);
        }
        return;
    }
    // AfterHardTrim:字符安全网这次真丢了东西。显式告警,不许静默降级——
    // 用户须知道模型眼下已经看不到那段原文;完整流水仍在存档,/export 可查。
    if (pressure.hard_trimmed_turns) {
        out << theme.error << trf("compact.hard_trim_turns", pressure.hard_dropped_messages) << theme.reset << "\n";
    } else if (pressure.hard_truncated_results) {
        out << theme.error << tr("compact.hard_trim_results") << theme.reset << "\n";
    }
}

// ---------------------------------------------------------------------------
// 命令分派注册制(会话终章):会话域的分派位。case 体原样自
// interactive_session 的大 switch 搬来,材料经 SlashDispatchContext 递入。
// ---------------------------------------------------------------------------

void PrintSlashHelp() {
    // 名单与 --help、Tab 补全同源(P3-2):正文打 cli::FormatSlashCommandListLines()
    // 生成的行,不再手抄进 i18n 表——手抄那份漏了 /plan /agents /agent。
    TermOut() << tr("slash_help.body");
    for (const std::string& line : lubancode::cli::FormatSlashCommandListLines()) {
        TermOut() << line << "\n";
    }
    TermOut() << tr("slash_help.keys");
}

CommandFlow HandleSlashHelp(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    (void)ctx;
    (void)parsed;
    PrintSlashHelp();
    return CommandFlow::Continue;
}

// ---------------------------------------------------------------------------
// P0-3 轨迹档的边界命令:/clear 八步换账、/resume 七步、/export 读 ReplayState
// ---------------------------------------------------------------------------

namespace {

// /clear 的运行时参与人(§3.3.1 第 3 步):轨迹侧 SessionManager 掌账,
// 这里只交"事实申报"。后台子代理的收口是 bounded join——CancelAllTasks
// 后限时等它们把 terminal 写进旧目录;到点仍 running 的记 unknown,不
// 装成功(closed 硬门,§3.3.2)。
class TrajectoryClearParticipant : public lubancode::trajectory::ClearParticipant {
public:
    TrajectoryClearParticipant(lubancode::runtime::TrajectorySessionLedger* ledger,
                               lubancode::tools::AgentTool* agent_tool)
        : ledger_(ledger), agent_tool_(agent_tool) {}

    std::string CancelActiveTurn() override { return {}; }  // slash 只在空闲时分派

    std::vector<ChildClosure> CancelActiveChildren() override {
        std::vector<ChildClosure> closures;
        if (agent_tool_ == nullptr) {
            return closures;
        }
        std::vector<int> running;
        for (const auto& task : agent_tool_->TaskSummaries()) {
            if (task.state == lubancode::tools::AgentTaskState::Running) {
                running.push_back(task.id);
            }
        }
        if (running.empty()) {
            return closures;
        }
        (void)agent_tool_->CancelAllTasks();
        // bounded join:至多等两秒,子代理线程各自在旧目录落 terminal。
        for (int waited_ms = 0; waited_ms < 2000; waited_ms += 50) {
            bool any_running = false;
            for (const auto& task : agent_tool_->TaskSummaries()) {
                if (task.state == lubancode::tools::AgentTaskState::Running) {
                    any_running = true;
                    break;
                }
            }
            if (!any_running) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        for (const int task_id : running) {
            ChildClosure closure;
            closure.run_id = "task-" + std::to_string(task_id);
            closure.terminal_written = true;
            closure.unknown = false;
            for (const auto& task : agent_tool_->TaskSummaries()) {
                if (task.id == task_id && task.state == lubancode::tools::AgentTaskState::Running) {
                    closure.terminal_written = false;  // 等到点还没停:unknown 不装成功
                    closure.unknown = true;
                    break;
                }
            }
            closures.push_back(std::move(closure));
        }
        return closures;
    }

    std::vector<std::string> CancelQueuedItems() override {
        // P0-4 排队账顺接:活队列里所有"等下一轮"的条目逐枚申报,clear
        // 八步在旧账里落 control.queue.item.cancelled(reason=clear)。id 用
        // 轨迹口径的 "q-<n>",与 enqueue 时记下的一致,对账才对得上。
        // TargetGone/Failed(等用户处置的)也一并算未送达——清场就是全清。
        std::vector<std::string> ids;
        for (const auto& item : lubancode::cli::SessionSteeringQueue().Snapshot()) {
            ids.push_back(lubancode::cli::QueueItemId(item.id));
        }
        return ids;
    }

    std::string ActiveRecordSelectionId() override {
        return ledger_ != nullptr && ledger_->record_selection().active()
                   ? ledger_->record_selection().record_id()
                   : std::string();
    }

    void ResetInMemoryState() override {
        // 第 8 步的清内存由调用方在 ClearSession 返回后办(UI 重建、hooks)。
    }

private:
    lubancode::runtime::TrajectorySessionLedger* ledger_ = nullptr;
    lubancode::tools::AgentTool* agent_tool_ = nullptr;
};

}  // namespace

CommandFlow HandleSlashClear(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    (void)parsed;
    const lubancode::cli::Theme& theme = *ctx.theme;
    // stash 是"还没说出口的话",不跟 history 一锅清(规格:草稿各自存账);
    // 清场时提醒一句它还在。
    if (lubancode::cli::ComposerStashHasContent()) {
        TermOut() << theme.stats << tr("stash.still_there") << theme.reset << "\n";
    }
    // Plan 模式单:/clear 起新 thread,回默认配置(单子"切换规矩")。计划
    // 成品与审阅悬稿一并翻篇,不继承。
    if (ctx.session_runtime->collaboration_mode() == lubancode::runtime::CollaborationMode::Plan) {
        ctx.switch_collaboration_mode(lubancode::runtime::CollaborationMode::Default, "clear");
    }
    ctx.reset_plan_review();
    SessionCommandState session_state = ctx.make_session_command_state();
    // P0-3 轨迹档(P0-2 遗留#3 收口):clear 走 SessionManager 八步换账
    //(§3.3.1)——旧账封口、新账开张,不复用 ID、不继承 history。flag 关
    // 照旧路,一字不变。
    if (ctx.trajectory != nullptr) {
        if (ctx.spinner_enabled) {
            ClearAndPrintBanner(*ctx.config, theme);
        }
        TrajectoryClearParticipant participant(ctx.trajectory, ctx.agent_tool);
        lubancode::trajectory::ClearRequest request;
        request.reason = "user_clear";
        request.user_initiated = true;
        const auto outcome = ctx.trajectory->ClearSession(request, &participant);
        if (!outcome.error_code.empty()) {
            TermOut() << theme.error << "clear 换账失败(" << outcome.error_code << "): "
                      << outcome.message << theme.reset << "\n";
            return CommandFlow::Continue;
        }
        // 第 8 步:清内存(与旧路同一套善后;store.Reset 无害——轨迹档
        // 旧档未开,原就是空操作)。
        session_state.rebuild_loop(false);
        if (session_state.on_agents_cleanup) {
            session_state.on_agents_cleanup();
        }
        session_state.start_ts = lubancode::tools::NowIdTimestamp();
        if (session_state.on_session_restarted) {
            session_state.on_session_restarted();
        }
        session_state.title.clear();
        TermOut() << tr("cmd.clear.done") << "\n";
        return CommandFlow::Continue;
    }
    // (P0-6:旧路已删;/clear 现行路是 trajectory 新场,账本恒开。)
    TermOut() << tr("session.no_home") << "\n";
    return CommandFlow::Continue;
}

CommandFlow HandleSlashContext(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    // /context presenter(终端接线收尾单):三类 token 与分层预算的现场收集
    // 在本文件,分派位只装材料。
    lubancode::app::ContextEstimateInputs context_in;
    context_in.prompt_options = ctx.prompt_options;
    context_in.model_instructions = ctx.current_model_instructions.get();
    context_in.soul = ctx.current_soul.get();
    context_in.registry = ctx.registry;
    context_in.tool_filter = ctx.main_tool_filter;
    context_in.tool_deferral = ctx.main_deferral;
    context_in.proxy_reference = ctx.main_proxy_reference;
    context_in.native_reference = ctx.main_native_reference;
    context_in.loaded_tools = &**ctx.loaded_tools;
    context_in.agent = ctx.main_agent;
    context_in.context_tracker = ctx.context_tracker;
    context_in.usage_ledger = ctx.model_router != nullptr ? &ctx.model_router->ledger() : nullptr;
    // 三角色有效路由(问题 6):与 /model roles 同一份 ModelRouterService
    // 的 Table(),栈上存一份给本次打印用(分角色账列全三角色 + 回落口径)。
    lubancode::agent::ModelRouteTable roles_table_storage;
    if (ctx.model_router != nullptr) {
        roles_table_storage = ctx.model_router->Table();
        context_in.roles_table = &roles_table_storage;
    }
    context_in.artifact_store = ctx.artifact_store.get();
    context_in.last_compact_line = ctx.last_compact_line;
    if (ctx.config != nullptr) {
        context_in.compact_partition_count = ctx.config->compact_partition_count;
    }
    RunContextCommand(parsed.args, context_in, *ctx.theme);
    return CommandFlow::Continue;
}

CommandFlow HandleSlashCompact(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    // /compact presenter:接线全在本文件,材料包由 make_compact_inputs 装好。
    lubancode::app::RunCompactCommand(parsed.args, ctx.make_compact_inputs());
    return CommandFlow::Continue;
}

CommandFlow HandleSlashRecord(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    // 只做接线:解析/问话/起草/安装全在 cli/record_command.cpp;材料包由
    // 录制接线器装。
    lubancode::cli::RecordCommandContext record_ctx = ctx.record_wiring->MakeCommandContext();
    lubancode::cli::HandleRecordCommand(parsed.args, record_ctx, *ctx.theme);
    return CommandFlow::Continue;
}

CommandFlow HandleSlashSessions(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    PrintSessionsCommand(ctx.trajectory, parsed.args);
    return CommandFlow::Continue;
}

CommandFlow HandleSlashArchive(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    // /archive(会话管理器单第四步):刷盘关柄→搬 archive/→退出。后台子代理
    // 还在跑时拒绝——归档的是会话档,别把还在写档的代理晾在半路。
    const lubancode::cli::Theme& theme = *ctx.theme;
    if (!parsed.args.empty()) {
        TermOut() << theme.error << tr("cmd.archive.usage") << theme.reset << "\n";
        return CommandFlow::Continue;
    }
    bool busy = false;
    if (lubancode::tools::AgentTool* agent_tool = ctx.agent_tool; agent_tool != nullptr) {
        for (const auto& task : agent_tool->TaskSummaries()) {
            if (task.state == lubancode::tools::AgentTaskState::Running) {
                busy = true;
                break;
            }
        }
    }
    if (busy) {
        TermOut() << theme.error << tr("cmd.archive.busy") << theme.reset << "\n";
        return CommandFlow::Continue;
    }
    if (ArchiveCurrentSession(ctx.trajectory, theme)) {
        TermOut() << tr("cmd.archive.exiting") << "\n";
        return CommandFlow::Exit;
    }
    return CommandFlow::Continue;
}

CommandFlow HandleSlashDelete(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    // /delete(第五步):永久删除当前会话。回合在跑/工具在飞/审批悬着时拒
    // 绝——slash 分派本身只在输入线程空闲时进,但后台子代理可能在飞,这里
    // 如实拦。确认屏在 handler。
    const lubancode::cli::Theme& theme = *ctx.theme;
    if (!parsed.args.empty()) {
        TermOut() << theme.error << tr("cmd.delete.usage") << theme.reset << "\n";
        return CommandFlow::Continue;
    }
    bool busy = false;
    if (lubancode::tools::AgentTool* agent_tool = ctx.agent_tool; agent_tool != nullptr) {
        for (const auto& task : agent_tool->TaskSummaries()) {
            if (task.state == lubancode::tools::AgentTaskState::Running) {
                busy = true;
                break;
            }
        }
    }
    if (busy) {
        TermOut() << theme.error << tr("cmd.delete.busy") << theme.reset << "\n";
        return CommandFlow::Continue;
    }
    if (DeleteCurrentSession(ctx.trajectory, theme, *ctx.session_title, CurrentDirUtf8(),
                             /*stdin_line=*/nullptr)) {
        TermOut() << tr("cmd.delete.exiting") << "\n";
        return CommandFlow::Exit;
    }
    return CommandFlow::Continue;
}

CommandFlow HandleSlashResume(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    const lubancode::cli::Theme& theme = *ctx.theme;
    // P0-3 轨迹档(§10.4):resume 走七步——source 只读(验账→折叠→悬空
    // 分档),当前场以 switch_to_resume 封口,新场 start_reason=resume 开张;
    // source Journal 永不 reopen append。flag 关照旧 SessionStore 路不动。
    if (ctx.trajectory != nullptr) {
        std::string target = parsed.args;
        if (target.empty()) {
            // 裸敲:全屏选择器(真控制台);非交互退回最近一场。
            const auto selected = PromptResumeTarget(ctx.trajectory, theme);
            if (selected.has_value()) {
                target = *selected;
            } else {
                target = ctx.trajectory->LatestResumableSessionId();
            }
            if (target.empty()) {
                TermOut() << tr("cmd.resume.none") << "\n";
                return CommandFlow::Continue;
            }
        } else {
            // 编号引用(按 /sessions 的列表序号,新→旧):先解编号再进七步。
            bool all_digits = true;
            for (const char c : target) {
                if (c < '0' || c > '9') {
                    all_digits = false;
                    break;
                }
            }
            if (all_digits) {
                std::size_t n = 0;
                try {
                    n = static_cast<std::size_t>(std::stoul(target));
                } catch (...) {
                    n = 0;
                }
                lubancode::trajectory::SessionIndexQuery query;
                query.limit = 20;
                const auto page = ctx.trajectory->ListWorkspaceSessions(query);
                if (n < 1 || n > page.entries.size()) {
                    TermOut() << trf("cmd.resume.out_of_range", target, page.entries.size()) << "\n";
                    return CommandFlow::Continue;
                }
                target = page.entries[n - 1].session_id;
            }
        }
        const auto summary = ctx.trajectory->ResumeInteractive(target, "resume");
        if (!summary.outcome.error_code.empty()) {
            TermOut() << theme.error << "resume 失败(" << summary.outcome.error_code
                      << "): " << summary.outcome.message << theme.reset << "\n";
            return CommandFlow::Continue;
        }
        ctx.main_agent->RestoreSessionHistory(summary.history);
        // 标题真值吃 replay 折叠(control.title.changed 的最后一条)。
        *ctx.session_title = summary.outcome.control.title.value_or(std::string());
        if (summary.outcome.approval_mode.has_value()) {
            // resume 继承盘上档:公共值域过具名桥回 CLI 显示档(收口审计
            // 单 P1:枚举间 static_cast 禁绝)。
            lubancode::cli::SetConfirmMode(lubancode::cli::ToConfirmMode(*summary.outcome.approval_mode));
        }
        TermOut() << trf("cmd.resume.restored", summary.outcome.source_session_id,
                         summary.history.size())
                  << "(新 session " << summary.outcome.new_session_id << ")\n";
        TermOut() << trf("cmd.resume.estimate", EstimateHistoryTokens(summary.history)) << "\n";
        if (!summary.outcome.dangling_tools.empty()) {
            TermOut() << theme.stats << "尾部悬空工具 " << summary.outcome.dangling_tools.size()
                      << " 道(已按三道账封存,未知副作用不重跑)" << theme.reset << "\n";
        }
        SessionCommandState session_state = ctx.make_session_command_state();
        if (session_state.on_resumed) {
            session_state.on_resumed();  // SessionStart source=resume
        }
        if (session_state.sync_worktree_directory && ctx.worktree_session != nullptr &&
            ctx.worktree_session->active()) {
            session_state.sync_worktree_directory();
        }
        return CommandFlow::Continue;
    }
    // (P0-6:旧路已删;没有轨迹账本就无从 resume——账本恒开。)
    TermOut() << tr("session.no_home") << "\\n";
    return CommandFlow::Continue;
}

CommandFlow HandleSlashExport(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    // P0-3 轨迹档(§14.5:/export 一律读 ReplayState,不从旁路 history 或
    // SessionStore 取数):折叠本场 main.jsonl 再投影导出。flag 关照旧路。
    if (ctx.trajectory != nullptr) {
        const auto fold = ctx.trajectory->FoldMainReplay();
        if (!fold.ok()) {
            TermOut() << trf("cmd.resume.read_failed", fold.message) << "\n";
            return CommandFlow::Continue;
        }
        const std::vector<lubancode::api::Message> history =
            lubancode::runtime::ProjectHistoryFromReplay(fold.state);
        if (history.empty()) {
            TermOut() << tr("cmd.export.empty") << "\n";
            return CommandFlow::Continue;
        }
        const std::string id = ctx.trajectory->session_id();
        std::string out_path = parsed.args;
        if (out_path.empty()) {
            // P0-2:出档归 session exports/(单子 §三:导出统一放 exports/)。
            out_path = (ctx.trajectory->session_dir() / "exports" / (id + ".md")).generic_string();
        }
        lubancode::tools::ExportSessionHeader header;
        header.cwd = fold.state.control.cwd.value_or(std::string());
        const std::string& title = *ctx.session_title;
        const std::string markdown = lubancode::tools::ExportSessionMarkdown(
            header, history, id, /*max_result_lines=*/30, title);
        const std::filesystem::path path(
            std::u8string(reinterpret_cast<const char8_t*>(out_path.data()), out_path.size()));
        std::error_code ec;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), ec);
        }
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            TermOut() << trf("cmd.export.write_failed", out_path) << "\n";
            return CommandFlow::Continue;
        }
        file << markdown;
        TermOut() << trf("cmd.export.done", out_path) << "\n";
        return CommandFlow::Continue;
    }
    // (P0-6:旧路已删;没有轨迹账本的会话导不出——账本恒开,开不出时
    // 装配层早已失败会话启动。)
    return CommandFlow::Continue;
}

CommandFlow HandleSlashTitle(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    // P0-2:标题真账走 control.title.changed(resume 折叠回
    // ReplayControlState.title);旧 SessionStore 的 title 事件行不再写。
    if (ctx.trajectory != nullptr) {
        SessionCommandState session_state = ctx.make_session_command_state();
        if (parsed.args.empty()) {
            TermOut() << (session_state.title.empty() ? tr("cmd.title.none")
                                                      : trf("cmd.title.current", session_state.title))
                      << "\n";
            return CommandFlow::Continue;
        }
        const std::string old_title = session_state.title;
        session_state.title = parsed.args;
        ctx.trajectory->RecordTitleChanged(parsed.args, old_title);
        TermOut() << trf("cmd.title.set", session_state.title) << "\n";
        if (session_state.on_title_changed) {
            session_state.on_title_changed(session_state.title);
        }
        return CommandFlow::Continue;
    }
    // (P0-6:旧路已删;标题真账走 control.title.changed。)
    TermOut() << tr("session.no_home") << "\\n";
    return CommandFlow::Continue;
}

CommandFlow HandleSlashExit(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    (void)ctx;
    (void)parsed;
    return CommandFlow::Exit;
}

}  // namespace lubancode::app
