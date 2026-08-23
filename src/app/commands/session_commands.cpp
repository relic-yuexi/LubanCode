// session_commands.hpp 的实现:上下文/压缩/会话存档命令的函数体。
#include "app/commands/session_commands.hpp"

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <sstream>
#include <vector>

#include "agent/compact.hpp"
#include "agent/artifact_store.hpp"
#include "agent/context_events.hpp"
#include "config/config.hpp"
#include "cli/spinner.hpp"
#include "cli/transcript.hpp"
#include "platform/console.hpp"

#include <filesystem>
#include <optional>
#include <string>

#include "agent/loop.hpp"
#include "agent/session_catalog.hpp"
#include "agent/session_store.hpp"
#include <nlohmann/json.hpp>
#include "app/commands/settings_commands.hpp"
#include "app/runtime_profile.hpp"
#include "cli/console_input.hpp"
#include "cli/context_tracker.hpp"
#include "cli/i18n.hpp"
#include "cli/session_picker.hpp"
#include "cli/session_picker_panel.hpp"
#include "cli/theme.hpp"
#include "cli/worktree.hpp"
#include "platform/paths.hpp"
#include "runtime/session_command_service.hpp"

namespace lubancode::app {


using lubancode::platform::CurrentDirUtf8;
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
                           const ContextLayersReport* layers) {
    if (args.empty()) {
        const auto lines = lubancode::cli::FormatContextBreakdown(
            sys_tokens, tools_tokens, history_tokens, context_tracker.last_cache_read_tokens(),
            context_tracker.window_tokens(), context_tracker.current_tokens(), theme,
            /*bar_width=*/16, context_tracker.last_cache_hit_percent());
        for (const auto& line : lines) {
            std::cout << line << "\n";
        }
        // 输出上限与来源(规格根因一):本轮每份请求给模型留的输出空间,
        // unset 也说破——"交服务端默认"比一枚看不见的 4096 诚实。输出预留
        // 已计入上面的 projected 评估(loop.cpp),这里展示的就是那份值。
        if (main_profile != nullptr) {
            if (main_profile->max_output_tokens.has_value()) {
                std::cout << trf("cmd.context.output_budget", *main_profile->max_output_tokens,
                                 app::OutputBudgetSourceText(main_profile->max_output_tokens_source, false))
                          << "\n";
            } else {
                std::cout << tr("cmd.context.output_budget_unset") << "\n";
            }
        }
        // 前缀缓存账(前缀缓存守恒单):epoch 与最近一次请求的命中率一行
        // 交代——命中跌下去时,用户看得出是主动换了哪根梁(epoch 断因在
        // 回合统计行/逐步流水账里),不再笼统赖服务端。没实测过就明说。
        if (context_tracker.last_total_input_tokens() > 0) {
            const int hit_percent = context_tracker.last_cache_hit_percent();
            std::cout << trf("cmd.context.epoch", cache_epoch,
                             lubancode::cli::FormatTokenCount(context_tracker.last_cache_read_tokens()),
                             lubancode::cli::FormatTokenCount(context_tracker.last_total_input_tokens()),
                             hit_percent >= 0 ? std::to_string(hit_percent) : std::string("?"))
                      << "\n";
        }
        // 口径说明:状态栏与这里读的是同一只 tracker,都是"最近一次主请求
        // 的占用",不是会话累计花销,也不含独立子代理的 token。最近一次请求
        // 没带回 usage 时再补一行旧值提醒(状态栏同款 ~ 前缀的完整说法)。
        std::cout << tr("cmd.context.note.semantics") << "\n";
        if (context_tracker.usage_stale()) {
            std::cout << tr("cmd.context.note.stale") << "\n";
        }
        if (context_tracker.ShouldAutoCompact()) {
            std::cout << tr("cmd.context.compact_hint") << "\n";
        }
        // artifact 层(第二期,规格"/context"节):落盘几枚、全文多少字节
        // 可追回——"原文还能去哪找"看得见。
        if (artifact_store != nullptr && artifact_store->active()) {
            const auto stats = artifact_store->StatsOf();
            if (stats.artifacts > 0) {
                std::cout << trf("cmd.context.artifacts", stats.artifacts, stats.total_bytes) << "\n";
            } else {
                std::cout << tr("cmd.context.artifacts_none") << "\n";
            }
        }
        // 分层占用与预算总账(第四期,规格"/context"节):inline 正文/L1
        // 预览/L2 摘要各几枚、预算怎么扣的、下一触发线在哪、最近一次
        // compact 用了谁——一张单子说清 token 花在哪、何时会压、原文去
        // 哪找(规格验收)。
        if (layers != nullptr) {
            std::cout << trf("cmd.context.layers", layers->inline_full_results, layers->artifact_previews,
                             layers->microcompact_summaries)
                      << "\n";
            if (layers->reclaimable_bytes > 0) {
                std::cout << trf("cmd.context.reclaimable", layers->reclaimable_bytes) << "\n";
            }
            if (layers->budget.has_value()) {
                const auto& plan = *layers->budget;
                std::cout << trf("cmd.context.budget", plan.window,
                                 plan.compactable_history_budget.has_value()
                                     ? lubancode::cli::FormatTokenCount(*plan.compactable_history_budget)
                                     : std::string("?"),
                                 lubancode::cli::FormatTokenCount(plan.overhead_total()))
                          << "\n";
                std::cout << trf("cmd.context.budget_detail", plan.stable_system + plan.model_instructions,
                                 plan.tool_schemas, plan.protected_hot_zone, plan.requested_output_reserve,
                                 plan.compact_prompt_overhead + plan.protocol_headroom,
                                 plan.tokenizer_error_margin)
                          << "\n";
                if (plan.compact_call_input_budget.has_value()) {
                    std::cout << trf("cmd.context.compact_budget",
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
                    std::cout << trf("cmd.context.next_line", lubancode::cli::FormatTokenCount(line),
                                     used >= 0 ? lubancode::cli::FormatTokenCount(used) : std::string("0"),
                                     used >= static_cast<std::int64_t>(line)
                                             ? tr("cmd.context.next_line_over")
                                             : lubancode::cli::FormatTokenCount(
                                                   static_cast<std::int64_t>(line) - used))
                              << "\n";
                }
            }
            if (!layers->last_compact_line.empty()) {
                std::cout << trf("cmd.context.last_compact", layers->last_compact_line) << "\n";
            }
        }
        // 分角色 usage 台账(模型分工第一期,规格"路由看得见"):普通 turn
        // 归 normal,压缩/抽取/标题的后台采样归 cheap,回退单独留痕。
        if (usage_ledger != nullptr) {
            const auto role_lines = usage_ledger->ReportLines();
            if (!role_lines.empty()) {
                std::cout << tr("router.usage.header") << "\n";
                for (const std::string& line : role_lines) {
                    std::cout << "  " << line << "\n";
                }
            }
            if (!usage_ledger->fallback_notes().empty()) {
                std::cout << tr("router.usage.fallback_header") << "\n";
                for (const std::string& note : usage_ledger->fallback_notes()) {
                    std::cout << "  " << note << "\n";
                }
            }
        }
        return;
    }
    const auto parsed = lubancode::config::ParseContextWindowTokens(args);
    if (!parsed.has_value()) {
        std::cout << parsed.error() << "\n";
        return;
    }
    context_tracker.set_window_tokens(*parsed);
    std::cout << trf("cmd.context.window_changed", *parsed) << "\n";
}

// /compact 命令:窗口预算 + manifest 守恒校验 + 热区保留,一条路走到底。
// --dry-run 只算结构压缩的可回收量与钉住项,不发请求、不改历史。
CompactCommandResult HandleCompactCommand(const std::string& args, lubancode::agent::AgentLoop& loop,
                                          lubancode::api::Backend& raw_backend,
                                          const lubancode::agent::ModelRoute& compact_route,
                                          const lubancode::cli::Theme& theme, bool spinner_enabled,
                                          const lubancode::agent::CompactOptions& options, int& compact_epoch,
                                          lubancode::agent::BackgroundCallAccounting* accounting) {
    const std::vector<lubancode::api::Message>& history = loop.History();
    if (history.empty()) {
        std::cout << tr("cmd.compact.empty") << "\n";
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
        std::cout << tr("cmd.compact.dryrun.header") << "\n";
        std::cout << trf("cmd.compact.dryrun.reclaim", struct_stats.reclaimable_bytes(),
                         struct_stats.duplicate_groups, struct_stats.superseded_observations,
                         struct_stats.offloaded_results)
                  << "\n";
        std::cout << trf("cmd.compact.dryrun.pinned", lubancode::agent::EstimateHistoryTokens(hot),
                         options.required_open_items.size())
                  << "\n";
        return {};
    }

    const std::size_t before_tokens = EstimateHistoryTokens(history);
    const std::size_t old_size = history.size();

    lubancode::agent::CompactOptions run_options = options;
    run_options.focus = focus;

    lubancode::cli::Spinner spinner(theme, spinner_enabled);
    const auto result = lubancode::agent::CompactHierarchical(raw_backend, compact_route.model, history,
                                                              run_options, compact_route.effort, accounting);
    spinner.Stop();

    if (!result.has_value()) {
        std::cout << theme.error << trf("cmd.compact.failed", result.error().message) << theme.reset << "\n";
        return {};
    }

    const auto new_history = lubancode::agent::BuildCompactedHistory(history, result->archive);
    const auto base_event = lubancode::agent::MakeCompactEvent(old_size, new_history);
    loop.ReplaceHistory(new_history);
    const std::size_t after_tokens = EstimateHistoryTokens(new_history);

    // compact_v2 事件:回放语义与 v1 同型,多的 manifest/epoch/metrics 供
    // 审计与 rebase。
    compact_epoch += 1;
    nlohmann::json manifest_json;
    manifest_json["goal"] = result->manifest.goal;
    manifest_json["constraints"] = result->manifest.constraints;
    manifest_json["open_items"] = result->manifest.open_items;
    manifest_json["next_action"] = result->manifest.next_action;
    nlohmann::json metrics_json;
    metrics_json["chunks"] = result->metrics.chunks;
    metrics_json["reduce_passes"] = result->metrics.reduce_passes;
    metrics_json["hierarchical"] = result->metrics.hierarchical;
    metrics_json["implementation"] = result->metrics.implementation;
    metrics_json["source_digest"] = result->metrics.source_digest;  // 第四期预计算复用钩子
    metrics_json["pre_tokens"] = before_tokens;
    metrics_json["post_tokens"] = after_tokens;
    metrics_json["trigger"] = "manual";
    const auto event = lubancode::agent::UpgradeToV2(base_event, compact_epoch, std::move(manifest_json),
                                                     std::move(metrics_json));

    std::cout << trf("cmd.compact.result", before_tokens, after_tokens) << "\n";
    if (result->metrics.hierarchical) {
        std::cout << trf("cmd.compact.hierarchical", result->metrics.chunks, result->metrics.reduce_passes)
                  << "\n";
    }
    if (!options.budget.window_tokens.has_value()) {
        std::cout << theme.stats << tr("cmd.compact.window_unknown") << theme.reset << "\n";
    }
    std::cout << trf("cmd.compact.manifest", result->manifest.constraints.size(),
                     result->manifest.open_items.size())
              << "\n";
    return CompactCommandResult{event, before_tokens, after_tokens, result->manifest.constraints.size(),
                                result->manifest.open_items.size()};
}
void PrintSessionsCommand(const std::string& sessions_dir, const std::string& args) {
    if (sessions_dir.empty()) {
        std::cout << tr("session.no_home") << "\n";
        return;
    }
    // /sessions archived:归档只读入口(第四步)。列 archive/ 子目录,标明
    // 这些场子不在默认列表里;想续聊先 `lubancode unarchive <id>`。
    if (args == "archived") {
        lubancode::agent::SessionCatalog catalog(sessions_dir);
        catalog.Scan();
        lubancode::agent::SessionQuery query;
        query.scope = lubancode::agent::SessionScope::All;
        query.state = lubancode::agent::SessionState::Archived;
        query.sort = lubancode::agent::SessionSort::Updated;
        query.limit = 0;
        const auto page = catalog.Query(query);
        if (page.entries.empty()) {
            std::cout << tr("cmd.sessions.archived_none") << "\n";
            return;
        }
        std::cout << trf("cmd.sessions.archived_header", page.total) << "\n";
        for (const auto& entry : page.entries) {
            const std::string& label = !entry.title.empty() ? entry.title : entry.first_user_text;
            std::cout << "  " << entry.id << "\n"
                      << trf("cmd.sessions.entry",
                              entry.updated_at.empty() ? tr("cmd.sessions.unknown_time") : entry.updated_at,
                              entry.message_count,
                              label.empty() ? tr("cmd.sessions.no_text")
                                             : lubancode::agent::TruncateUtf8Chars(label, 40))
                      << "\n";
        }
        std::cout << tr("cmd.sessions.archived_hint") << "\n";
        return;
    }
    const bool all = args == "all";
    if (!args.empty() && !all) {
        std::cout << tr("cmd.sessions.usage") << "\n";
        return;
    }
    const auto entries =
        lubancode::agent::ListSessions(sessions_dir, 20, all ? std::string() : CurrentDirUtf8());
    if (entries.empty()) {
        if (all) {
            std::cout << trf("cmd.sessions.none_all", sessions_dir) << "\n";
        } else {
            std::cout << tr("cmd.sessions.none_here") << "\n";
        }
        return;
    }
    std::cout << trf("cmd.sessions.header", entries.size(),
                      all ? tr("cmd.sessions.scope_all") : tr("cmd.sessions.scope_here"))
              << "\n";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        // 标题优先,没设过标题回退首句摘要。
        const std::string& label = !entry.title.empty() ? entry.title : entry.first_user_text;
        std::cout << "  " << (i + 1) << ") " << entry.id << "\n"
                   << trf("cmd.sessions.entry",
                           entry.started_at.empty() ? tr("cmd.sessions.unknown_time") : entry.started_at,
                           entry.message_count,
                           label.empty() ? tr("cmd.sessions.no_text")
                                          : lubancode::agent::TruncateUtf8Chars(label, 40))
                   << "\n";
        if (all) {
            std::cout << trf("cmd.sessions.dir_line",
                              entry.cwd.empty() ? tr("cmd.sessions.dir_unknown")
                                                 : lubancode::agent::AbbreviateUtf8Middle(entry.cwd, 48))
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

// SessionCatalog 的一页 -> picker 喂料(相对时间在这层算,协议层留稳定串)。
lubancode::cli::SessionPickerFeed MakePickerFeed(const std::vector<lubancode::agent::SessionSummary>& entries,
                                                 long long now_epoch) {
    lubancode::cli::SessionPickerFeed feed;
    feed.entries.reserve(entries.size());
    const std::string now_key = lubancode::agent::NowTimestamp();
    const long long now = now_epoch != 0 ? now_epoch : SessionTsToEpoch(now_key);
    for (const auto& entry : entries) {
        lubancode::cli::SessionPickerEntry row;
        row.id = entry.id;
        row.title = entry.title;
        row.preview = entry.first_user_text;
        row.cwd = entry.cwd;
        row.updated_ago = lubancode::cli::FormatSessionAgo(now, SessionTsToEpoch(entry.updated_at));
        row.created_ago = lubancode::cli::FormatSessionAgo(now, SessionTsToEpoch(entry.created_at));
        row.damaged = entry.health == lubancode::agent::SessionHealth::Damaged;
        // Ctrl+E 展开详情:摘要里现成的字段直转,不多读一盘。
        row.created_at = entry.created_at;
        row.updated_at = entry.updated_at;
        row.model = entry.model;
        row.message_count = entry.message_count;
        feed.entries.push_back(std::move(row));
    }
    feed.total = entries.size();
    feed.now_epoch = now;
    return feed;
}

// Ctrl+T 转录浮层的取数:整份读进来,取头尾各 max_half 行(大文件按需
// 读——只在浮层开了且选中 id 变了时被调一回,不是每键读盘)。事件行
// (compact/title/cwd/queue)不进转录,只摆消息行;用户/助手各按首块
// 文本拼一行(工具调用摆 "[工具] 名字"),像一份压缩的流水。
std::vector<std::string> MakeTranscriptExcerpt(const std::string& sessions_dir, const std::string& id,
                                               std::size_t max_half) {
    std::vector<std::string> lines;
    const std::string file_path = sessions_dir + "/" + id + ".jsonl";
    const auto content = lubancode::agent::ReadSessionFileBytes(file_path);
    if (!content.has_value()) {
        return lines;
    }
    std::istringstream iss(*content);
    std::string line;
    bool first = true;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (first) {
            first = false;
            continue;  // meta 行不进转录
        }
        const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (!j.is_object()) {
            continue;
        }
        if (j.contains("type")) {
            continue;  // 事件行(compact/title/cwd/queue)不进转录
        }
        const auto message = lubancode::agent::DeserializeSessionMessage(line);
        if (!message.has_value()) {
            continue;
        }
        std::string text;
        for (const auto& block : message->content) {
            if (const auto* tb = std::get_if<lubancode::api::TextBlock>(&block); tb != nullptr) {
                if (!tb->text.empty()) {
                    text = tb->text.substr(0, tb->text.find('\n'));
                    break;
                }
            }
            if (const auto* use = std::get_if<lubancode::api::ToolUseBlock>(&block); use != nullptr) {
                text = "[工具] " + use->name;
                break;
            }
            if (std::get_if<lubancode::api::ToolResultBlock>(&block) != nullptr) {
                text = "[工具结果]";
                break;
            }
        }
        if (text.empty()) {
            continue;
        }
        const char* who = message->role == lubancode::api::Role::User ? "user" : "assistant";
        std::istringstream text_stream(text);
        std::string first_text_line;
        std::getline(text_stream, first_text_line);
        lines.push_back(std::string("  ") + who + " · " + first_text_line);
    }
    // 大文件取头尾:全量超了就掐中间,首尾各 max_half 行,断口插一行省略号。
    if (lines.size() > max_half * 2) {
        std::vector<std::string> trimmed;
        trimmed.reserve(max_half * 2 + 1);
        trimmed.insert(trimmed.end(), lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(max_half));
        trimmed.push_back(std::string("  …(") + std::to_string(lines.size() - max_half * 2) + " 行省略)…");
        trimmed.insert(trimmed.end(), lines.end() - static_cast<std::ptrdiff_t>(max_half), lines.end());
        return trimmed;
    }
    return lines;
}

}  // namespace

std::optional<std::string> PromptResumeTarget(const std::string& sessions_dir,
                                              const lubancode::cli::Theme& theme) {
    if (sessions_dir.empty()) {
        std::cout << tr("session.no_home") << "\n";
        return std::nullopt;
    }
    if (!lubancode::platform::StdinIsInteractive() || !lubancode::platform::ProbeStdoutConsole().is_console) {
        std::cout << tr("cmd.resume.usage") << "\n";
        return std::nullopt;
    }

    // 打开时扫一回台账(指纹缓存);本目录与全部都空才说"没什么可恢复"。
    lubancode::agent::SessionCatalog catalog(sessions_dir);
    catalog.Scan();
    lubancode::agent::SessionQuery query;
    query.scope = lubancode::agent::SessionScope::Cwd;
    query.sort = lubancode::agent::SessionSort::Updated;
    query.cwd = CurrentDirUtf8();
    query.limit = 0;  // 面板自己管视口,数据一次给全
    if (catalog.Query(query).total == 0) {
        lubancode::agent::SessionQuery all_query = query;
        all_query.scope = lubancode::agent::SessionScope::All;
        if (catalog.Query(all_query).total == 0) {
            std::cout << tr("cmd.resume.none") << "\n";
            return std::nullopt;
        }
    }

    // 面板循环:面板里改 Filter/Sort 会把形状带出来,这里重查 catalog
    // (没动的场走指纹缓存,不重读)再进面板,选中项按 id 留住。
    lubancode::cli::SessionPickerScope scope = lubancode::cli::SessionPickerScope::Cwd;
    lubancode::cli::SessionPickerSort sort = lubancode::cli::SessionPickerSort::Updated;
    std::string keep_id;  // 换筛选前的选中项,重进面板时守住它
    for (;;) {
        query.scope = scope == lubancode::cli::SessionPickerScope::Cwd
                          ? lubancode::agent::SessionScope::Cwd
                          : lubancode::agent::SessionScope::All;
        query.sort = sort == lubancode::cli::SessionPickerSort::Updated
                         ? lubancode::agent::SessionSort::Updated
                         : lubancode::agent::SessionSort::Created;
        const auto page = catalog.Query(query);
        const auto feed = MakePickerFeed(page.entries, 0);
        // Ctrl+T 转录浮层:按需读盘(选中 id 变了面板才回调这一回)。
        lubancode::cli::SessionTranscriptProvider transcript = [&catalog](const std::string& id) {
            return MakeTranscriptExcerpt(catalog.sessions_dir(), id, kTranscriptHalfRows);
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
            std::cout << theme.stats << tr("cmd.resume.cancelled") << theme.reset << "\n";
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
bool ResumeSession(const std::string& target, const std::string& sessions_dir,
                    lubancode::agent::AgentLoop& loop, lubancode::agent::SessionStore& store,
                    std::size_t& persisted_count, lubancode::agent::SessionMeta& session_meta,
                    std::string& session_title, const std::string& wire_str, const std::string& current_model,
                    const lubancode::cli::Theme& theme, bool quiet_if_none,
                    lubancode::cli::WorktreeSession* worktree_session, int* compact_epoch_out,
                    const std::function<void(const std::vector<lubancode::agent::ArchivedQueueItem>&)>*
                        on_queue_restored,
                    const std::function<void(const std::optional<lubancode::agent::ModeEvent>&,
                                             const std::vector<lubancode::agent::PlanEvent>&,
                                             const std::optional<lubancode::agent::PlanReviewEvent>&)>*
                        on_mode_restored) {
    if (sessions_dir.empty()) {
        std::cout << tr("session.no_home") << "\n";
        return false;
    }
    const auto entries = lubancode::agent::ListSessions(sessions_dir, 20, CurrentDirUtf8());

    std::string id;
    std::string file_path;
    bool all_digits = !target.empty();
    for (const char c : target) {
        if (c < '0' || c > '9') {
            all_digits = false;
            break;
        }
    }
    if (target.empty()) {
        // --continue:本目录最近一场;一场都没有就按 quiet_if_none 处理。
        if (entries.empty()) {
            if (!quiet_if_none) {
                std::cout << tr("cmd.resume.none") << "\n";
            }
            return false;
        }
        id = entries.front().id;
        file_path = entries.front().file_path;
    } else if (all_digits) {
        std::size_t n = 0;
        try {
            n = static_cast<std::size_t>(std::stoul(target));
        } catch (...) {
            n = 0;
        }
        if (n < 1 || n > entries.size()) {
            std::cout << trf("cmd.resume.out_of_range", target, entries.size()) << "\n";
            return false;
        }
        id = entries[n - 1].id;
        file_path = entries[n - 1].file_path;
    } else {
        // 按 id 找:先在列表里对,不在(比 20 场更老)就直接拼路径试。
        for (const auto& entry : entries) {
            if (entry.id == target) {
                id = entry.id;
                file_path = entry.file_path;
                break;
            }
        }
        if (id.empty()) {
            id = target;
            file_path = sessions_dir + "/" + target + ".jsonl";
        }
    }

    const auto content = lubancode::agent::ReadSessionFileBytes(file_path);
    if (!content.has_value()) {
        std::cout << trf("cmd.resume.read_failed", file_path) << "\n";
        return false;
    }
    auto session = lubancode::agent::ParseSessionFile(*content);
    if (!session.has_value()) {
        std::cout << trf("cmd.resume.bad_meta", file_path) << "\n";
        return false;
    }

    loop.ReplaceHistory(session->messages);
    persisted_count = session->messages.size();
    if (!store.ResumeAt(file_path, id)) {
        std::cout << theme.error << trf("cmd.resume.takeover_failed", file_path) << theme.reset << "\n";
    }
    session_meta = session->meta;
    session_title = session->title;
    if (compact_epoch_out != nullptr) {
        *compact_epoch_out = session->compact_epoch;  // 压缩序号接着旧账数
    }

    const std::string restored_history = lubancode::cli::FormatRestoredHistory(
        session->all_messages, theme, lubancode::cli::DetectConsoleWidth().value_or(80),
        session->compact_positions);
    if (!restored_history.empty()) {
        std::cout << "\n" << theme.banner << trf("cmd.resume.history.header", id) << theme.reset << "\n\n"
                  << restored_history << theme.stats << tr("cmd.resume.history.end") << theme.reset << "\n\n";
    }

    if (session->compact_count > 0) {
        // 经过压缩的场子:恢复的是回放出来的有效态,不是全量流水。
        std::cout << trf("cmd.resume.restored_compact", id, session->messages.size(),
                          session->all_messages.size(), session->compact_count);
    } else {
        std::cout << trf("cmd.resume.restored", id, session->messages.size());
    }
    if (session->repaired > 0) {
        std::cout << trf("cmd.resume.repaired", session->repaired);
    }
    if (session->skipped_lines > 0) {
        std::cout << trf("cmd.resume.skipped", session->skipped_lines);
    }
    std::cout << "。\n";
    // 排队账重建(路径二):存档最后一条 queue 快照交还会话层;有货时给
    // 用户一行,别让人以为排过的话凭空蒸发。
    if (on_queue_restored != nullptr && *on_queue_restored) {
        (*on_queue_restored)(session->queued_messages);
        if (!session->queued_messages.empty()) {
            std::cout << theme.stats << trf("cmd.resume.queue_restored", session->queued_messages.size())
                      << theme.reset << "\n";
        }
    }
    // Plan 模式单:mode/plan/review 账交还会话层。老档没 mode 行给
    // nullopt(按 Default),会话层自己判;恢复 Plan 档时由它打一行提示。
    if (on_mode_restored != nullptr && *on_mode_restored) {
        const std::optional<lubancode::agent::ModeEvent> mode_event =
            session->last_mode_event.mode.empty()
                ? std::nullopt
                : std::optional<lubancode::agent::ModeEvent>(session->last_mode_event);
        (*on_mode_restored)(mode_event, session->plan_events, session->last_plan_review);
    }
    // context 记账:真实 usage 得等恢复后第一次请求才校准,这里先按字符
    // 粗估打一行,心里有数。
    std::cout << trf("cmd.resume.estimate", EstimateHistoryTokens(session->messages)) << "\n";
    if (!session->meta.model.empty() && session->meta.model != current_model) {
        std::cout << theme.stats << trf("cmd.resume.model_mismatch", session->meta.model, current_model)
                  << theme.reset << "\n";
    }
    if (!session->meta.wire.empty() && session->meta.wire != wire_str) {
        std::cout << theme.stats << trf("cmd.resume.wire_mismatch", session->meta.wire, wire_str) << theme.reset
                  << "\n";
    }

    // 会话跟 cwd 走(0.27.x):存档记录的 cwd 是一间房就验明正身搬回去。
    if (worktree_session != nullptr) {
        const std::string saved = session->meta.cwd;
        const std::string now = CurrentDirUtf8();
        if (!saved.empty() &&
            lubancode::agent::NormalizePathForCompare(saved) != lubancode::agent::NormalizePathForCompare(now)) {
            const std::filesystem::path saved_path(
                std::u8string(reinterpret_cast<const char8_t*>(saved.data()), saved.size()));
            std::error_code path_ec;
            if (!std::filesystem::exists(saved_path, path_ec)) {
                std::cout << theme.stats << trf("cmd.resume.worktree_gone", saved) << theme.reset << "\n";
            } else {
                const auto entered = worktree_session->EnterByPath(saved_path);
                if (entered.code == lubancode::cli::WorktreeResultCode::VerificationFailed) {
                    std::cout << theme.error << trf("cmd.resume.worktree_refused", saved, entered.detail)
                              << theme.reset << "\n";
                } else if (entered.code == lubancode::cli::WorktreeResultCode::Created) {
                    std::cout << theme.stats << trf("cmd.resume.worktree_back", saved) << theme.reset << "\n";
                }
            }
        }
    }
    return true;
}

// /export [路径]:当前会话导出 Markdown,默认写 sessions/<id>.md。
// 有存档文件就从文件读**全量流水**导出(压缩不丢内容,发生点插一行标注);
// 没有存档文件(没落过盘)退回导内存里这份历史。/title 设过的标题当大标题。
void HandleExportCommand(const std::string& args, const lubancode::agent::AgentLoop& loop,
                          const lubancode::agent::SessionStore& store, const std::string& sessions_dir,
                          const lubancode::agent::SessionMeta& session_meta, const std::string& session_title,
                          const lubancode::agent::ContextArtifactStore* artifact_store) {
    const auto& history = loop.History();
    if (history.empty()) {
        std::cout << tr("cmd.export.empty") << "\n";
        return;
    }
    const std::string id =
        !store.session_id().empty() ? store.session_id() : lubancode::agent::NowIdTimestamp() + "-export";
    std::string out_path = args;
    if (out_path.empty()) {
        if (sessions_dir.empty()) {
            std::cout << tr("cmd.export.need_path") << "\n";
            return;
        }
        out_path = sessions_dir + "/" + id + ".md";
    }

    // 全量优先:存档文件在,就按文件里的流水导(含压缩标注);读不动再退
    // 回内存这份(此时没有压缩位置可标)。
    std::string markdown;
    bool exported_from_file = false;
    if (!store.file_path().empty()) {
        if (const auto content = lubancode::agent::ReadSessionFileBytes(store.file_path());
            content.has_value()) {
            if (const auto session = lubancode::agent::ParseSessionFile(*content); session.has_value()) {
                const std::string& title = !session->title.empty() ? session->title : session_title;
                markdown = lubancode::agent::ExportSessionMarkdown(session->meta, session->all_messages, id,
                                                                     /*max_result_lines=*/30, title,
                                                                     session->compact_positions);
                exported_from_file = true;
            }
        }
    }
    if (!exported_from_file) {
        markdown = lubancode::agent::ExportSessionMarkdown(session_meta, history, id,
                                                             /*max_result_lines=*/30, session_title);
    }

    // artifact 附录(第二期,规格"原文不丢":"/export 必须说明哪些内容来自
    // artifact"):逐枚列 id/工具/字节数/sha 指纹与仓路径,导出的 Markdown
    // 里被折叠的工具结果都能按 id 对上真本。本导出走全量流水(全文都在),
    // 这份附录是"还能去哪核"的地图,不是内容本体。
    if (artifact_store != nullptr && artifact_store->active() && !artifact_store->refs().empty()) {
        markdown += "\n\n## 附:可追回 artifact\n\n";
        markdown += "| id | 工具 | 字节 | 行 | sha256(前12) | 真本 |\n";
        markdown += "|----|------|------|----|--------------|------|\n";
        for (const auto& ref : artifact_store->refs()) {
            markdown += "| " + ref.artifact_id + " | " + ref.tool_name + " | " + std::to_string(ref.bytes) +
                        " | " + std::to_string(ref.lines) + " | " + ref.sha256.substr(0, 12) + " | `" +
                        ref.blob_path + "` |\n";
        }
        markdown += "\n真本按 sha256 内容寻址存于 `" + artifact_store->root() + "`,hash 不合的 blob 会被隔离不供给。\n";
    }

    const std::filesystem::path path(
        std::u8string(reinterpret_cast<const char8_t*>(out_path.data()), out_path.size()));
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        std::cout << trf("cmd.export.write_failed", out_path) << "\n";
        return;
    }
    file << markdown;
    file.close();
    std::cout << trf("cmd.export.done", out_path) << "\n";
}

// ---------------------------------------------------------------------------
// 会话命令 handler:原样搬自会话主循环的 slash case,行为一字未改。
// ---------------------------------------------------------------------------

CommandFlow HandleClearCommand(SessionCommandState& state, const lubancode::config::Config& config,
                               const lubancode::cli::Theme& theme, bool spinner_enabled) {
    // 真控制台才清屏——ANSI 转义混进管道/重定向输出会污染脚本消费者,
    // spinner_enabled 就是通用的"是不是真控制台"信号。清完屏紧接着重打
    // 图标 + 横幅——回归修复:此前清屏后屏幕只剩"已清空对话历史"一句,
    // 连自己是谁、在哪个目录、什么模型都看不见了,用户反馈"清得太狠"。
    // 重打这两行,清屏后至少留得住这几条身份信息,不是一片空白。
    if (spinner_enabled) {
        ClearAndPrintBanner(config, theme);
    }
    // 后台子代理清场(0.28.x 面板规格):停掉全部任务、未送达的介入消息
    // 列给人看——新会话不该带着旧场子的代理与排着的话。
    if (state.on_agents_cleanup) {
        state.on_agents_cleanup();
    }
    state.rebuild_loop(false);
    // 存档跟着翻篇:旧文件留在磁盘上,新会话下一条消息另起一份新文件
    // (id 用新的时间戳)。标题属于旧场子,一并翻篇。
    state.store.Reset();
    state.start_ts = lubancode::agent::NowIdTimestamp();
    if (state.on_session_restarted) {
        state.on_session_restarted();  // project memory 的会话源跟着换新场
    }
    state.persisted_count = 0;
    state.store_broken = false;
    state.title.clear();
    state.title_pending = false;
    std::cout << tr("cmd.clear.done") << "\n";
    return CommandFlow::Continue;
}

CommandFlow HandleTitleCommand(SessionCommandState& state, const std::string& args,
                               const lubancode::cli::Theme& theme) {
    if (args.empty()) {
        std::cout << (state.title.empty() ? tr("cmd.title.none") : trf("cmd.title.current", state.title))
                  << "\n";
        return CommandFlow::Continue;
    }
    state.title = args;
    if (state.store.active() && !state.store_broken) {
        if (state.store.AppendTitleEvent(state.title)) {
            std::cout << trf("cmd.title.set", state.title) << "\n";
        } else {
            std::cout << theme.error << tr("cmd.title.write_failed") << theme.reset << "\n";
        }
    } else {
        // 还没建档(首条消息才落盘):先记着,建档成功后由落盘路径补写
        // 事件行。
        state.title_pending = true;
        std::cout << trf("cmd.title.set_pending", state.title) << "\n";
    }
    // 跨会话名册跟着改名(重名仍用短 peer_id 定人)。
    if (state.on_title_changed) {
        state.on_title_changed(state.title);
    }
    return CommandFlow::Continue;
}

CommandFlow HandleResumeCommand(SessionCommandState& state, const std::string& args,
                                const lubancode::cli::Theme& theme) {
    std::string target = args;
    if (target.empty()) {
        const auto selected = PromptResumeTarget(state.sessions_dir, theme);
        if (!selected.has_value()) {
            return CommandFlow::Continue;
        }
        target = *selected;
    }
    if (ResumeSession(target, state.sessions_dir, state.loop, state.store, state.persisted_count,
                      state.meta, state.title, state.wire_str, *state.current_model, theme,
                      /*quiet_if_none=*/false, state.worktree_session, &state.compact_epoch,
                      state.on_queue_restored ? &state.on_queue_restored : nullptr,
                      state.on_mode_restored ? &state.on_mode_restored : nullptr)) {
        state.store_broken = false;  // 换了场,存档失败的旧账翻篇
        state.title_pending = false;
        if (state.worktree_session != nullptr && state.worktree_session->active()) {
            // resume 把会话搬回了房:提示词与子代理 cwd 同步。
            if (state.sync_worktree_directory) {
                state.sync_worktree_directory();
            }
        }
        // hooks:恢复的历史开新账(SessionStart source=resume)。
        if (state.on_resumed) {
            state.on_resumed();
        }
    }
    return CommandFlow::Continue;
}

// ---------------------------------------------------------------------------
// 归档与永久删除(第四、五步):引用解析、确认屏、顶层命令与会话内命令。
// 搬与删全经 agent::SessionLifecycle,这里不直接碰 filesystem。
// ---------------------------------------------------------------------------

namespace {

// SessionCatalog 的摘要账 -> lifecycle 的候选账(标题从缓存补)。默认只列
// 活动会话;include_archived 连归档场一起列(unarchive 的目标恰是归档场)。
std::vector<lubancode::agent::SessionRefCandidate> MakeCandidates(const std::string& sessions_dir,
                                                                  bool include_archived = false) {
    lubancode::agent::SessionCatalog catalog(sessions_dir);
    catalog.Scan();
    std::vector<lubancode::agent::SessionRefCandidate> out;
    const auto collect = [&](lubancode::agent::SessionState state) {
        lubancode::agent::SessionQuery query;
        query.scope = lubancode::agent::SessionScope::All;
        query.state = state;
        query.limit = 0;
        const auto page = catalog.Query(query);
        out.reserve(out.size() + page.entries.size());
        for (const auto& entry : page.entries) {
            out.push_back(lubancode::agent::SessionRefCandidate{entry.id, entry.file_path, entry.title});
        }
    };
    collect(lubancode::agent::SessionState::Active);
    if (include_archived) {
        collect(lubancode::agent::SessionState::Archived);
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

bool ResolveSessionReference(const std::string& sessions_dir, const std::string& ref,
                             const std::function<std::string()>& stdin_line, std::string& out_id,
                             std::string& out_title, std::string& out_message, bool& ambiguous) {
    return ResolveSessionReference(sessions_dir, ref, stdin_line, /*include_archived=*/false, out_id,
                                   out_title, out_message, ambiguous);
}

bool ResolveSessionReference(const std::string& sessions_dir, const std::string& ref,
                             const std::function<std::string()>& stdin_line, bool include_archived,
                             std::string& out_id, std::string& out_title, std::string& out_message,
                             bool& ambiguous) {
    (void)stdin_line;
    ambiguous = false;
    out_id.clear();
    out_title.clear();
    out_message.clear();
    const auto candidates = MakeCandidates(sessions_dir, include_archived);
    const auto hits = lubancode::agent::ResolveSessionRef(candidates, ref, ambiguous);
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

int HandleSessionManagementCommand(const std::string& sessions_dir, int kind, const std::string& ref,
                                   bool force, const lubancode::cli::Theme& theme,
                                   const std::function<std::string()>& stdin_line) {
    (void)theme;
    if (sessions_dir.empty()) {
        std::cout << tr("session.no_home") << "\n";
        return 1;
    }
    const bool is_delete = kind == 2;
    const bool is_archive = kind == 0;
    if (ref.empty()) {
        std::cout << tr(is_delete ? "cmd.session.delete.usage" : "cmd.session.archive.usage") << "\n";
        return 1;
    }
    // 搬删经 runtime 侧的 SessionCommandService(第六步:typed command
    // 收口)——终端适配层只管确认屏与文案,不直接碰 lifecycle。
    lubancode::runtime::SessionCommandService service(sessions_dir);

    std::string id;
    std::string title;
    std::string message;
    bool ambiguous = false;
    // unarchive/delete 的目标可能在归档里(单子:删除只碰根或 archive
    // 里验明的那一份):候选连归档一起列。archive 只在活动会话里解
    // (已归档的重复归档无意义,幂等成功)。
    if (!ResolveSessionReference(sessions_dir, ref, stdin_line, !is_archive, id, title,
                                 message, ambiguous)) {
        std::cout << message << "\n";
        return 1;
    }

    if (is_delete) {
        // 摘要细节(确认屏要写标题/完整 id/cwd)。
        lubancode::agent::SessionCatalog catalog(sessions_dir);
        catalog.Scan();
        const lubancode::agent::SessionSummary* summary = catalog.Find(id);
        const std::string show_title = title.empty() && summary != nullptr ? summary->title : title;
        const std::string cwd = summary != nullptr ? summary->cwd : std::string();
        const std::string first_text = summary != nullptr ? summary->first_user_text : std::string();
        const std::string label = !show_title.empty()
                                      ? show_title
                                      : (!first_text.empty() ? first_text : tr("cmd.sessions.no_text"));
        // 确认屏:标题/完整 id/cwd/"永久删除"。缺省取消;EOF、空答皆取消。
        // --force 只给脚本:跳过确认(帮助里写明不可恢复)。
        bool confirmed = force;
        if (!confirmed) {
            std::cout << tr("cmd.session.delete.confirm_header") << "\n"
                      << trf("cmd.session.delete.confirm_title",
                             lubancode::agent::TruncateUtf8Chars(label, 60))
                      << "\n"
                      << trf("cmd.session.delete.confirm_id", id) << "\n"
                      << trf("cmd.session.delete.confirm_cwd",
                             cwd.empty() ? tr("cmd.sessions.dir_unknown")
                                         : lubancode::agent::AbbreviateUtf8Middle(cwd, 60))
                      << "\n"
                      << tr("cmd.session.delete.confirm_prompt");
            std::cout.flush();
            confirmed = ConfirmAnswer(ReadConfirmLine(stdin_line));
            if (!confirmed) {
                std::cout << tr("cmd.session.delete.cancelled") << "\n";
                return 1;
            }
        }
        // typed command:协议形状的 confirm 走 payload(确认策略归适配层,
        // 服务只认 confirm=true 才动手)。
        lubancode::runtime::ClientCommand command;
        command.kind = lubancode::runtime::ClientCommandKind::DeleteThread;
        command.thread_id = id;
        command.payload = {{"confirm", true}};
        const auto receipt = service.HandleCommand(command);
        if (!receipt.accepted) {
            std::cout << trf("cmd.session.delete.failed", id) << "\n";
            return 1;
        }
        std::cout << trf("cmd.session.delete.done", id) << "\n";
        return 0;
    }

    lubancode::runtime::ClientCommand command;
    command.kind = is_archive ? lubancode::runtime::ClientCommandKind::ArchiveThread
                              : lubancode::runtime::ClientCommandKind::UnarchiveThread;
    command.thread_id = id;
    const auto receipt = service.HandleCommand(command);
    if (!receipt.accepted) {
        std::cout << trf(is_archive ? "cmd.session.archive.failed" : "cmd.session.unarchive.failed", id)
                  << "\n";
        return 1;
    }
    std::cout << trf(is_archive ? "cmd.session.archive.done" : "cmd.session.unarchive.done", id) << "\n";
    return 0;
}

bool ArchiveCurrentSession(const std::string& sessions_dir, lubancode::agent::SessionStore& store,
                           const lubancode::cli::Theme& theme) {
    (void)theme;
    if (sessions_dir.empty()) {
        std::cout << tr("session.no_home") << "\n";
        return false;
    }
    if (!store.active() || store.session_id().empty()) {
        std::cout << tr("cmd.archive.not_active") << "\n";
        return false;
    }
    lubancode::runtime::SessionCommandService service(sessions_dir);
    // 活动句柄:搬之前先刷盘收柄(Windows sharing violation 的闸)。目标
    // 就是当前会话时,lifecycle 调这里的回调。
    service.SetActiveFile(store.file_path(), [&store](const std::string&) {
        store.Reset();  // ofstream close 自带 flush;RAII 收柄
        return true;
    });
    lubancode::runtime::ClientCommand command;
    command.kind = lubancode::runtime::ClientCommandKind::ArchiveThread;
    command.thread_id = store.session_id();
    const auto receipt = service.HandleCommand(command);
    if (!receipt.accepted) {
        // 搬失败:句柄已收但文件还在原地,账没坏;如实告诉人。
        std::cout << trf("cmd.session.archive.failed", store.session_id()) << "\n";
        return false;
    }
    std::cout << trf("cmd.session.archive.done", store.session_id()) << "\n";
    return true;
}

bool DeleteCurrentSession(const std::string& sessions_dir, lubancode::agent::SessionStore& store,
                          const lubancode::agent::SessionMeta& meta, const std::string& title,
                          const lubancode::cli::Theme& theme) {
    (void)theme;
    if (sessions_dir.empty()) {
        std::cout << tr("session.no_home") << "\n";
        return false;
    }
    if (!store.active() || store.session_id().empty()) {
        std::cout << tr("cmd.delete.not_active") << "\n";
        return false;
    }
    // 确认屏:标题/完整 id/cwd/"永久删除"。缺省取消;EOF、空答皆取消。
    const std::string label =
        !title.empty() ? title : (!meta.cwd.empty() ? meta.cwd : tr("cmd.sessions.no_text"));
    std::cout << tr("cmd.session.delete.confirm_header") << "\n"
              << trf("cmd.session.delete.confirm_title", lubancode::agent::TruncateUtf8Chars(label, 60))
              << "\n"
              << trf("cmd.session.delete.confirm_id", store.session_id()) << "\n"
              << trf("cmd.session.delete.confirm_cwd",
                     meta.cwd.empty() ? tr("cmd.sessions.dir_unknown")
                                      : lubancode::agent::AbbreviateUtf8Middle(meta.cwd, 60))
              << "\n"
              << tr("cmd.session.delete.confirm_prompt");
    std::cout.flush();
    if (!ConfirmAnswer(ReadConfirmLine(nullptr))) {
        std::cout << tr("cmd.session.delete.cancelled") << "\n";
        return false;
    }
    lubancode::runtime::SessionCommandService service(sessions_dir);
    service.SetActiveFile(store.file_path(), [&store](const std::string&) {
        store.Reset();
        return true;
    });
    lubancode::runtime::ClientCommand command;
    command.kind = lubancode::runtime::ClientCommandKind::DeleteThread;
    command.thread_id = store.session_id();
    command.payload = {{"confirm", true}};  // 确认屏已收过,这里带确认动手
    const auto receipt = service.HandleCommand(command);
    if (!receipt.accepted) {
        std::cout << trf("cmd.session.delete.failed", store.session_id()) << "\n";
        return false;
    }
    std::cout << trf("cmd.session.delete.done", store.session_id()) << "\n";
    return true;
}

}  // namespace lubancode::app

