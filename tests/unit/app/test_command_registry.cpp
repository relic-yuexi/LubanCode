// 命令分派注册制(会话终章)的对账钉子:旧 interactive_session 大 switch
// 的 47 案,逐案在 SlashCommandTable 里留名。这份测试把"行为逐一照旧"
// 折成可数的账:
//   1. 表上行数与枚举全集一致(旧 switch 47 案 + 后续各单新增案,如今
//      57:自定义 Agent 单阶段 1 添 /agents、/agent 两案,Package 单
//      阶段 1 添 /package 一案,自进化闭环阶段 1 添 /evolve 一案,
//      AGENTS.md 作用域单 P1 添 /instructions 一案,Token 账本单 A2 添
//      /usage 一案;
//      55:多渠道单阶段 2 添 /channels、/channel 两案;
//      56:端云协同可观测单 T1 添 /telemetry 一案(只读状态面);
//      57:Token 账本单 A5 添 /insights 一案(跨会话洞察报告);
//      58:ContextWindow 交互面板单添 /context-window 一案(同屏调窗口
//         与思考强度的面板,本会话生效);
//   2. 枚举无重复、无遗漏(死案 Image/NotSlash 也留名,handler 为空);
//   3. 活案(有 handler)在 cli 词汇表都有主名,展示规则与帮助面
//      (cli::AllSlashCommands,同出词汇表)对得上——已知差异如实记:
//      /effort 是 /think 的别名(帮助面有、分派面归 think),/hooks 的
//      展示修复已随 HC-05 行为提交落账,/trace 仍显式不展示(帮助面
//      旧缺口,展示修复不在 HC-05 单内);
//   4. 死案的分派兜底(Continue)与旧 switch 的 break 同语义。
#include <doctest/doctest.h>

#include <set>
#include <string>
#include <vector>

#include "app/commands/command_registry.hpp"
#include "cli/slash_commands.hpp"
#include "workflow/catalog.hpp"

namespace {

// 枚举全集(旧 switch 47 案的案序 + 各单新增案;Agents/Agent 是自定义
// Agent 单阶段 1 添的,Package 是 Package 单阶段 1 添的,Evolve 是自进化
// 闭环阶段 1 添的,Instructions 是 AGENTS.md 作用域单 P1 添的,Usage 是
// Token 账本单 A2 添的,Insights 是 Token 账本单 A5 添的)。
const std::vector<lubancode::cli::SlashCommand>& AllCommandEnums() {
    static const std::vector<lubancode::cli::SlashCommand> all = {
        lubancode::cli::SlashCommand::Image,      lubancode::cli::SlashCommand::Help,
        lubancode::cli::SlashCommand::Model,      lubancode::cli::SlashCommand::Provider,
        lubancode::cli::SlashCommand::Config,     lubancode::cli::SlashCommand::Update,
        lubancode::cli::SlashCommand::Init,       lubancode::cli::SlashCommand::Language,
        lubancode::cli::SlashCommand::Worktree,   lubancode::cli::SlashCommand::Clear,
        lubancode::cli::SlashCommand::Context,    lubancode::cli::SlashCommand::Usage,
        lubancode::cli::SlashCommand::Insights,
        lubancode::cli::SlashCommand::ContextWindow,
        lubancode::cli::SlashCommand::Compact,
        lubancode::cli::SlashCommand::Think,      lubancode::cli::SlashCommand::Skills,
        lubancode::cli::SlashCommand::Skill,      lubancode::cli::SlashCommand::Mcp,
        lubancode::cli::SlashCommand::Lsp,        lubancode::cli::SlashCommand::Todos,
        lubancode::cli::SlashCommand::Plugins,    lubancode::cli::SlashCommand::Plugin,
        lubancode::cli::SlashCommand::Tools,      lubancode::cli::SlashCommand::Hooks,
        lubancode::cli::SlashCommand::Background, lubancode::cli::SlashCommand::Keymap,
        lubancode::cli::SlashCommand::Plan,       lubancode::cli::SlashCommand::Package,
        lubancode::cli::SlashCommand::Evolve,     lubancode::cli::SlashCommand::Trace,
        lubancode::cli::SlashCommand::Doctor,     lubancode::cli::SlashCommand::Goal,
        lubancode::cli::SlashCommand::Loop,       lubancode::cli::SlashCommand::Memory,
        lubancode::cli::SlashCommand::Record,     lubancode::cli::SlashCommand::Sessions,
        lubancode::cli::SlashCommand::Archive,    lubancode::cli::SlashCommand::Delete,
        lubancode::cli::SlashCommand::Resume,     lubancode::cli::SlashCommand::Export,
        lubancode::cli::SlashCommand::Copy,       lubancode::cli::SlashCommand::Title,
        lubancode::cli::SlashCommand::Soul,       lubancode::cli::SlashCommand::Prompt,
        lubancode::cli::SlashCommand::Peers,      lubancode::cli::SlashCommand::Send,
        lubancode::cli::SlashCommand::Peerperm,   lubancode::cli::SlashCommand::Workflow,
        lubancode::cli::SlashCommand::Agents,     lubancode::cli::SlashCommand::Agent,
        lubancode::cli::SlashCommand::Instructions,
        lubancode::cli::SlashCommand::Channels,   lubancode::cli::SlashCommand::Channel,
        lubancode::cli::SlashCommand::Exit,       lubancode::cli::SlashCommand::Unknown,
        lubancode::cli::SlashCommand::NotSlash,
    };
    return all;
}

}  // namespace

