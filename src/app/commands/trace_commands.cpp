// /trace 命令 presenter 实现(合同见 trace_commands.hpp)。函数体自
// interactive_session 的 DispatchSlashCommand Trace case 原文搬家(改道:
// hub/存档走 ctx、输出走 TerminalPort、switch-case 的 break 收成 return),
// 行为一字不差——注释一并随行。

#include "app/commands/trace_commands.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/tool_trace.hpp"
#include "cli/terminal_frame.hpp"  // frame::*(TUI 排版批 5b:/trace 渲染段)
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "platform/console.hpp"  // GetScreenInfo:整条 /trace 的框宽同一把尺
#include "runtime/tool_trace_hub.hpp"
#include "tools/path_utils.hpp"
#include "tools/session_utils.hpp"  // NowTimestamp(P0-6 自 sessions 迁来)

namespace lubancode::app {

namespace {

namespace frame = lubancode::cli::frame;

// ---- TUI 排版批 5b(/trace 全族)的公共小件(批 2 同款) --------------------
//
// 本文件文案是硬编码中文(不走 i18n 表),单子合同"不新增文案"在此读作:
// 既有句子原样进 frame,一字不添不改;表格列头用数据 schema 名(#/exec/
// tool/outcome/error/ms/rel),SentenceField 拆句内冒号。

int TraceFrameWidth() {
    if (const auto info = lubancode::platform::GetScreenInfo()) {
        return info->width;
    }
    return 0;
}

void EmitFrameLines(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        TermOut() << line << "\n";
    }
}

std::string TrimAscii(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

frame::Field SentenceField(const std::string& sentence,
                           frame::FieldAccent accent = frame::FieldAccent::None) {
    const std::size_t colon = sentence.find(':');
    if (colon == std::string::npos) {
        return frame::Field{"", sentence, accent};
    }
    return frame::Field{TrimAscii(sentence.substr(0, colon)), TrimAscii(sentence.substr(colon + 1)), accent};
}

void PrintTraceNotice(const lubancode::cli::Theme& theme, std::initializer_list<std::string> sentences,
                      frame::FieldAccent accent = frame::FieldAccent::None) {
    std::vector<frame::Field> fields;
    for (const std::string& sentence : sentences) {
        fields.push_back(SentenceField(sentence, accent));
    }
    EmitFrameLines(frame::RenderKeyValues({}, fields, theme, frame::Light(), TraceFrameWidth()));
}

// outcome 单元格的语义色:成功 pass;取消/未启动 skip;其余(工具错/未知/
// schema 拒/闸拒...)是明确失败,走 error 档(fail 不另立色,批 0 合同)。
frame::CellTone OutcomeTone(const lubancode::agent::ToolExecutionRecord& record) {
    using lubancode::agent::ToolOutcome;
    switch (record.outcome) {
        case ToolOutcome::Succeeded:
            return frame::CellTone::Pass;
        case ToolOutcome::CancelledBeforeStart:
            return frame::CellTone::Skip;
        default:
            return frame::CellTone::Fail;
    }
}

// 关系边连排(rel 列):parent/retry/blocked/compensates 与 corrupt 标注,
// 与旧 summary 行的附注同一套词,一字不改。
std::string RelationText(const lubancode::agent::ToolExecutionRecord& record) {
    std::ostringstream out;
    if (record.corrupt) {
        out << "[trace_corrupt] ";
    }
    if (!record.parent_execution_id.empty()) {
        out << "(parent " << record.parent_execution_id << ") ";
    }
    if (!record.retry_of.empty()) {
        out << "(retry of " << record.retry_of << ") ";
    }
    if (!record.blocked_by.empty()) {
        out << "(blocked by " << record.blocked_by << ") ";
    }
    if (!record.compensates.empty()) {
        out << "(compensates " << record.compensates << ") ";
    }
    std::string text = out.str();
    if (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
    return text;
}

// 枚账表格(批 5b):一行一枚 execution,短字段进列(#/exec/tool/outcome/
// error/ms),关系边进 rel 列——塞不下的长字段批 2 先例是另起明细表,这里
// 关系边本就是短串,同表可容。
void PrintExecutionTable(const lubancode::cli::Theme& theme, const std::string& title,
                         const std::vector<const lubancode::agent::ToolExecutionRecord*>& records) {
    std::vector<frame::TableColumn> columns;
    columns.push_back({"#"});
    columns.push_back({"exec"});
    columns.push_back({"tool"});
    columns.push_back({"outcome"});
    columns.push_back({"error"});
    columns.push_back({"ms", 0, /*align_right=*/true});
    columns.push_back({"rel"});
    std::vector<frame::TableRow> rows;
    for (const auto* record : records) {
        rows.push_back(frame::TableRow{
            {"#" + std::to_string(record->sequence_in_batch), record->execution_id, record->tool_name,
             lubancode::agent::ToString(record->outcome), record->error_code,
             std::to_string(record->duration_ms), RelationText(*record)},
            {frame::CellTone::Normal, frame::CellTone::Normal, frame::CellTone::Normal,
             OutcomeTone(*record), OutcomeTone(*record), frame::CellTone::Normal, frame::CellTone::Normal}});
    }
    EmitFrameLines(frame::RenderTable(title, columns, rows, theme, frame::Light(), TraceFrameWidth()));
}

}  // namespace

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
            PrintTraceNotice(theme, {"导出件会离开本机,一律脱敏,没有 --raw 档。"}, frame::FieldAccent::Error);
            return;
        }
        if (out_path.empty()) {
            PrintTraceNotice(theme, {"用法: /trace export <路径>"}, frame::FieldAccent::Error);
            return;
        }
        if (ctx.trace_hub == nullptr) {
            PrintTraceNotice(theme, {"本会话没有追踪 hub,没有可导出的追踪账。"}, frame::FieldAccent::Error);
            return;
        }
        // P0-6:旧 session 存档已删,导出吃 hub 的进程内最近账(有界 512
        // 枚);跨进程的持久真账在 trajectory Journal,事件侧折叠口属
        // trace 单后续波次,如实注明。
        const auto ledger = ctx.trace_hub->BuildRecentLedger();
        nlohmann::json bundle;
        bundle["schema"] = "tool_trace_export_v1";
        bundle["note_scope"] = "in-process recent ledger (bounded)";
        bundle["exportedAt"] = lubancode::tools::NowTimestamp();
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
            PrintTraceNotice(theme, {"导出文件打不开: " + out_path}, frame::FieldAccent::Error);
            return;
        }
        const std::string body = bundle.dump(2);
        out_file.write(body.data(), static_cast<std::streamsize>(body.size()));
        out_file.close();
        PrintTraceNotice(theme, {"已导出脱敏追踪账(" + std::to_string(ledger.executions().size()) +
                                 " 枚 execution): " + out_path});
        return;
    }
    if (args == "errors") {
        // 表格化(批 5b):取数从 ErrorLines() 的行串改为 ledger 同口径过滤
        //(Succeeded/CancelledBeforeStart 除外,与 hub 的过滤同一对枚举值),
        // 行集不变,只是一行一枚进列。
        std::vector<const lubancode::agent::ToolExecutionRecord*> failed;
        if (ctx.trace_hub != nullptr) {
            const auto ledger = ctx.trace_hub->BuildRecentLedger();
            for (const auto& record : ledger.executions()) {
                if (record.outcome == lubancode::agent::ToolOutcome::Succeeded ||
                    record.outcome == lubancode::agent::ToolOutcome::CancelledBeforeStart) {
                    continue;
                }
                failed.push_back(&record);
            }
        }
        if (failed.empty()) {
            PrintTraceNotice(theme, {"本会话没有明确失败或 unknown 的工具调用。"});
        } else {
            PrintExecutionTable(theme, "明确失败或 unknown 的工具调用", failed);
        }
        return;
    }
    const bool detail_query = args.rfind("toolu ", 0) == 0 || args.rfind("turn ", 0) == 0 ||
                              (!args.empty() && args != "errors" && args != "--raw" &&
                               args.find(' ') == std::string::npos);
    if (detail_query) {
        if (ctx.trace_hub != nullptr) {
            // P0-6:旧 session 存档已删,详细档吃 hub 的进程内最近账;跨进程
            // 持久真账在 trajectory Journal(事件侧折叠口属 trace 单后续波次)。
            const auto ledger = ctx.trace_hub->BuildRecentLedger();
            if (args.rfind("toolu ", 0) == 0) {
                const std::string id = args.substr(6);
                const auto records = ledger.FindByToolUse(id);
                if (!records.empty()) {
                    PrintExecutionTable(theme, "toolu " + id, records);
                }
            } else if (args.rfind("turn ", 0) == 0) {
                const std::string id = args.substr(5);
                std::vector<const lubancode::agent::ToolExecutionRecord*> records;
                for (const auto& record : ledger.executions()) {
                    if (record.turn_id == id) {
                        records.push_back(&record);
                    }
                }
                if (!records.empty()) {
                    PrintExecutionTable(theme, "turn " + id, records);
                }
            } else {
                const auto* record = ledger.FindByExecution(args);
                if (record != nullptr) {
                    std::vector<frame::Field> fields;
                    fields.push_back(frame::Field{"", lubancode::agent::FormatExecutionSummaryLine(*record, false)});
                    if (!record->error_code.empty()) {
                        fields.push_back(frame::Field{"error_code", record->error_code});
                    }
                    if (!record->source_instance.empty()) {
                        fields.push_back(frame::Field{"source", record->source_instance});
                    }
                    fields.push_back(frame::Field{
                        "recovery", lubancode::agent::ToString(record->Classify()),
                        OutcomeTone(*record) == frame::CellTone::Pass ? frame::FieldAccent::None
                                                                      : frame::FieldAccent::Error});
                    EmitFrameLines(frame::RenderKeyValues({}, fields, theme, frame::Light(), TraceFrameWidth()));
                } else {
                    PrintTraceNotice(theme, {"没有这枚 execution 的账: " + args});
                }
            }
        }
        return;
    }
    const std::string summary = ctx.trace_hub != nullptr ? ctx.trace_hub->LastBatchSummary() : std::string();
    if (summary.empty()) {
        PrintTraceNotice(theme, {"还没有工具调用的追踪账(本会话尚未跑过工具)。"});
    } else {
        PrintTraceNotice(theme, {summary});
    }
}

// 命令分派注册制(会话终章):/trace 的分派位——四档诊断的命令与排版全在
// 本文件,分派位只递 hub 与主题(HC-06:窄材料由绑定单元在装配期折好)。
CommandFlow HandleSlashTrace(const TraceCommandContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    HandleTraceCommand(ctx, parsed.args);
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
