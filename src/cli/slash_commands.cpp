#include "cli/slash_commands.hpp"

#include <cctype>

#include "cli/i18n.hpp"

namespace lubancode::cli {

namespace {

std::string Trim(const std::string& s) {
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])) != 0) {
        ++begin;
    }
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
        --end;
    }
    return s.substr(begin, end - begin);
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

ParsedSlashCommand ParseSlashCommand(const std::string& input) {
    ParsedSlashCommand parsed;

    const std::string trimmed = Trim(input);
    if (trimmed.empty() || trimmed[0] != '/') {
        parsed.command = SlashCommand::NotSlash;
        return parsed;
    }

    const std::size_t space = trimmed.find_first_of(" \t");
    const std::string word = (space == std::string::npos) ? trimmed : trimmed.substr(0, space);
    const std::string args = (space == std::string::npos) ? "" : Trim(trimmed.substr(space + 1));

    parsed.raw_word = word;
    parsed.args = args;

    const std::string lower = ToLower(word);
    if (lower == "/help") {
        parsed.command = SlashCommand::Help;
    } else if (lower == "/model") {
        parsed.command = SlashCommand::Model;
    } else if (lower == "/config") {
        parsed.command = SlashCommand::Config;
    } else if (lower == "/clear") {
        parsed.command = SlashCommand::Clear;
    } else if (lower == "/exit" || lower == "/quit") {
        parsed.command = SlashCommand::Exit;
    } else if (lower == "/context") {
        parsed.command = SlashCommand::Context;
    } else if (lower == "/compact") {
        parsed.command = SlashCommand::Compact;
    } else if (lower == "/think" || lower == "/effort") {
        // M10:/effort 是 /think 的别名,同一个命令枚举值——解析层面这俩
        // 词天生等价,不用另开一个 SlashCommand::Effort 分叉出两套处理逻辑。
        parsed.command = SlashCommand::Think;
    } else if (lower == "/skills") {
        parsed.command = SlashCommand::Skills;
    } else if (lower == "/skill") {
        parsed.command = SlashCommand::Skill;
    } else if (lower == "/mcp") {
        parsed.command = SlashCommand::Mcp;
    } else if (lower == "/lsp") {
        parsed.command = SlashCommand::Lsp;
    } else if (lower == "/todos") {
        parsed.command = SlashCommand::Todos;
    } else if (lower == "/plugins") {
        parsed.command = SlashCommand::Plugins;
    } else if (lower == "/tools") {
        parsed.command = SlashCommand::Tools;
    } else if (lower == "/sessions") {
        parsed.command = SlashCommand::Sessions;
    } else if (lower == "/resume") {
        parsed.command = SlashCommand::Resume;
    } else if (lower == "/export") {
        parsed.command = SlashCommand::Export;
    } else if (lower == "/title") {
        parsed.command = SlashCommand::Title;
    } else if (lower == "/soul") {
        parsed.command = SlashCommand::Soul;
    } else if (lower == "/prompt") {
        parsed.command = SlashCommand::Prompt;
    } else if (lower == "/language" || lower == "/lang") {
        // /lang 是 /language 的省事别名,跟 LUBANCODE_LANG 环境变量对得上口。
        parsed.command = SlashCommand::Language;
    } else {
        parsed.command = SlashCommand::Unknown;
    }
    return parsed;
}

const std::vector<SlashCommandInfo>& AllSlashCommands() {
    // i18n:说明文字按当前语言现查(tr),语言切换后惰性重建——静态表 +
    // 记住"上次是按哪种语言建的",不一致就重来一遍。交互循环是单线程消费
    // (Tab 补全、/help),不用加锁。
    static std::vector<SlashCommandInfo> commands;
    static std::string built_for;
    if (commands.empty() || built_for != CurrentLanguage()) {
        built_for = CurrentLanguage();
        commands = {
            {"/help", tr("slash.desc.help")},
            {"/model", tr("slash.desc.model")},
            {"/config", tr("slash.desc.config")},
            {"/language", tr("slash.desc.language")},
            {"/clear", tr("slash.desc.clear")},
            {"/exit", tr("slash.desc.exit")},
            {"/context", tr("slash.desc.context")},
            {"/compact", tr("slash.desc.compact")},
            {"/think", tr("slash.desc.think")},
            {"/effort", tr("slash.desc.effort")},
            {"/skills", tr("slash.desc.skills")},
            {"/skill", tr("slash.desc.skill")},
            {"/mcp", tr("slash.desc.mcp")},
            {"/lsp", tr("slash.desc.lsp")},
            {"/todos", tr("slash.desc.todos")},
            {"/plugins", tr("slash.desc.plugins")},
            {"/tools", tr("slash.desc.tools")},
            {"/sessions", tr("slash.desc.sessions")},
            {"/resume", tr("slash.desc.resume")},
            {"/export", tr("slash.desc.export")},
            {"/title", tr("slash.desc.title")},
            {"/soul", tr("slash.desc.soul")},
            {"/prompt", tr("slash.desc.prompt")},
        };
    }
    return commands;
}

}  // namespace lubancode::cli