TEST_CASE("命令注册表:58 案齐整,枚举可对") {
    const std::vector<lubancode::app::SlashCommandSpec>& table = lubancode::app::SlashCommandTable();
    REQUIRE(table.size() == 58);

    SUBCASE("枚举逐一在表,无重复") {
        std::set<int> seen;
        for (const lubancode::app::SlashCommandSpec& spec : table) {
            CHECK_MESSAGE(seen.insert(static_cast<int>(spec.command)).second,
                          std::to_string(static_cast<int>(spec.command)));
        }
        CHECK(seen.size() == table.size());
        // 旧 switch 的每一案都能在表上查到。
        for (const lubancode::cli::SlashCommand command : AllCommandEnums()) {
            bool found = false;
            for (const lubancode::app::SlashCommandSpec& spec : table) {
                if (spec.command == command) {
                    found = true;
                    break;
                }
            }
            CHECK_MESSAGE(found, "旧 switch 的案子在注册表上没有留名");
        }
    }

    SUBCASE("死案只有 Image/NotSlash,handler 为空") {
        int dead = 0;
        for (const lubancode::app::SlashCommandSpec& spec : table) {
            if (spec.handler == nullptr) {
                ++dead;
                const bool is_dead_case = spec.command == lubancode::cli::SlashCommand::Image ||
                                          spec.command == lubancode::cli::SlashCommand::NotSlash;
                CHECK_MESSAGE(is_dead_case, "意外的死案");
            }
        }
        CHECK(dead == 2);
    }
}

TEST_CASE("命令注册表:活案名字与帮助面对账") {
    const std::vector<lubancode::app::SlashCommandSpec>& table = lubancode::app::SlashCommandTable();
    // 帮助面(cli::AllSlashCommands,Tab 补全与 /help 同源,均出词汇表)。
    std::set<std::string> help_names;
    for (const lubancode::cli::SlashCommandInfo& info : lubancode::cli::AllSlashCommands()) {
        help_names.insert(info.name);
    }
    // 正向:活案(有 handler)在词汇表都有主名,desc_key 定展示——非空的
    // 名字必在帮助面,空的必不在。已知差异如实记:/unknown 是兜底案,不是
    // 用户词汇;/hooks 展示修复已落账(desc_key 非空),/trace 仍显式
    // 不展示(帮助面旧缺口,展示修复不在 HC-05 单内)。
    for (const lubancode::app::SlashCommandSpec& spec : table) {
        if (spec.handler == nullptr) {
            continue;  // 死案
        }
        const lubancode::cli::SlashCommandDescriptor* descriptor =
            lubancode::cli::FindSlashCommandDescriptor(spec.command);
        REQUIRE_MESSAGE(descriptor != nullptr, "活案在词汇表上没有主名");
        const std::string full_name = std::string("/") + descriptor->word;
        if (descriptor->desc_key == nullptr) {
            CHECK_MESSAGE(help_names.count(full_name) == 0, full_name);
            continue;  // 不展示的主命令(现状:/trace)
        }
        CHECK_MESSAGE(help_names.count(full_name) == 1, full_name);
    }
    // 反向:帮助面的每个名字都落得到词汇表行,且对应枚举在分派面有案。
    // 例外如实记:/image 是死案(正戏在图片附件路),/effort 是 /think 的
    // 别名行(解析层折给 Think,分派面不单列)。
    for (const std::string& name : help_names) {
        const lubancode::cli::SlashCommandDescriptor* listed = nullptr;
        for (const lubancode::cli::SlashCommandDescriptor& descriptor :
             lubancode::cli::SlashCommandDescriptors()) {
            if (name == std::string("/") + descriptor.word && descriptor.desc_key != nullptr) {
                listed = &descriptor;
                break;
            }
        }
        REQUIRE_MESSAGE(listed != nullptr, name);
        if (listed->command == lubancode::cli::SlashCommand::Image) {
            continue;
        }
        bool found = false;
        for (const lubancode::app::SlashCommandSpec& spec : table) {
            if (spec.command == listed->command && spec.handler != nullptr) {
                found = true;
                break;
            }
        }
        CHECK_MESSAGE(found, name);
    }
}

