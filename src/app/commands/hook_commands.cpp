#include "app/commands/hook_commands.hpp"
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

using lubancode::cli::TermOut;
using lubancode::cli::TermErr;

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>

#include "app/hook_runtime.hpp"
#include "cli/terminal_frame.hpp"  // frame::*(TUI 排版批 2:/hooks 渲染段)
#include "platform/console.hpp"    // GetScreenInfo:整条 /hooks 的框宽同一把尺
#include "platform/text_encoding.hpp"

namespace lubancode::app {

namespace {

namespace frame = lubancode::cli::frame;

using lubancode::hooks::HookDefinition;
using lubancode::hooks::HookDispatcher;
using lubancode::hooks::HookRunRecord;

// ---- TUI 排版批 2(/hooks 全族)的公共小件 ----------------------------------
//
// 渲染段只调 cli::frame::* 三助手(批 0 基件,约定见 docs/development/
// tui_style.md)。本文件的历史文案是硬编码中文(不走 i18n 表),单子合同
// "不新增文案"在此读作:既有句子原样进 frame,一字不添不改;表头与列名
// 用数据 schema 名(id/event/matcher/...),与批 1 裁量同一条。表格化后
// 句内冒号按 SentenceField 拆两列(拆的是既有文案,同批 1)。

int HooksFrameWidth() {
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

void PrintNotice(const lubancode::cli::Theme& theme, std::initializer_list<std::string> sentences,
                 frame::FieldAccent accent = frame::FieldAccent::None) {
    std::vector<frame::Field> fields;
    for (const std::string& sentence : sentences) {
        fields.push_back(SentenceField(sentence, accent));
    }
    EmitFrameLines(frame::RenderKeyValues({}, fields, theme, frame::Light(), HooksFrameWidth()));
}

std::string TrustStateText(const HookDefinition& def) {
    if (def.disabled) {
        return "已禁用";
    }
    switch (def.source_kind) {
        case lubancode::hooks::HookSourceKind::Managed:
            return "managed(策略信任)";
        case lubancode::hooks::HookSourceKind::Project:
            return def.trusted ? "已信任" : "待审查(未信任,已跳过)";
        default:
            return "用户级(免审查)";
    }
}

// state 列的语义色(单子批 2):信任链通(managed/用户级/已信任)走 pass
// 档;待审查与已禁用都被跳过,走 skip 档——error 档留给 outcome 列的
// failure/timeout 等真失败。
frame::CellTone TrustStateTone(const HookDefinition& def) {
    if (def.disabled) {
        return frame::CellTone::Skip;
    }
    if (def.source_kind == lubancode::hooks::HookSourceKind::Project && !def.trusted) {
        return frame::CellTone::Skip;
    }
    return frame::CellTone::Pass;
}

// outcome 列的语义色(单子批 2):ok=pass;blocked 与 skipped_*=业务性
// 跳过,走 skip;failure/timeout/spawn_failed/schema_error 是真失败,走
// error 档(fail 不另立色,批 0 合同)。
frame::CellTone OutcomeTone(const std::string& outcome) {
    if (outcome == "ok") {
        return frame::CellTone::Pass;
    }
    if (outcome == "blocked" || outcome.rfind("skipped", 0) == 0) {
        return frame::CellTone::Skip;
    }
    if (!outcome.empty()) {
        return frame::CellTone::Fail;
    }
    return frame::CellTone::Normal;
}

std::string OutcomeText(const HookRunRecord* record) {
    if (record == nullptr) {
        return "尚未运行";
    }
    std::ostringstream out;
    out << record->outcome << " 退出码 " << record->exit_code << " 耗时 " << record->duration_ms << "ms";
    if (!record->detail.empty()) {
        std::string detail = record->detail;
        if (detail.size() > 120) {
            detail.resize(lubancode::platform::Utf8PrefixBoundary(detail, 120));
            detail += "…";
        }
        out << " | " << detail;
    }
    return out.str();
}

std::string ExecText(const HookDefinition& def) {
    std::ostringstream out;
    out << (def.handler.async ? "async(本期未启用执行)" : "同步") << " 超时 " << (def.handler.timeout_ms / 1000)
        << "s 失败策略 " << def.handler.failure_policy;
    return out.str();
}

void PrintDefinitionList(const lubancode::cli::Theme& theme, HookDispatcher& dispatcher) {
    const auto& defs = dispatcher.definitions();
    if (defs.empty()) {
        PrintNotice(theme, {"没有装载任何 hooks。配置写在 <目录>/.lubancode/config.json 的 hooks 段"
                            "(schema 2 用事件名键,如 PreToolUse;旧 pre_tool 等四类仍受支持)。"});
        return;
    }
    // 主表:一 hook 一行,短字段(id/event/matcher/state/hash/最近一次结
    // 果)。state 列上 pass/skip 语义色;legacy 与 deduped 标注并进对应列
    // 的值(与旧输出的连排一字不差)。
    std::vector<frame::TableColumn> columns;
    columns.push_back({"id"});
    columns.push_back({"event"});
    columns.push_back({"matcher"});
    columns.push_back({"state"});
    columns.push_back({"hash"});
    columns.push_back({"last"});
    std::vector<frame::TableRow> rows;
    for (const auto& def : defs) {
        rows.push_back(frame::TableRow{
            {"#" + std::to_string(def.id),
             std::string(lubancode::hooks::ToString(def.event)) + (def.legacy ? "[legacy]" : ""),
             (def.matcher.empty() || def.matcher == "*" ? "*" : def.matcher) + (def.regex ? "(regex)" : ""),
             TrustStateText(def),
             def.definition_hash_short + (def.deduped ? "(与同事件同命令定义去重,不执行)" : ""),
             OutcomeText(dispatcher.LastRecordFor(def.id))},
            {frame::CellTone::Normal, frame::CellTone::Normal, frame::CellTone::Normal, TrustStateTone(def),
             frame::CellTone::Normal, frame::CellTone::Normal}});
    }
    EmitFrameLines(frame::RenderTable(
        "已装载 " + std::to_string(defs.size()) +
            " 条 hook 定义(user 与项目配置相加;项目级须先信任才执行)",
        columns, rows, theme, frame::Light(), HooksFrameWidth()));
    // 明细表:命令/来源/执行是长字段,另起一张表——塞主表会被列帽截断,
    // 而命令全文正是 trust 审查要看的材料(批 1"附注防截断"同一条裁量)。
    std::vector<frame::TableColumn> detail_columns;
    detail_columns.push_back({"id"});
    detail_columns.push_back({"command"});
    detail_columns.push_back({"source"});
    detail_columns.push_back({"exec"});
    std::vector<frame::TableRow> detail_rows;
    for (const auto& def : defs) {
        detail_rows.push_back(frame::TableRow{{"#" + std::to_string(def.id),
                                               lubancode::hooks::HookCommandDisplay(def.handler),
                                               def.source_label, ExecText(def)}});
    }
    EmitFrameLines(
        frame::RenderTable({}, detail_columns, detail_rows, theme, frame::Light(), HooksFrameWidth()));
    // 动作提示进键值对框("动作:"按 SentenceField 拆列)。
    PrintNotice(theme, {"动作:/hooks trust <#id> 审查后信任当前 hash;/hooks untrust <#id> 撤信;"
                        "/hooks disable|enable <#id> 禁用/启用;/hooks runs 看运行记录。",
                        "命令或参数一改,hash 即变,项目级须重审。"});
}

void PrintRunRecords(const lubancode::cli::Theme& theme, HookDispatcher& dispatcher, int limit) {
    const std::vector<HookRunRecord> records = dispatcher.RecentRecords(static_cast<std::size_t>(limit));
    if (records.empty()) {
        PrintNotice(theme, {"还没有任何 hook 运行记录。"});
        return;
    }
    // 主表:一记录一行。exit/ms 是数值列右对齐;outcome 列上 pass/skip/
    // error 三档语义色(单子批 2 验收点)。
    std::vector<frame::TableColumn> columns;
    columns.push_back({"id"});
    columns.push_back({"event"});
    columns.push_back({"outcome"});
    columns.push_back({"exit", 0, /*align_right=*/true});
    columns.push_back({"ms", 0, /*align_right=*/true});
    columns.push_back({"source"});
    std::vector<frame::TableRow> rows;
    for (const auto& record : records) {
        rows.push_back(frame::TableRow{{"#" + std::to_string(record.definition_id), record.event_name,
                                        record.outcome, std::to_string(record.exit_code),
                                        std::to_string(record.duration_ms), record.source_label},
                                       {frame::CellTone::Normal, frame::CellTone::Normal,
                                        OutcomeTone(record.outcome)}});
    }
    EmitFrameLines(frame::RenderTable("最近 " + std::to_string(records.size()) + " 条 hook 运行记录(新在前)",
                                      columns, rows, theme, frame::Light(), HooksFrameWidth()));
    // 附注:stderr 首段与 detail 是长文本,列表行挂 "#id" 标签对回主表行
    //(解码口径与截断标注原样保留;detail 的 160 字节帽照旧)。
    std::vector<frame::ListRow> notes;
    for (const auto& record : records) {
        const std::string id_label = "#" + std::to_string(record.definition_id);
        if (!record.stderr_head.empty()) {
            std::string head = record.stderr_head;
            if (!record.stderr_encoding.empty()) {
                head = "(" + record.stderr_encoding + ") " + head;
            }
            if (record.stderr_truncated) {
                head += " …(截断)";
            }
            notes.push_back(frame::ListRow{id_label, "stderr: " + std::move(head), {}, frame::Bullet::None});
        }
        if (!record.detail.empty()) {
            std::string detail = record.detail;
            if (detail.size() > 160) {
                detail.resize(lubancode::platform::Utf8PrefixBoundary(detail, 160));
                detail += "…";
            }
            notes.push_back(frame::ListRow{id_label, std::move(detail), {}, frame::Bullet::None});
        }
    }
    EmitFrameLines(frame::RenderList({}, notes, theme, frame::Light(), HooksFrameWidth()));
}

bool ParseId(const std::string& text, int& out) {
    if (text.empty() || text[0] != '#') {
        return false;
    }
    try {
        out = std::stoi(text.substr(1));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

void HandleHooksCommand(const std::string& args, lubancode::hooks::HookDispatcher* dispatcher,
                         const lubancode::cli::Theme& theme) {
    if (dispatcher == nullptr) {
        PrintNotice(theme, {"hooks 运行时未初始化(异常路径),本命令不可用。"}, frame::FieldAccent::Error);
        return;
    }
    // 安全点:先把后台子代理投递的记录归并进来,列表与流水看到的才是全账。
    for (const std::string& notice : AdoptBackgroundHookRecordNotices()) {
        TermOut() << "[hooks] " << notice << "\n";
    }
    if (dispatcher->Empty()) {
        PrintDefinitionList(theme, *dispatcher);
        return;
    }

    std::istringstream stream(args);
    std::string action;
    stream >> action;
    std::string id_text;
    stream >> id_text;

    if (action.empty() || action == "list") {
        PrintDefinitionList(theme, *dispatcher);
        return;
    }
    if (action == "runs") {
        int limit = 20;
        if (!id_text.empty()) {
            try {
                limit = std::stoi(id_text);
                if (limit < 1) {
                    limit = 20;
                }
                if (limit > 100) {
                    limit = 100;
                }
            } catch (const std::exception&) {
            }
        }
        PrintRunRecords(theme, *dispatcher, limit);
        return;
    }
    if (action == "trust" || action == "untrust" || action == "disable" || action == "enable") {
        int id = 0;
        if (!ParseId(id_text, id)) {
            PrintNotice(theme, {"用法:/hooks " + action + " <#id>(id 见 /hooks 列表,如 #3 就写 #3)"});
            return;
        }
        const HookDefinition* def = dispatcher->FindDefinition(id);
        if (def == nullptr) {
            PrintNotice(theme, {"没有 #" + std::to_string(id) + " 这条定义,先 /hooks 看清单。"},
                        frame::FieldAccent::Error);
            return;
        }
        if (action == "trust") {
            if (dispatcher->TrustDefinition(id)) {
                PrintNotice(theme, {"#" + std::to_string(id) + " 已信任当前 hash(" + def->definition_hash_short +
                                    "),即时生效。"});
            }
            return;
        }
        if (action == "untrust") {
            if (dispatcher->UntrustDefinition(id)) {
                PrintNotice(theme,
                            {"#" + std::to_string(id) + " 已撤信;项目级定义下次起跳过,直到重新 trust。"});
            }
            return;
        }
        if (action == "disable") {
            if (!dispatcher->SetDefinitionDisabled(id, true)) {
                PrintNotice(theme, {"#" + std::to_string(id) + " 是 managed 策略钩子,普通用户不能禁用。"},
                            frame::FieldAccent::Error);
            } else {
                PrintNotice(theme, {"#" + std::to_string(id) + " 已禁用。"});
            }
            return;
        }
        if (dispatcher->SetDefinitionDisabled(id, false)) {
            PrintNotice(theme, {"#" + std::to_string(id) + " 已重新启用。"});
        }
        return;
    }
    PrintNotice(theme,
                {"不认得的子命令: " + action, "可用:/hooks(列表)/hooks runs [N]/hooks trust|untrust|disable|enable <#id>"},
                frame::FieldAccent::Error);
}

// 命令分派注册制(会话终章):/hooks 的分派位(HC-06:窄材料,dispatcher
// 执行时现取,与旧路同源)。
CommandFlow HandleSlashHooks(const HookCommandContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    HandleHooksCommand(parsed.args, lubancode::app::HookRuntime(), *ctx.theme);
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
