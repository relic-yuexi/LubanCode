// 交互循环里,输入以 `/` 开头的走命令分发,不发给模型。这里只管"识别是哪个
// 命令、参数是什么"这一件纯逻辑的事(输入串 -> 命令枚举 + 参数),不管命令
// 具体怎么执行——执行逻辑留给 main.cpp(要碰 AgentLoop、要读用户主目录之类,
// 不是纯函数)。

#pragma once

#include <string>
#include <vector>

namespace lubancode::cli {

enum class SlashCommand {
    NotSlash,  // 不是以 / 开头,交互循环不该拦截,原样发给模型
    Help,
    Model,
    Config,
    Clear,
    Exit,
    Context,  // /context [档位]:看当前上下文占用,或临时改窗口大小
    Compact,  // /compact [重点说明]:手动触发一次历史压缩
    Think,    // /think [档位]:看/改推理强度
    Skills,   // /skills:列出扫描到的技能(M9)
    Mcp,      // /mcp:列出挂载的 MCP 服务器状态、工具清单(M8)
    Lsp,      // /lsp:列出各语言 LSP 服务器状态(未启动/运行中/已闲置关停)
    Todos,    // /todos:查看当前待办清单(M11/0.10.0)
    Plugins,  // /plugins:列出挂载的插件工具(DLL + lua)和加载警告(M7)
    Sessions,  // /sessions:列最近的会话存档(本目录;/sessions all 列全部目录)
    Resume,    // /resume <编号或id>:载入某场存档历史续聊
    Export,    // /export [路径]:当前会话导出 Markdown
    Title,     // /title [标题]:看/设当前会话标题(追加 title 事件行,最后一条胜)
    Unknown,  // 以 / 开头,但不认得这个命令
};

struct ParsedSlashCommand {
    SlashCommand command = SlashCommand::NotSlash;
    std::string args;      // 命令词后面剩下的部分,已剥两端空白;没有就是空串
    std::string raw_word;  // 原始命令词(小写化之前),Unknown 时用来提示"XXX 不认得"
};

// 纯函数:识别一行输入是不是 slash 命令、是哪一个、参数是什么。
// 命令词大小写不敏感(/Model 和 /model 视为一样);命令词和参数之间按第一个
// 空白切开。输入前后空白先剥掉。
ParsedSlashCommand ParseSlashCommand(const std::string& input);

// 一个命令一条:名字 + 一句话说明。/help 打印用的就是这份,line_editor 的
// Tab 补全、实时提示行也是从这份转过去的候选——统共这一份定义,不重复写。
struct SlashCommandInfo {
    std::string name;
    std::string description;
};
const std::vector<SlashCommandInfo>& AllSlashCommands();

}  // namespace lubancode::cli