TEST_CASE("命令注册表:死案与查无的兜底同旧 switch") {
    lubancode::app::SlashDispatchContext ctx{};  // 全空材料:死案不该摸它
    // Image:旧 case 是 break(Continue);表上 handler 为空,路由兜底 Continue。
    const lubancode::cli::ParsedSlashCommand image = lubancode::cli::ParseSlashCommand("/image foo.png");
    CHECK(lubancode::app::DispatchSessionSlashCommand(ctx, image) == lubancode::app::CommandFlow::Continue);
    // NotSlash:上一层已分流,进不来;兜底 Continue。
    const lubancode::cli::ParsedSlashCommand plain = lubancode::cli::ParseSlashCommand("hello");
    CHECK(plain.command == lubancode::cli::SlashCommand::NotSlash);
    CHECK(lubancode::app::DispatchSessionSlashCommand(ctx, plain) == lubancode::app::CommandFlow::Continue);
    // /exit 是纯路由案:空材料也该原样回 Exit。
    const lubancode::cli::ParsedSlashCommand exit_cmd = lubancode::cli::ParseSlashCommand("/exit");
    CHECK(lubancode::app::DispatchSessionSlashCommand(ctx, exit_cmd) == lubancode::app::CommandFlow::Exit);
}

// ---------------------------------------------------------------------------
// Workflow alias 的内建冲突检查口径(HC-05 行为提交):保留词吃全量词汇表
// (主名+别名+隐藏词),不再复用展示名单。workflow_commands.cpp 的
// BuiltinSlashWords 是文件内部件,这里按它的同一口径(AllSlashReservedWords)
// 喂 DetectAliasConflicts,钉死"alias=hooks、alias=quit 被判内建冲突"。
// ---------------------------------------------------------------------------
TEST_CASE("Workflow alias 冲突: 保留词全量口径,/hooks 与隐藏别名 /quit 都判冲突") {
    lubancode::workflow::Catalog catalog;
    for (const char* alias : {"hooks", "quit", "sansheng-liubu"}) {
        lubancode::workflow::CatalogEntry entry;
        entry.definition.id = std::string("wf-") + alias;
        entry.definition.alias = alias;
        entry.definition.enabled = true;
        catalog.entries.push_back(entry);
    }
    lubancode::workflow::DetectAliasConflicts(catalog, {}, lubancode::cli::AllSlashReservedWords());
    // /hooks:能执行的主命令,曾被展示名单漏掉——全量口径下必禁用直呼。
    CHECK(catalog.disabled_aliases.count("hooks") == 1);
    CHECK(catalog.FindByAlias("hooks") == nullptr);
    // /quit:/exit 的隐藏别名,内建照样压住,须报冲突。
    CHECK(catalog.disabled_aliases.count("quit") == 1);
    CHECK(catalog.FindByAlias("quit") == nullptr);
    // 不撞内建的 alias 照旧直呼无阻。
    CHECK(catalog.disabled_aliases.count("sansheng-liubu") == 0);
    CHECK(catalog.FindByAlias("sansheng-liubu") != nullptr);
    bool hooks_builtin_conflict = false;
    for (const auto& conflict : catalog.conflicts) {
        if (conflict.kind == "builtin" && conflict.alias == "hooks") {
            hooks_builtin_conflict = true;
        }
    }
    CHECK(hooks_builtin_conflict);
}
