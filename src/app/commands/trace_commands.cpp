// /trace 命令 presenter 实现(合同见 trace_commands.hpp)。函数体自
// interactive_session 的 DispatchSlashCommand Trace case 原文搬家(改道:
// hub/存档走 ctx、输出走 TerminalPort、switch-case 的 break 收成 return),
// 行为一字不差——注释一并随行。

#include "app/commands/trace_commands.hpp"

#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "agent/tool_trace.hpp"
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "platform/console.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "sessions/session_store.hpp"
#include "tools/path_utils.hpp"

namespace lubancode::app {

using lubancode::cli::TermOut;

void HandleTraceCommand(const TraceCommandContext& ctx, const std::string& args) {
    const lubancode::cli::Theme& theme = *ctx.theme;
    // 逐枚追踪单:只读诊断入口。batch(缺省)/errors 两档吃 hub 的
    // 进程内最近账;详细档(execution_id/toolu/turn)翻 session 存档
    // 的真本(重启后仍有账可查)。
    if (args.rfind("export", 0) == 0) {
        // /trace export <路径>(逐枚追踪单第 5 期):脱敏诊断包。
        // 内容 = meta + 全部 execution 的遮敏摘要(outcome/
        // error_code/来源/关系边/恢复结论/耗时/字节与 sha/
        // preview),不带 inline 原文、不带完整 stderr/env
        //(单子"隐私与脱敏")。默认遮敏;--raw 不放行——
        // 本会话虽是 TTY,导出件会离开本机,交互确认的
        // 语义没法带到文件上,一律脱敏(要比对的拿 preview
        // 与 sha 自己对)。
        std::string out_path = args.substr(6);
        while (!out_path.empty() && (out_path.front() == ' ' || out_path.front() == '\t')) {
            out_path.erase(out_path.begin());
        }
        if (out_path == "--raw" || out_path.rfind("--raw ", 0) == 0) {
            TermOut() << theme.error << "导出件会离开本机,一律脱敏,没有 --raw 档。" << theme.reset << "\n";
            return;
        }
        if (out_path.empty()) {
            TermOut() << theme.error << "用法: /trace export <路径>" << theme.reset << "\n";
            return;
        }
        if (ctx.session_store == nullptr || !ctx.session_store->active()) {
            TermOut() << theme.error << "本会话没有存档,没有可导出的追踪账。" << theme.reset << "\n";
            return;
        }
        const auto bytes = lubancode::sessions::ReadSessionFileBytes(ctx.session_store->file_path());
        if (!bytes.has_value()) {
            TermOut() << theme.error << "会话档读不到: " << ctx.session_store->file_path() << theme.reset << "\n";
            return;
        }
        const auto loaded = lubancode::sessions::ParseSessionFile(*bytes);
        if (!loaded.has_value()) {
            TermOut() << theme.error << "会话档解析失败。" << theme.reset << "\n";
            return;
        }
        const auto ledger = lubancode::runtime::ToolTraceHub::BuildLedger(loaded->tool_trace_events);
        nlohmann::json bundle;
        bundle["schema"] = "tool_trace_export_v1";
        bundle["session"] = ctx.session_store->session_id();
        bundle["exportedAt"] = lubancode::sessions::NowTimestamp();
        bundle["note"] = "脱敏诊断包:只有遮敏摘要,无正文原文";
        nlohmann::json items = nlohmann::json::array();
        for (const auto& record : ledger.executions()) {
            nlohmann::json item;
            item["executionId"] = record.execution_id;
            item["toolUseId"] = record.tool_use_id;
            item["toolName"] = record.tool_name;
            item["turnId"] = record.turn_id;
            item["batchId"] = record.batch_id;
            item["sequenceInBatch"] = record.sequence_in_batch;
            item["source"] = lubancode::agent::ToString(record.source_kind);
            item["sourceInstance"] = record.source_instance;
            item["parentExecutionId"] = record.parent_execution_id;
            item["retryOf"] = record.retry_of;
            item["blockedBy"] = record.blocked_by;
            item["compensates"] = record.compensates;
            item["outcome"] = lubancode::agent::ToString(record.outcome);
            item["errorCode"] = record.error_code;
            item["durationMs"] = record.duration_ms;
            item["recovery"] = lubancode::agent::ToString(record.Classify());
            item["corrupt"] = record.corrupt;
            item["resultBytes"] = record.result_ref.bytes;
            item["resultSha256"] = record.result_ref.sha256;
            item["resultPreview"] = record.result_ref.preview;  // BuildTracePreview 已过 RedactSecrets
            if (!record.result_ref.artifact_id.empty()) {
                item["resultArtifactId"] = record.result_ref.artifact_id;
            }
            items.push_back(std::move(item));
        }
        bundle["executions"] = std::move(items);
        bundle["verificationCount"] = ledger.verifications().size();
        bundle["corruptCount"] = ledger.corrupt_count();

        std::ofstream out_file(lubancode::tools::Utf8ToPath(out_path), std::ios::binary | std::ios::trunc);
        if (!out_file.is_open()) {
            TermOut() << theme.error << "导出文件打不开: " << out_path << theme.reset << "\n";
            return;
        }
        const std::string body = bundle.dump(2);
        out_file.write(body.data(), static_cast<std::streamsize>(body.size()));
        out_file.close();
        TermOut() << theme.stats << "已导出脱敏追踪账(" << ledger.executions().size()
                  << " 枚 execution): " << out_path << theme.reset << "\n";
        return;
    }
    if (args == "errors") {
        const auto lines = ctx.trace_hub != nullptr ? ctx.trace_hub->ErrorLines() : std::vector<std::string>{};
        if (lines.empty()) {
            TermOut() << theme.stats << "本会话没有明确失败或 unknown 的工具调用。" << theme.reset << "\n";
        } else {
            for (const std::string& line : lines) {
                TermOut() << theme.stats << line << theme.reset << "\n";
            }
        }
        return;
    }
    const bool detail_query = args.rfind("toolu ", 0) == 0 || args.rfind("turn ", 0) == 0 ||
                              (!args.empty() && args != "errors" && args != "--raw" &&
                               args.find(' ') == std::string::npos);
    if (detail_query) {
        if (ctx.session_store != nullptr && ctx.session_store->active()) {
            const auto bytes = lubancode::sessions::ReadSessionFileBytes(ctx.session_store->file_path());
            if (bytes.has_value()) {
                const auto loaded = lubancode::sessions::ParseSessionFile(*bytes);
                if (loaded.has_value()) {
                    const auto ledger =
                        lubancode::runtime::ToolTraceHub::BuildLedger(loaded->tool_trace_events);
                    if (args.rfind("toolu ", 0) == 0) {
                        const std::string id = args.substr(6);
                        for (const auto* record : ledger.FindByToolUse(id)) {
                            TermOut() << theme.stats
                                      << lubancode::agent::FormatExecutionSummaryLine(*record, false)
                                      << theme.reset << "\n";
                        }
                    } else if (args.rfind("turn ", 0) == 0) {
                        const std::string id = args.substr(5);
                        for (const auto& record : ledger.executions()) {
                            if (record.turn_id == id) {
                                TermOut() << theme.stats
                                          << lubancode::agent::FormatExecutionSummaryLine(record, false)
                                          << theme.reset << "\n";
                            }
                        }
                    } else {
                        const auto* record = ledger.FindByExecution(args);
                        if (record != nullptr) {
                            TermOut() << theme.stats
                                      << lubancode::agent::FormatExecutionSummaryLine(*record, false)
                                      << theme.reset << "\n";
                            if (!record->error_code.empty()) {
                                TermOut() << theme.stats << "  error_code: " << record->error_code
                                          << theme.reset << "\n";
                            }
                            if (!record->source_instance.empty()) {
                                TermOut() << theme.stats << "  source: " << record->source_instance
                                          << theme.reset << "\n";
                            }
                            TermOut() << theme.stats << "  recovery: "
                                      << lubancode::agent::ToString(record->Classify()) << theme.reset
                                      << "\n";
                        } else {
                            TermOut() << theme.stats << "没有这枚 execution 的账: " << args
                                      << theme.reset << "\n";
                        }
                    }
                }
            }
        }
        return;
    }
    const std::string summary = ctx.trace_hub != nullptr ? ctx.trace_hub->LastBatchSummary() : std::string();
    if (summary.empty()) {
        TermOut() << theme.stats << "还没有工具调用的追踪账(本会话尚未跑过工具)。" << theme.reset << "\n";
    } else {
        TermOut() << theme.stats << summary << theme.reset;
    }
}

}  // namespace lubancode::app
