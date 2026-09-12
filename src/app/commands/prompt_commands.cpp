// prompt_commands.hpp 的实现:魂/法/提示词命令的函数体。
#include "app/commands/prompt_commands.hpp"

#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)
#include "app/commands/prompt_audit_commands.hpp"  // /prompt audit 的分派壳递这(A3)
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

using lubancode::cli::TermOut;
using lubancode::cli::TermErr;

#include <iostream>

#include "agent/prompts.hpp"
#include "config/config.hpp"

#include <memory>
#include <optional>
#include <string>

#include "cli/console_input.hpp"
#include "cli/i18n.hpp"
#include "cli/theme.hpp"
#include "config/prompt_files.hpp"

namespace lubancode::app {


using lubancode::cli::tr;
using lubancode::cli::trf;

// ---------------------------------------------------------------------------
// 魂法分家(0.16.x):/soul //prompt 的执行逻辑。纯拼接/剥注释在
// agent/prompts.hpp,文件生成/还原/扫描在 config/prompt_files,这里只做
// "接命令、找文件、打印、问一句"这层壳。
// ---------------------------------------------------------------------------

// 数一段 UTF-8 文本有多少个字符(码点)——/prompt 报"字数"用,字节数对
// 中文没意义。
std::size_t CountUtf8Chars(const std::string& text) {
    std::size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

// 按魂名读内容(原始全文,注释留给注入时剥):"off" -> 空;空串/"default"
// -> SOUL.md;别的名字 -> souls/<名字>.md。默认 SOUL.md 走专用读口:缺失、
// 打不开或 UTF-8 已坏都降成空魂,不拦启动。warn 为真时打一行说明。启动读
// 一次、/soul 切换即时重读,都走这一个函数。
std::string LoadSoulContentByName(const std::string& name, bool warn) {
    if (name == "off") {
        return std::string();
    }
    const auto luban_dir = lubancode::config::HomeLubancodeDir();
    if (!luban_dir.has_value()) {
        return std::string();
    }
    const bool default_soul = name.empty() || name == "default";
    const std::string path = default_soul ? lubancode::config::SoulFilePath(*luban_dir)
                                          : lubancode::config::SoulPathByName(*luban_dir, name);
    const auto content = default_soul ? lubancode::config::ReadSoulFile(*luban_dir)
                                      : lubancode::config::ReadTextFileIfExists(path);
    if (!content.has_value()) {
        if (warn) {
            TermOut() << trf("soul.unavailable", path) << "\n";
        }
        return std::string();
    }
    return *content;
}

// ---------------------------------------------------------------------------
// Soul 会话冻结单 P0:HandleSoulCommand 的新语义(单内 §5.2 表,用户定案)。
// 双状态:session_soul 是本会话快照(锁定前草稿/锁定后只读),configured_*
// 是持久默认值的内存映像。保存顺序一律"先校验、保存成功,再动账"——
// 失败报错回滚,不宣称成功,不留内存/磁盘各用一份的状态。
// ---------------------------------------------------------------------------

namespace {

// 把魂选择落进配置文件。没有配置文件不算失败(内存默认账照改,新会话
// 反正读不到那份配置,调用方打一行"仅本会话");落盘失败给错误串。
std::string PersistSoulChoice(const std::optional<std::string>& config_file_path, const std::string& choice) {
    if (!config_file_path.has_value()) {
        return std::string();
    }
    const auto updated = lubancode::config::UpdateSoulInConfigFile(*config_file_path, choice);
    if (!updated.has_value()) {
        return updated.error();
    }
    return std::string();
}

// 保存回执:内容规范化后相同提示"内容未变",不记虚假 pending/revision;
// 其余按锁定状态走 §5.2 的两条原文提示。
void PrintSoulSaveReceipt(const lubancode::runtime::SessionSoulSnapshot& session_soul, bool changed) {
    if (!changed) {
        TermOut() << tr("cmd.soul.unchanged") << "\n";
        return;
    }
    if (session_soul.locked) {
        TermOut() << trf("cmd.soul.locked.save_hint", session_soul.name) << "\n";
    } else {
        TermOut() << tr("cmd.soul.draft.save_hint") << "\n";
    }
}

// 保存成功后的账目善手:configured 两笔(正文/选择)换新;锁定前草稿跟着
// 默认走,锁定后快照一字不动(§5.2 表右列——多次修改以后一次成功保存
// 为准,每次成功即覆盖)。
void CommitSoulDefault(lubancode::runtime::SessionSoulSnapshot& session_soul,
                       const std::shared_ptr<std::string>& configured_content, std::string& configured_name,
                       std::string new_name, std::string new_content) {
    const bool default_changed =
        new_name != configured_name || lubancode::runtime::SessionSoulContentHash(new_content) !=
                                           lubancode::runtime::SessionSoulContentHash(*configured_content);
    *configured_content = std::move(new_content);
    configured_name = std::move(new_name);
    if (session_soul.locked) {
        PrintSoulSaveReceipt(session_soul, default_changed);
        return;
    }
    const bool changed = lubancode::runtime::UpdateSessionSoulDraft(session_soul, configured_name,
                                                                    *configured_content,
                                                                    "config:" + configured_name);
    PrintSoulSaveReceipt(session_soul, changed);
}

}  // namespace

void HandleSoulCommand(const std::string& args, lubancode::runtime::SessionSoulSnapshot& session_soul,
                       const std::shared_ptr<std::string>& configured_content, std::string& configured_name,
                       const std::optional<std::string>& config_file_path) {
    const auto luban_dir = lubancode::config::HomeLubancodeDir();
    if (!luban_dir.has_value()) {
        TermOut() << tr("cmd.soul.no_home") << "\n";
        return;
    }

    if (args.empty()) {
        // 裸敲:本会话快照 + 锁定状态 + 下个新会话默认值,差异标清
        //(pending 只是展示状态,不是自动应用队列)。
        const std::vector<std::string> souls = lubancode::config::ListSouls(*luban_dir);
        TermOut() << trf("cmd.soul.current", session_soul.name) << "\n";
        TermOut() << (session_soul.locked ? tr("cmd.soul.status.locked") : tr("cmd.soul.status.unlocked"))
                  << "\n";
        const std::string visible = lubancode::agent::StripPromptComments(session_soul.content);
        if (visible.empty()) {
            TermOut() << tr("cmd.soul.empty_note") << "\n";
        } else {
            TermOut() << visible << "\n";
        }
        TermOut() << trf("cmd.soul.default_header", configured_name) << "\n";
        const bool pending =
            configured_name != session_soul.name ||
            lubancode::runtime::SessionSoulContentHash(*configured_content) != session_soul.content_hash;
        TermOut() << (pending ? trf("cmd.soul.pending_note", configured_name) : tr("cmd.soul.pending_same"))
                  << "\n";
        TermOut() << tr("cmd.soul.available_header") << "\n";
        TermOut() << tr("cmd.soul.default_item") << "\n";
        for (const auto& name : souls) {
            TermOut() << "  - " << name << "\n";
        }
        TermOut() << "\n" << tr("cmd.soul.usage") << "\n";
        return;
    }

    if (args == "off") {
        // off 只改选择,不删正文(clear 才清空正文,§5.2)。先保存,成功才动账。
        const std::string persist_error = PersistSoulChoice(config_file_path, "off");
        if (!persist_error.empty()) {
            TermOut() << trf("cmd.write_config.failed", persist_error) << "\n";
            return;
        }
        if (!config_file_path.has_value()) {
            TermOut() << tr("cmd.session_only") << "\n";
        }
        CommitSoulDefault(session_soul, configured_content, configured_name, "off", std::string());
        return;
    }

    if (args == "default") {
        // 解析默认文件,保存选择并更新草稿;锁定后只更新以后采用的默认。
        const std::string content = LoadSoulContentByName("default", /*warn=*/true);
        const std::string persist_error = PersistSoulChoice(config_file_path, "default");
        if (!persist_error.empty()) {
            TermOut() << trf("cmd.write_config.failed", persist_error) << "\n";
            return;
        }
        if (!config_file_path.has_value()) {
            TermOut() << tr("cmd.session_only") << "\n";
        }
        CommitSoulDefault(session_soul, configured_content, configured_name, "default", content);
        return;
    }

    if (args == "clear") {
        // clear 清空默认正文(SOUL.md 还原空魂),快照不动——锁定后的当前
        // 快照仍可从会话档的 blob 恢复(§5.2)。写失败整体回滚:两本账、
        // 磁盘文件都不动,不宣称成功。
        const auto cleared = lubancode::config::ClearSoulFile(*luban_dir);
        if (!cleared.has_value()) {
            TermOut() << trf("cmd.soul.write_failed", cleared.error()) << "\n";
            return;
        }
        // 配置选择归位 default:这一笔失败不回滚 SOUL.md(文件内容已是
        // 默认空魂,configured 内存账照文件同步,选择项失败明说)。
        const std::string persist_error = PersistSoulChoice(config_file_path, "default");
        if (!persist_error.empty()) {
            TermOut() << trf("cmd.soul.default_config_failed", persist_error) << "\n";
        }
        CommitSoulDefault(session_soul, configured_content, configured_name, "default",
                          lubancode::config::DefaultSoulFileContent());
        return;
    }

    // 具名魂:参数恰好命中 souls/<名字>.md 时仍是选魂(兼容旧用法)。
    const std::string path = lubancode::config::SoulPathByName(*luban_dir, args);
    const auto content = lubancode::config::ReadTextFileIfExists(path);
    if (content.has_value()) {
        const std::string persist_error = PersistSoulChoice(config_file_path, args);
        if (!persist_error.empty()) {
            TermOut() << trf("cmd.write_config.failed", persist_error) << "\n";
            return;
        }
        if (!config_file_path.has_value()) {
            TermOut() << tr("cmd.session_only") << "\n";
        }
        CommitSoulDefault(session_soul, configured_content, configured_name, args, *content);
        return;
    }

    // 其余一律当正文:写进 SOUL.md(默认正文与正文选择同时归位 default)。
    // 先写文件,成功才动账。
    const auto written = lubancode::config::WriteSoulFile(*luban_dir, args);
    if (!written.has_value()) {
        TermOut() << trf("cmd.soul.write_failed", written.error()) << "\n";
        return;
    }
    const std::string persist_error = PersistSoulChoice(config_file_path, "default");
    if (!persist_error.empty()) {
        TermOut() << trf("cmd.soul.default_config_failed", persist_error) << "\n";
    }
    CommitSoulDefault(session_soul, configured_content, configured_name, "default", args);
}

// /prompt 命令:裸敲显示当前法(人格段)的来源和字数,外加各提示词模块
// 的来源统计(用户文件/内置,0.21.x 运行时化);/prompt reset 带二次确认,
// 把 system_prompt.md 还原成内置默认(旧文件挪成 .bak)。
// persona 是本会话实际在用的人格段(空串 = 内置默认);law_source 是启动时
// 算好的来源说明(CLI 参数/文件/内置);prompts_dir 是用户模块目录
// (~/.lubancode/prompts,找不到主目录时空串)。
void HandlePromptCommand(const std::string& args, const std::string& law_source, const std::string& persona,
                          const std::string& prompts_dir) {
    if (args.empty()) {
        const std::string effective =
            persona.empty() ? lubancode::agent::AssembledCorePersona(prompts_dir) : persona;
        TermOut() << trf("cmd.prompt.info", law_source, CountUtf8Chars(effective)) << "\n";
        if (!prompts_dir.empty()) {
            const auto sources = lubancode::agent::PromptModuleSources(prompts_dir);
            std::size_t modified_count = 0;
            for (const auto& source : sources) {
                if (source.from_user_file && source.differs_from_embedded) {
                    ++modified_count;
                }
            }
            TermOut() << trf("cmd.prompt.modules_header", prompts_dir, modified_count, sources.size()) << "\n";
            for (const auto& source : sources) {
                const char* tag = !source.from_user_file          ? "cmd.prompt.module_builtin"
                                  : source.differs_from_embedded ? "cmd.prompt.module_user_modified"
                                                                  : "cmd.prompt.module_user_same";
                TermOut() << "  - " << source.rel_path << "  [" << tr(tag) << "]\n";
            }
        }
        return;
    }
    if (args != "reset") {
        TermOut() << tr("cmd.prompt.usage") << "\n";
        return;
    }

    const std::optional<std::string> answer = lubancode::cli::ReadLine(tr("cmd.prompt.confirm"));
    if (!answer.has_value() || (*answer != "y" && *answer != "Y")) {
        TermOut() << tr("cmd.prompt.cancelled") << "\n";
        return;
    }
    const auto luban_dir = lubancode::config::HomeLubancodeDir();
    if (!luban_dir.has_value()) {
        TermOut() << tr("cmd.prompt.no_home") << "\n";
        return;
    }
    const auto reset_result =
        lubancode::config::ResetSystemPromptFile(*luban_dir, lubancode::agent::DefaultPersona());
    if (!reset_result.has_value()) {
        TermOut() << trf("cmd.prompt.reset_failed", reset_result.error()) << "\n";
        return;
    }
    TermOut() << trf("cmd.prompt.reset_done", lubancode::config::SystemPromptFilePath(*luban_dir));
    if (!reset_result->empty()) {
        TermOut() << trf("cmd.prompt.old_file", *reset_result);
    }
    TermOut() << "。\n" << tr("cmd.prompt.reset_tail") << "\n";
}

// ---------------------------------------------------------------------------
// 命令分派注册制(会话终章):魂/法域的分派位。
// ---------------------------------------------------------------------------

CommandFlow HandleSlashSoul(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    // Soul 会话冻结单 P0:命令只改会话快照与 configured 默认账;皮上的
    // 同步经 SyncAgentRequestPolicy 走锁定闸——已锁定的会话不会换魂。
    if (ctx.soul_session != nullptr) {
        HandleSoulCommand(parsed.args, *ctx.soul_session, ctx.current_soul, *ctx.current_soul_name,
                          *ctx.config_file_path);
    }
    ctx.sync_request_policy();
    return CommandFlow::Continue;
}

CommandFlow HandleSlashPrompt(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    // Token 账本单 A3:/prompt audit … 递给审计件;裸敲与 reset 行为不动。
    if (parsed.args.rfind("audit", 0) == 0 &&
        (parsed.args.size() == 5 || parsed.args[5] == ' ' || parsed.args[5] == '\t')) {
        std::string audit_args = parsed.args.size() > 5 ? parsed.args.substr(5) : std::string();
        return HandleSlashPromptAudit(ctx, audit_args);
    }
    HandlePromptCommand(parsed.args, ctx.opts->law_source, *ctx.persona, *ctx.prompts_dir);
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
