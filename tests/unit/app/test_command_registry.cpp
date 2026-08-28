// 命令分派注册制(会话终章)的对账钉子:旧 interactive_session 大 switch
// 的 47 案,逐案在 SlashCommandTable 里留名。这份测试把"行为逐一照旧"
// 折成可数的账:
//   1. 表上行数与枚举全集一致(旧 switch 47 案 + 后续各单新增案,如今
//      49:自定义 Agent 单阶段 1 添 /agents、/agent 两案);
//   2. 枚举无重复、无遗漏(死案 Image/NotSlash 也留名,handler 为空);
//   3. 活案(有 handler)的名字与 cli::AllSlashCommands 的帮助面逐一对应
//      ——已知差异如实记:/effort 是 /think 的别名(帮助面有、分派面归
//      think),/hooks 与 /trace 分派面有、帮助面没有(帮助面的旧缺口,
//      本单不动);
//   4. 死案的分派兜底(Continue)与旧 switch 的 break 同语义。
#include <doctest/doctest.h>

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "app/commands/command_registry.hpp"
#include "cli/slash_commands.hpp"

namespace {

// 枚举全集(旧 switch 47 案的案序 + 各单新增案;Agents/Agent 是自定义
// Agent 单阶段 1 添的)。
const std::vector<lubancode::cli::SlashCommand>& AllCommandEnums() {
    static const std::vector<lubancode::cli::SlashCommand> all = {
        lubancode::cli::SlashCommand::Image,      lubancode::cli::SlashCommand::Help,
        lubancode::cli::SlashCommand::Model,      lubancode::cli::SlashCommand::Provider,
        lubancode::cli::SlashCommand::Config,     lubancode::cli::SlashCommand::Update,
        lubancode::cli::SlashCommand::Init,       lubancode::cli::SlashCommand::Language,
        lubancode::cli::SlashCommand::Worktree,   lubancode::cli::SlashCommand::Clear,
        lubancode::cli::SlashCommand::Context,    lubancode::cli::SlashCommand::Compact,
        lubancode::cli::SlashCommand::Think,      lubancode::cli::SlashCommand::Skills,
        lubancode::cli::SlashCommand::Skill,      lubancode::cli::SlashCommand::Mcp,
        lubancode::cli::SlashCommand::Lsp,        lubancode::cli::SlashCommand::Todos,
        lubancode::cli::SlashCommand::Plugins,    lubancode::cli::SlashCommand::Plugin,
        lubancode::cli::SlashCommand::Tools,      lubancode::cli::SlashCommand::Hooks,
        lubancode::cli::SlashCommand::Background, lubancode::cli::SlashCommand::Keymap,
        lubancode::cli::SlashCommand::Plan,       lubancode::cli::SlashCommand::Trace,
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
        lubancode::cli::SlashCommand::Exit,       lubancode::cli::SlashCommand::Unknown,
        lubancode::cli::SlashCommand::NotSlash,
    };
    return all;
}

}  // namespace

TEST_CASE("命令注册表:49 案齐整,枚举可对") {
    const std::vector<lubancode::app::SlashCommandSpec>& table = lubancode::app::SlashCommandTable();
    REQUIRE(table.size() == 49);

    SUBCASE("枚举逐一在表,无重复") {
        std::set<int> seen;
        for (const lubancode::app::SlashCommandSpec& spec : table) {
            const int value = static_cast<int>(spec.command);
            CHECK_MESSAGE(seen.insert(value).second, spec.name);
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
                CHECK_MESSAGE(is_dead_case, spec.name);
            }
        }
        CHECK(dead == 2);
    }
}

TEST_CASE("命令注册表:活案名字与帮助面对账") {
    const std::vector<lubancode::app::SlashCommandSpec>& table = lubancode::app::SlashCommandTable();
    // 帮助面(cli::AllSlashCommands,Tab 补全与 /help 同源)。
    std::set<std::string> help_names;
    for (const lubancode::cli::SlashCommandInfo& info : lubancode::cli::AllSlashCommands()) {
        help_names.insert(info.name);
    }
    // 已知差异(帮助面 ↔ 分派面):
    //   /effort 是 /think 的别名——帮助面单列,分派面归 think 一案;
    //   /image 的正戏在图片附件路(ProcessLine 截走),分派面是死案;
    //   /hooks 与 /trace 分派面有,帮助面没有(帮助面的旧缺口,本单不动)。
    for (const lubancode::app::SlashCommandSpec& spec : table) {
        if (spec.handler == nullptr) {
            continue;  // 死案
        }
        // spec.name 是 const char*:按内容比,不比指针(链接器合并字面量与否
        // 不该左右对账结果)。
        const std::string_view bare_name = spec.name;
        const std::string full_name = std::string("/") + spec.name;
        if (bare_name == "unknown") {
            continue;  // 兜底案,不是用户命令
        }
        if (bare_name == "hooks" || bare_name == "trace") {
            CHECK_MESSAGE(help_names.count(full_name) == 0, full_name);
            continue;  // 帮助面缺口,如实记录
        }
        CHECK_MESSAGE(help_names.count(full_name) == 1, full_name);
    }
    // 反向:帮助面的每个名字在分派面都有落点。例外如实记:/image 是死案
    //(正戏在图片附件路),/effort 在 parser 层折给 Think(分派面不单列)。
    for (const std::string& name : help_names) {
        if (name == "/image" || name == "/effort") {
            continue;
        }
        const std::string bare = name.substr(1);
        bool found = false;
        for (const lubancode::app::SlashCommandSpec& spec : table) {
            if (std::string_view(spec.name) == bare) {
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
