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
    Provider,  // /provider add|list|switch|remove:多端模型服务配置
    Config,
    Init,      // /init:在项目根创建 AGENTS.md 并立刻载入
    Clear,
    Exit,
    Context,  // /context [档位]:看当前上下文占用,或临时改窗口大小
    Compact,  // /compact [重点说明]:手动触发一次历史压缩
    Think,    // /think [档位]:看/改推理强度
    Skills,   // /skills:列出扫描到的技能(M9)
    Skill,    // /skill install|list|update|remove:远端技能分发
    Mcp,      // /mcp:列出挂载的 MCP 服务器状态、工具清单(M8)
    Lsp,      // /lsp:列出各语言 LSP 服务器状态(未启动/运行中/已闲置关停)
    Todos,    // /todos:查看当前待办清单(M11/0.10.0)
    Plugins,  // /plugins:列出挂载的插件工具(DLL + lua)和加载警告(M7)
    Tools,    // /tools:列工具三态——核心(恒在)/已加载/延迟未加载(tool_search)
    Memory,   // /memory:项目记忆开关、查看、显式记忆、遗忘与重建
    Sessions,  // /sessions:列最近的会话存档(本目录;/sessions all 列全部目录)
    Resume,    // /resume <编号或id>:载入某场存档历史续聊
    Export,    // /export [路径]:当前会话导出 Markdown
    Title,     // /title [标题]:看/设当前会话标题(追加 title 事件行,最后一条胜)
    Soul,      // /soul [内容|clear|名字|off|default]:魂(风格叠加层)查看/设置
    Prompt,    // /prompt [reset]:看法(系统提示词)的来源,或还原 system_prompt.md
    Language,  // /language [语言码]:列可选界面语言/切换(i18n)
    Image,     // /image <路径...>:附一张或多张本地图片
    Worktree,  // /worktree new|list|exit:隔离工作树会话
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

// /provider 的二级参数也收在 cli 层做纯解析，main.cpp 只接收已拆好的
// 字段、做写盘和切会话。model / window 选项都只认一个值；少参数、重复
// 选项、夹生子命令一律 Invalid，由调用方统一打印用法。
enum class ProviderCommandAction { Invalid, List, Add, Switch, Remove, Set };

struct ParsedProviderCommand {
    ProviderCommandAction action = ProviderCommandAction::Invalid;
    std::string name;
    std::string base_url;
    std::string wire;
    std::string key_env = "ANTHROPIC_AUTH_TOKEN";
    std::string key;     // --key:明文 api_key,可选,一行式旧用法的新增项
    std::string model;
    std::string effort;  // --effort:model_reasoning_effort,可选
    std::string window;
    // action == Add 且 wizard == true 时,说明这是"裸敲 /provider add"或
    // "/provider add 名字"这两种触发分步向导的写法(words.size() <= 2),
    // main.cpp 据此走 RunProviderAddWizard 而不是一行式解析结果。
    bool wizard = false;
    // action == Set 时才有意义:field 是要设的字段名(已小写化,认
    // native_web_search / extra_body / extra_header 三种)。
    //   - native_web_search:固定四个词,value 是第四个词(已小写化,on/
    //     off/true/false/1/0 这类写法留给 main.cpp 调
    //     config::ParseBoolToggle 去解读)。
    //   - extra_body:value 是"字段名之后、到这一行结尾"的原始文本(保留
    //     大小写、空白、花括号里的一切——一坨 JSON 不能按空格切词),合不
    //     合法留给 main.cpp/config 层解析。
    //   - extra_header:header_name 是紧跟 extra_body/extra_header 后面
    //     那一个词(保留原始大小写,HTTP 头名字大小写有意义,不替用户
    //     改掉);value 是 header_name 之后、到行尾的原始文本(可以有
    //     空格),空串表示删除这一条头。
    // 这层只管拆词,不管字段认不认得、值合不合法,跟 wire/window 那两个
    // 字段同一个分工路数。
    std::string field;
    std::string header_name;
    std::string value;
};

ParsedProviderCommand ParseProviderCommand(const std::string& args);

// /provider remove 的会话级护栏。拆成纯函数，命令处理与单测共用，免得
// "当前端不许删"这条规矩散在 main 的 IO 分支里。
bool CanRemoveProvider(const std::string& active_provider, const std::string& name);

// 一个命令一条:名字 + 一句话说明。/help 打印用的就是这份,line_editor 的
// Tab 补全、实时提示行也是从这份转过去的候选——统共这一份定义,不重复写。
// i18n:说明文字经 tr(slash.desc.*)取值,/language 切换后下一次调用重建,
// 返回引用在下一次语言切换之前有效。
struct SlashCommandInfo {
    std::string name;
    std::string description;
};
const std::vector<SlashCommandInfo>& AllSlashCommands();

}  // namespace lubancode::cli
