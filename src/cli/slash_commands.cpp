#include "cli/slash_commands.hpp"

#include <cctype>

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
    } else if (lower == "/mcp") {
        parsed.command = SlashCommand::Mcp;
    } else {
        parsed.command = SlashCommand::Unknown;
    }
    return parsed;
}

const std::vector<SlashCommandInfo>& AllSlashCommands() {
    static const std::vector<SlashCommandInfo> kCommands = {
        {"/help", "列出所有命令"},
        {"/model", "拉模型列表选,或 /model 名字 直接切"},
        {"/config", "打印当前生效配置和本会话在用的 model"},
        {"/clear", "清空对话历史"},
        {"/exit", "退出(裸词 exit/quit 也认)"},
        {"/context", "看当前上下文占用;/context 256k|512k|1m 临时改窗口大小"},
        {"/compact", "手动压缩历史;/compact 重点说明 可指定这次额外保留什么"},
        {"/think", "看当前推理强度;/think 档位 切档位,档位以服务商为准(/effort 同义)"},
        {"/effort", "同 /think(推理强度别名)"},
        {"/skills", "列出扫描到的技能(主目录级 + 项目级)"},
        {"/mcp", "列出挂载的 MCP 服务器状态和工具清单"},
    };
    return kCommands;
}

}  // namespace lubancode::cli
