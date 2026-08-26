#include "app/commands/hook_commands.hpp"
#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

using lubancode::cli::TermOut;
using lubancode::cli::TermErr;

#include <iostream>
#include <sstream>
#include <vector>

#include "app/hook_runtime.hpp"
#include "platform/text_encoding.hpp"

namespace lubancode::app {

namespace {

using lubancode::hooks::HookDefinition;
using lubancode::hooks::HookDispatcher;
using lubancode::hooks::HookRunRecord;

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

void PrintDefinitionList(HookDispatcher& dispatcher) {
    const auto& defs = dispatcher.definitions();
    if (defs.empty()) {
        TermOut() << "没有装载任何 hooks。配置写在 <目录>/.lubancode/config.json 的 hooks 段"
                     "(schema 2 用事件名键,如 PreToolUse;旧 pre_tool 等四类仍受支持)。\n";
        return;
    }
    TermOut() << "已装载 " << defs.size() << " 条 hook 定义(user 与项目配置相加;项目级须先信任才执行):\n";
    for (const auto& def : defs) {
        TermOut() << "  #" << def.id << " [" << std::string(lubancode::hooks::ToString(def.event)) << "]"
                  << (def.legacy ? "[legacy]" : "") << "\n"
                  << "      命令    : " << lubancode::hooks::HookCommandDisplay(def.handler) << "\n"
                  << "      来源    : " << def.source_label << "\n"
                  << "      matcher : " << (def.matcher.empty() || def.matcher == "*" ? "*" : def.matcher)
                  << (def.regex ? "(regex)" : "") << "\n"
                  << "      状态    : " << TrustStateText(def) << "\n"
                  << "      hash    : " << def.definition_hash_short
                  << (def.deduped ? "(与同事件同命令定义去重,不执行)" : "") << "\n"
                  << "      执行    : " << (def.handler.async ? "async(本期未启用执行)" : "同步")
                  << " 超时 " << (def.handler.timeout_ms / 1000) << "s"
                  << " 失败策略 " << def.handler.failure_policy << "\n"
                  << "      最近    : " << OutcomeText(dispatcher.LastRecordFor(def.id)) << "\n";
    }
    TermOut() << "动作:/hooks trust <#id> 审查后信任当前 hash;/hooks untrust <#id> 撤信;"
                 "/hooks disable|enable <#id> 禁用/启用;/hooks runs 看运行记录。\n"
                 "命令或参数一改,hash 即变,项目级须重审。\n";
}

void PrintRunRecords(HookDispatcher& dispatcher, int limit) {
    const std::vector<HookRunRecord> records = dispatcher.RecentRecords(static_cast<std::size_t>(limit));
    if (records.empty()) {
        TermOut() << "还没有任何 hook 运行记录。\n";
        return;
    }
    TermOut() << "最近 " << records.size() << " 条 hook 运行记录(新在前):\n";
    for (const auto& record : records) {
        TermOut() << "  #" << record.definition_id << " [" << record.event_name << "] " << record.outcome
                  << " 退出码 " << record.exit_code << " 耗时 " << record.duration_ms << "ms"
                  << " 来自 " << record.source_label << "\n";
        // stderr 首段单列一行:解码口径标清(utf-8/cp936/unknown),超上限带
        // 截断标志;编码未定时那行本身就是原始字节摘要,不是替换符。
        if (!record.stderr_head.empty()) {
            std::string head = record.stderr_head;
            if (!record.stderr_encoding.empty()) {
                head = "(" + record.stderr_encoding + ") " + head;
            }
            if (record.stderr_truncated) {
                head += " …(截断)";
            }
            TermOut() << "      stderr: " << head << "\n";
        }
        if (!record.detail.empty()) {
            std::string detail = record.detail;
            if (detail.size() > 160) {
                detail.resize(lubancode::platform::Utf8PrefixBoundary(detail, 160));
                detail += "…";
            }
            TermOut() << "      " << detail << "\n";
        }
    }
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
        TermOut() << "hooks 运行时未初始化(异常路径),本命令不可用。\n";
        return;
    }
    // 安全点:先把后台子代理投递的记录归并进来,列表与流水看到的才是全账。
    for (const std::string& notice : AdoptBackgroundHookRecordNotices()) {
        TermOut() << "[hooks] " << notice << "\n";
    }
    if (dispatcher->Empty()) {
        PrintDefinitionList(*dispatcher);
        return;
    }

    std::istringstream stream(args);
    std::string action;
    stream >> action;
    std::string id_text;
    stream >> id_text;

    if (action.empty() || action == "list") {
        PrintDefinitionList(*dispatcher);
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
        PrintRunRecords(*dispatcher, limit);
        return;
    }
    if (action == "trust" || action == "untrust" || action == "disable" || action == "enable") {
        int id = 0;
        if (!ParseId(id_text, id)) {
            TermOut() << "用法:/hooks " << action << " <#id>(id 见 /hooks 列表,如 #3 就写 #3)\n";
            return;
        }
        const HookDefinition* def = dispatcher->FindDefinition(id);
        if (def == nullptr) {
            TermOut() << "没有 #" << id << " 这条定义,先 /hooks 看清单。\n";
            return;
        }
        if (action == "trust") {
            if (dispatcher->TrustDefinition(id)) {
                TermOut() << "#" << id << " 已信任当前 hash(" << def->definition_hash_short << "),即时生效。\n";
            }
            return;
        }
        if (action == "untrust") {
            if (dispatcher->UntrustDefinition(id)) {
                TermOut() << "#" << id << " 已撤信;项目级定义下次起跳过,直到重新 trust。\n";
            }
            return;
        }
        if (action == "disable") {
            if (!dispatcher->SetDefinitionDisabled(id, true)) {
                TermOut() << "#" << id << " 是 managed 策略钩子,普通用户不能禁用。\n";
            } else {
                TermOut() << "#" << id << " 已禁用。\n";
            }
            return;
        }
        if (dispatcher->SetDefinitionDisabled(id, false)) {
            TermOut() << "#" << id << " 已重新启用。\n";
        }
        return;
    }
    TermOut() << "不认得的子命令: " << action
              << "\n可用:/hooks(列表)/hooks runs [N]/hooks trust|untrust|disable|enable <#id>\n";
    (void)theme;
}

// 命令分派注册制(会话终章):/hooks 的分派位。
CommandFlow HandleSlashHooks(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    HandleHooksCommand(parsed.args, lubancode::app::HookRuntime(), *ctx.theme);
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
