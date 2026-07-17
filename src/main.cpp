// lubancode - C++ AI 编程 CLI
// M5:去掉硬编码的 MiniMax 默认值(lubancode 是通用工具,不绑死哪一家模型
// 服务)——base_url/api_key/model 不再有内置默认值。交互模式启动时这几项
// 缺了就先走一遍初次配置向导,配完直接进入会话,不用重启;单发模式/管道
// 模式缺配置则直接报可读的错。交互循环里加了 /help /model /config /clear
// /exit 几个 slash 命令,/model 能让模型切换真正在下一次请求生效。

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/compact.hpp"
#include "agent/loop.hpp"
#include "agent/prompts.hpp"
#include "agent/session_store.hpp"
#include "api/anthropic/client.hpp"
#include "api/backend.hpp"
#include "api/models.hpp"
#include "api/responses/client.hpp"
#include "cli/console_input.hpp"
#include "cli/context_tracker.hpp"
#include "cli/diff.hpp"
#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/setup_wizard.hpp"
#include "cli/slash_commands.hpp"
#include "cli/spinner.hpp"
#include "cli/theme.hpp"
#include "cli/todo_render.hpp"
#include "cli/transcript.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "config/prompt_files.hpp"
#include "lsp/manager.hpp"
#include "mcp/client.hpp"
#include "mcp/mcp_tool.hpp"
#include "tools/agent_tool.hpp"
#include "tools/edit_file.hpp"
#include "tools/hooks.hpp"
#include "tools/lua_tool.hpp"
#include "tools/plugin_loader.hpp"
#include "tools/lsp_tool.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/run_command.hpp"
#include "tools/search.hpp"
#include "tools/skill_loader.hpp"
#include "tools/skill_tool.hpp"
#include "tools/todo_tool.hpp"
#include "tools/tool_search.hpp"
#include "tools/web_fetch.hpp"
#include "tools/web_search.hpp"
#include "tools/write_file.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
// windows.h 的 max/min 宏会跟 <algorithm> 的 std::max/std::min 撞车(UI-B
// 的锚点记账用到了 std::max),NOMINMAX 关掉,跟 console_input.cpp 一个规矩。
#define NOMINMAX
#include <windows.h>
#endif

namespace {

constexpr std::string_view kVersion = "0.17.0";

void PrintVersion() {
    std::cout << "lubancode " << kVersion << "\n";
}

void PrintHelp() {
    std::cout
        << "lubancode " << kVersion << " - C++ AI 编程 CLI\n\n"
        << "用法:\n"
        << "  lubancode [选项]\n"
        << "  lubancode \"问题\"          一次问答,能用工具就用工具\n"
        << "  lubancode                  不带参数则进入交互循环;首次运行缺配置会先走一遍初次配置\n"
        << "                              向导,配完直接进入会话,不用重启。exit/quit 或 EOF(Ctrl+Z /\n"
        << "                              管道读尽)退出;空行只是重新给提示符,不退出\n\n"
        << "选项:\n"
        << "  --version              打印版本号\n"
        << "  --help                 打印本帮助\n"
        << "  --yes                  自动确认所有需要确认的工具调用(比如 run_command),不再逐条询问\n"
        << "  --continue             交互模式启动时自动恢复本目录最近一场会话存档(等价开场 /resume\n"
        << "                         最近一场);本目录没有存档就正常开新会话\n"
        << "  --config               打印最终生效的配置(api_key 打码)和每个字段来自哪一级,排查配置问题用\n"
        << "  --system-prompt <文件> 用这个文件(.md/.txt,UTF-8)替换默认系统提示的人格段,工作目录、\n"
        << "                         工具调用这些运行必需的上下文照样追加,不受影响。压过配置文件里的\n"
        << "                         system_prompt_file 字段,也压过 ~/.lubancode/system_prompt.md\n"
        << "  --reset-system-prompt  把 ~/.lubancode/system_prompt.md(法)还原成内置默认,旧文件挪成\n"
        << "                         system_prompt.md.bak(同 /prompt reset,只是不进交互、打结果就退)\n\n"
        << "魂法分家(首次启动自动生成,缺了每次启动补齐,已存在不覆盖):\n"
        << "  ~/.lubancode/system_prompt.md  \"法\"——系统提示词人格段,改它定制行为;内容剥掉顶部注释后\n"
        << "                                 全空白就回退内置默认\n"
        << "  ~/.lubancode/SOUL.md           \"魂\"——风格叠加层,写点风格指令(如\"只用文言文答话\"),\n"
        << "                                 注入在系统提示最后;留空 = 无效果\n"
        << "  ~/.lubancode/souls/*.md        备选魂(自带 wenyan.md 文言示例),交互模式 /soul 名字 切换\n\n"
        << "交互模式里,输入以 / 开头的一行走命令,不发给模型:\n"
        << "  /help           列出所有命令\n"
        << "  /model          拉取模型列表,编号选择切换(默认第一个)\n"
        << "  /model 名字     直接切到指定模型名,不用拉列表\n"
        << "  /config         打印当前生效配置(复用 --config 的逻辑),外加本会话实际在用的 model\n"
        << "  /clear          清空对话历史\n"
        << "  /context        看当前上下文占用(token 数/窗口大小/百分比)\n"
        << "  /context 512k   临时改窗口大小(256k/512k/1m/裸数字都认),只本会话生效\n"
        << "  /compact [重点说明]  手动触发一次历史压缩,可选指定这次额外保留什么\n"
        << "  /think          看当前推理强度(/effort 同义)\n"
        << "  /think 档位     切推理强度,档位以服务商为准(anthropic 内置 none/low/medium/high/xhigh/max\n"
        << "                  映射,responses 原样递给 API)\n"
        << "  /skills         列出扫描到的技能(主目录级 + 项目级)\n"
        << "  /mcp            列出挂载的 MCP 服务器状态和工具清单\n"
        << "  /lsp            列出各语言 LSP 服务器状态(未启动/运行中/已闲置关停)\n"
        << "  /todos          查看当前待办清单(todo_write 工具维护的那份)\n"
        << "  /plugins        列出挂载的插件工具(主目录 .lubancode/plugins 下的 *.dll 和 *.lua)\n"
        << "  /tools          列工具三态:核心(恒在)/已加载/延迟未加载(工具总数超过配置文件\n"
        << "                  tool_search_threshold(默认 20,0=永不延迟)时,MCP/插件等外挂工具\n"
        << "                  延迟挂载,模型用 tool_search 检索后方可调用)\n"
        << "  /sessions       列最近 20 场会话存档(时间倒序编号)\n"
        << "  /resume 编号或id  载入该场存档历史续聊\n"
        << "  /export [路径]  当前会话导出 Markdown(默认 sessions/<id>.md)\n"
        << "  /sessions       列本目录最近 20 场会话存档(时间倒序编号);/sessions all 列全部目录\n"
        << "  /resume 编号或id  载入该场存档历史续聊(编号按本目录列表数)\n"
        << "  /export [路径]  当前会话导出 Markdown(默认 sessions/<id>.md;全量流水,压缩点带标注)\n"
        << "  /title [标题]   看/设本场会话标题,/sessions 列表和 /export 大标题都用它\n"
        << "  /soul           列出可用的魂;/soul 名字 切换风格叠加层(即时生效),/soul off 关,\n"
        << "                  /soul default 回 SOUL.md\n"
        << "  /prompt         看当前法(系统提示词)的来源和字数;/prompt reset 还原 system_prompt.md\n"
        << "  Shift+Enter     输入框里插一个换行,写多行消息(Alt+Enter 同义;注意 Windows Terminal\n"
        << "                  默认把 Alt+Enter 绑成全屏切换、会吞掉这个键,用 Shift+Enter 最稳);\n"
        << "                  Enter 把整段(多行拼换行)一次发出,空白内容按 Enter 原地不动\n"
        << "  ESC             流式回复期间按下:打断当前这轮回答,已出的半截话保留、下一轮能接着聊;\n"
        << "                  空闲时按下:清空正在编辑的整段输入;确认提示 [y/a/N] 下按下:等同拒绝;\n"
        << "                  聚焦查看(Ctrl+E)画面里按下:返回会话\n"
        << "  / 后按 ↓↑       进入候选菜单直选(↓ 选中第一条,↓↑ 循环移动),Enter 执行选中命令\n"
        << "                  (已敲的参数尾巴原样保留),继续打字/退格/ESC 回普通编辑\n"
        << "  Ctrl+O          紧凑/详细全局切换:把全部工具条目按新档整块重打(详细 = 完整参数\n"
        << "                  JSON + 完整输出/diff 全文),再按切回\n"
        << "  Ctrl+E          聚焦查看当前焦点条目(无焦点则最近一条)全文;再按 Ctrl+E 或 ESC 返回\n"
        << "  Tab             输入框有内容:补全/轮转 slash 命令(现职);输入框为空:进入焦点态\n"
        << "                  并选中最近一条工具条目;焦点态内 Tab 往旧走、Shift+Tab 往新走,\n"
        << "                  ESC/Enter 退出焦点态回编辑\n"
        << "  Shift+Tab       循环切确认档(confirm/auto/yolo)——任何时候都是,跟状态行提示\n"
        << "                  一致;只有焦点态内例外(那里是焦点往新走)\n"
        << "  流式期间打字回车  不会打断当前流,而是排进队列,本轮结束后按顺序自动发出\n"
        << "  /exit           退出(裸词 exit/quit 也认)\n\n"
        << "配置优先级(从高到低,按字段逐个决,不是整套配置一刀切):\n"
        << "  1) LUBANCODE_ 专属环境变量\n"
        << "       LUBANCODE_WIRE          协议选择,anthropic 或 responses\n"
        << "       LUBANCODE_BASE_URL      API 地址\n"
        << "       LUBANCODE_API_KEY       认证令牌\n"
        << "       LUBANCODE_MODEL         模型名\n"
        << "       LUBANCODE_MAX_CONTEXT   history 裁剪阈值(字符数,老的硬安全网)\n"
        << "       LUBANCODE_THEME         终端配色主题,dark / light / plain\n"
        << "       LUBANCODE_SYSTEM_PROMPT_FILE  人格文件路径,同 --system-prompt(命令行参数压过这个)\n"
        << "       LUBANCODE_CONTEXT_WINDOW      上下文窗口 token 数,256k/512k/1m/裸数字\n"
        << "       LUBANCODE_COMPACT_MODEL       压缩用的模型,空 = 跟当前会话模型一致\n"
        << "       LUBANCODE_THINK               推理强度,档位以服务商为准,空 = 不发这个参数\n"
        << "       LUBANCODE_SOUL                魂的名字,default = SOUL.md,off = 不叠加,别的名字\n"
        << "                                     = souls/<名字>.md\n"
        << "  2) 配置文件(第一个找到的生效,查找顺序:cwd 的 .lubancode/config.json → 主目录的\n"
        << "     .lubancode/config.json → cwd 的旧位置 .lubancode.json → 主目录的旧位置\n"
        << "     .lubancode.json;读到旧位置会自动挪到新位置)。字段:wire / base_url / api_key / model /\n"
        << "     max_context_chars / theme / system_prompt_file / context_window / compact_model / think,\n"
        << "     全部可选。另有 hooks / mcpServers / search 三段(只从配置文件读,没有环境变量、没有内置默认值):\n"
        << "       \"mcpServers\": {\"服务器名\": {\"command\": \"...\", \"args\": [...], \"env\": {...}}}\n"
        << "       起进程握手成功后,工具以 mcp__服务器名__工具名 挂进工具表,/mcp 看状态\n"
        << "       \"search\": {\"provider\": \"tavily|brave|serper\", \"api_key\": \"...\"}\n"
        << "       配了这一段才会注册 web_search 工具;web_fetch 工具无须配置,始终可用\n"
        << "     再有 lsp 一段(同样只从配置文件读;没配 = 不启用 = lsp 工具不注册):\n"
        << "       \"lsp\": {\"cpp\": {\"command\": \"clangd\", \"args\": [...], \"extensions\": [\".cpp\", \".hpp\"],\n"
        << "                \"idle_minutes\": 10}}\n"
        << "       配了才注册 lsp 工具(definition/references/symbols/diagnostics 语义查询),懒启动、\n"
        << "       闲置自动关停,/lsp 看各语言服务器状态\n"
        << "  3) 通用环境变量(向后兼容旧用法,跟 Claude Code 等工具共用同名变量时容易撞车,\n"
        << "     建议改用第 1 级的 LUBANCODE_* 专属变量):\n"
        << "       wire=anthropic 时读 ANTHROPIC_BASE_URL / ANTHROPIC_AUTH_TOKEN / ANTHROPIC_MODEL\n"
        << "       wire=responses 时读 OPENAI_BASE_URL / OPENAI_API_KEY / OPENAI_MODEL\n"
        << "  4) 内置默认值:wire=anthropic、max_context_chars=" << lubancode::config::kDefaultMaxContextChars
        << "、theme=" << lubancode::config::kDefaultTheme
        << "、context_window=" << lubancode::config::kDefaultContextWindowTokens << "。\n"
        << "     base_url/api_key/model/system_prompt_file/compact_model/think 不绑死任何一家模型服务,\n"
        << "     没有内置默认值——四级都没配到,交互模式会自动走初次配置向导;单发模式/管道模式会直接\n"
        << "     报错,提示三条配置途径。用 --config 能看到当前实际生效的配置和每个字段的来源。\n";
}

// 按 wire 造对应的后端实现。agent 层只认 Backend 这个抽象接口,不关心
// 背后具体是哪个协议在干活。
std::unique_ptr<lubancode::api::Backend> BuildBackend(const lubancode::config::Config& config) {
    if (config.wire == lubancode::config::Wire::Responses) {
        return std::make_unique<lubancode::api::responses::ResponsesBackend>(config.base_url, config.auth_token);
    }
    return std::make_unique<lubancode::api::anthropic::AnthropicBackend>(config.base_url, config.auth_token);
}

// 包一层 Backend:真正发请求前,把 Request.model 换成"当前会话实际在用的
// model"。AgentLoop 的 model 是构造时定死的私有成员,没有 setter(agent 层
// 现有文件不让动,详见任务规矩),这层包装是唯一能让 /model 切换在下一次
// 请求"真正生效"、又不用碰 agent/loop.hpp/.cpp 的办法。current_model 用
// shared_ptr,是因为 /model 命令改的和这里读的得是同一块内存,AgentLoop
// 只认引用、包装器要跨多轮 Run() 存活。
class ModelOverrideBackend : public lubancode::api::Backend {
public:
    ModelOverrideBackend(lubancode::api::Backend& inner, std::shared_ptr<std::string> current_model)
        : inner_(inner), current_model_(std::move(current_model)) {}

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        lubancode::api::Request patched = request;
        patched.model = *current_model_;
        return inner_.send_stream(patched, on_event, cancel);
    }

private:
    lubancode::api::Backend& inner_;
    std::shared_ptr<std::string> current_model_;
};

// 包一层 Backend:真正发请求前,把 Request.reasoning_effort 换成"当前会话
// 实际在用的推理强度"。跟 ModelOverrideBackend 是同一个套路,同样的理由
// (AgentLoop 没有 setter,agent 层现有文件不让动)——current_think 为空串
// 就是"不发这个参数",维持原有行为不变。/think 命令改的和这里读的是
// 同一块 shared_ptr<string> 内存,单发模式(AskOnce)没有 /think 命令,
// current_think 构造后就不再变,等价于"直接按配置发一次"。
class ThinkOverrideBackend : public lubancode::api::Backend {
public:
    ThinkOverrideBackend(lubancode::api::Backend& inner, std::shared_ptr<std::string> current_think)
        : inner_(inner), current_think_(std::move(current_think)) {}

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        lubancode::api::Request patched = request;
        patched.reasoning_effort = *current_think_;
        return inner_.send_stream(patched, on_event, cancel);
    }

private:
    lubancode::api::Backend& inner_;
    std::shared_ptr<std::string> current_think_;
};

// 包一层 Backend:真正发请求前,把模型目录(models.json)里当前模型的
// base_instructions 作为独立段追加到 Request.system 末尾。跟 Model/
// ThinkOverrideBackend 同一个套路、同一个理由(AgentLoop 的系统提示构造时
// 定死,agent 层现有文件不动)——current_instructions 为空串就是"不追加",
// 原样透传,零破坏。/model 切换改的和这里读的是同一块 shared_ptr<string>
// 内存,切到目录外模型时上层把它清空,旧模型的指令自然不再发。
class ModelInstructionsBackend : public lubancode::api::Backend {
public:
    ModelInstructionsBackend(lubancode::api::Backend& inner, std::shared_ptr<std::string> current_instructions)
        : inner_(inner), current_instructions_(std::move(current_instructions)) {}

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        lubancode::api::Request patched = request;
        patched.system = lubancode::agent::WithModelInstructions(request.system, *current_instructions_);
        return inner_.send_stream(patched, on_event, cancel);
    }

private:
    lubancode::api::Backend& inner_;
    std::shared_ptr<std::string> current_instructions_;
};

// 包一层 Backend:真正发请求前,把当前的"魂"(SOUL.md / souls/ 的风格叠加
// 层)追加到 Request.system 最后。跟上面几层同一个套路、同一个理由。魂
// 必须压轴——所以这一层要放在 ModelInstructionsBackend 的更内侧(请求先
// 经过 instructions 层追加模型专属段,再到这层追加魂),字符串里魂自然
// 排最后。current_soul 存的是魂文件的原始内容(注释由 agent::WithSoul 在
// 注入时剥),/soul 切换改的和这里读的是同一块 shared_ptr<string> 内存,
// 下一轮请求即时生效;空串(SOUL.md 默认、或 /soul off)= 不追加,零破坏。
class SoulOverlayBackend : public lubancode::api::Backend {
public:
    SoulOverlayBackend(lubancode::api::Backend& inner, std::shared_ptr<std::string> current_soul)
        : inner_(inner), current_soul_(std::move(current_soul)) {}

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        lubancode::api::Request patched = request;
        patched.system = lubancode::agent::WithSoul(request.system, *current_soul_);
        return inner_.send_stream(patched, on_event, cancel);
    }

private:
    lubancode::api::Backend& inner_;
    std::shared_ptr<std::string> current_soul_;
};

// tool_search(延迟挂载):包一层 Backend,真正发请求前把"延迟未加载"工具
// 的紧凑索引段追加到 Request.system 末尾。跟 ModelInstructionsBackend 同一个
// 套路、同一个理由(AgentLoop 的系统提示构造后改不了,agent 层现有构造不
// 破)——index_provider 每次 send_stream 现算,tool_search 命中后的下一次
// 请求,新挂载的工具自然从索引段里消失;provider 给空串就原样透传,零破坏。
// 这层只包给主 AgentLoop 用,不进 AgentTool 拿的那条链——子代理的索引段
// 按它自己的注册表算,由 AgentTool::SetDeferredIndexProvider 单独注入,
// 两边各管各的,不会重复追加。
class DeferredIndexBackend : public lubancode::api::Backend {
public:
    DeferredIndexBackend(lubancode::api::Backend& inner, std::function<std::string()> index_provider)
        : inner_(inner), index_provider_(std::move(index_provider)) {}

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        lubancode::api::Request patched = request;
        patched.system = lubancode::agent::WithDeferredToolsIndex(
            request.system, index_provider_ ? index_provider_() : std::string());
        return inner_.send_stream(patched, on_event, cancel);
    }

private:
    lubancode::api::Backend& inner_;
    std::function<std::string()> index_provider_;
};

// /tools 命令:列工具三态——核心(恒在)/已加载的延迟工具/延迟未加载,
// 各带计数。没启用延迟机制(总数没超阈值,或阈值是 0)时说明一句,不摆
// 三态的空架子。
void PrintToolsCommand(const lubancode::tools::ToolRegistry& registry, const std::set<std::string>& loaded,
                        bool deferral_enabled, int threshold) {
    std::vector<const lubancode::tools::Tool*> core;
    std::vector<const lubancode::tools::Tool*> loaded_deferred;
    std::vector<const lubancode::tools::Tool*> pending_deferred;
    for (const auto& tool : registry.All()) {
        if (!tool->deferred()) {
            core.push_back(tool.get());
        } else if (loaded.count(tool->name()) != 0) {
            loaded_deferred.push_back(tool.get());
        } else {
            pending_deferred.push_back(tool.get());
        }
    }
    if (!deferral_enabled) {
        std::cout << "工具共 " << registry.All().size() << " 个,"
                   << (threshold == 0 ? std::string("阈值 0(永不延迟)")
                                       : "低于阈值 " + std::to_string(threshold))
                   << ",全量直挂,tool_search 延迟机制未启用。
";
        for (const auto& tool : registry.All()) {
            std::cout << "  - " << tool->name() << "
";
        }
        return;
    }
    std::cout << "tool_search 延迟挂载已启用(阈值 " << threshold << ",loaded 集合会话级,/clear 不清)。
";
    std::cout << "核心工具(恒在)" << core.size() << " 个:
";
    for (const auto* tool : core) {
        std::cout << "  - " << tool->name() << "
";
    }
    std::cout << "已加载的延迟工具 " << loaded_deferred.size() << " 个:
";
    for (const auto* tool : loaded_deferred) {
        std::cout << "  - " << tool->name() << "
";
    }
    if (loaded_deferred.empty()) {
        std::cout << "  (还没有,模型用 tool_search 命中后会出现在这里)
";
    }
    std::cout << "延迟未加载 " << pending_deferred.size() << " 个(在系统提示索引段里,检索后挂载):
";
    for (const auto* tool : pending_deferred) {
        std::cout << "  - " << tool->name() << "
";
    }
}

// --config、/config 共用:打印最终生效的配置和每个字段的来源。session_model
// 有值时(/config 场景)额外打一行"本会话实际在用的 model"——/model 切换
// 只影响会话内存,不一定跟 config.model(四级合并出来的那份)一致。
// catalog 非空时(现在两个调用点都传)追加两行:模型目录路径 + 条目数,
// 以及"当前模型(会话在用的那个,没有就看 config.model)命没命中目录"。
void PrintConfigDiagnostics(const lubancode::config::ConfigResult& result,
                             const std::optional<std::string>& session_model = std::nullopt,
                             const lubancode::config::ModelCatalog* catalog = nullptr) {
    const auto& config = result.config;
    const auto& sources = result.sources;
    const std::string wire_str = config.wire == lubancode::config::Wire::Responses ? "responses" : "anthropic";

    std::cout << "lubancode 最终生效的配置:\n\n";
    std::cout << "  wire               = " << wire_str << "  [" << lubancode::config::ToString(sources.wire) << "]\n";
    std::cout << "  base_url           = " << (config.base_url.empty() ? "(未设置)" : config.base_url) << "  ["
              << lubancode::config::ToString(sources.base_url) << "]\n";
    std::cout << "  api_key            = " << lubancode::config::MaskApiKey(config.auth_token) << "  ["
              << lubancode::config::ToString(sources.auth_token) << "]\n";
    std::cout << "  model              = " << (config.model.empty() ? "(未设置)" : config.model) << "  ["
              << lubancode::config::ToString(sources.model) << "]\n";
    std::cout << "  max_context_chars  = " << config.max_context_chars << "  ["
              << lubancode::config::ToString(sources.max_context_chars) << "]\n";
    std::cout << "  theme              = " << config.theme << "  [" << lubancode::config::ToString(sources.theme)
              << "]\n";
    std::cout << "  system_prompt_file = " << (config.system_prompt_file.empty() ? "(未设置)" : config.system_prompt_file)
              << "  [" << lubancode::config::ToString(sources.system_prompt_file) << "]\n";
    std::cout << "  context_window     = " << config.context_window_tokens << " tokens  ["
              << lubancode::config::ToString(sources.context_window_tokens) << "]\n";
    std::cout << "  compact_model      = " << (config.compact_model.empty() ? "(未设置,跟会话模型一致)" : config.compact_model)
              << "  [" << lubancode::config::ToString(sources.compact_model) << "]\n";
    std::cout << "  think              = " << (config.think.empty() ? "(未设置,不发这个参数)" : config.think) << "  ["
              << lubancode::config::ToString(sources.think) << "]\n";
    std::cout << "  soul               = " << (config.soul.empty() ? "(未设置,用主目录 SOUL.md)" : config.soul)
              << "  [" << lubancode::config::ToString(sources.soul) << "]\n";
    std::cout << "  tool_search_threshold = " << config.tool_search_threshold
              << (config.tool_search_threshold == 0 ? "(永不延迟)" : "") << "  ["
              << lubancode::config::ToString(sources.tool_search_threshold) << "]\n";
    if (result.config_file_path.has_value()) {
        std::cout << "  配置文件           = " << *result.config_file_path << "\n";
    }
    // M9:hooks 只从配置文件来,没有来源分级可打,只打个数——四类都是空的
    // 就直接说"未配置",省得打一堆 ×0。
    {
        const auto& hooks = config.hooks;
        std::vector<std::string> parts;
        if (!hooks.pre_tool.empty()) {
            parts.push_back("pre_tool×" + std::to_string(hooks.pre_tool.size()));
        }
        if (!hooks.post_tool.empty()) {
            parts.push_back("post_tool×" + std::to_string(hooks.post_tool.size()));
        }
        if (!hooks.session_start.empty()) {
            parts.push_back("session_start×" + std::to_string(hooks.session_start.size()));
        }
        if (!hooks.session_end.empty()) {
            parts.push_back("session_end×" + std::to_string(hooks.session_end.size()));
        }
        std::cout << "  hooks              = ";
        if (parts.empty()) {
            std::cout << "(未配置)";
        } else {
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) {
                    std::cout << ", ";
                }
                std::cout << parts[i];
            }
        }
        std::cout << "\n";
    }
    // M8:mcpServers 同样只从配置文件来,没有分级来源可打,只打个数——
    // /config 只报"配置了几个",实际存活状态得看 /mcp(那个才知道哪个真的
    // 起来了、握手成没成功)。
    {
        std::cout << "  mcpServers         = ";
        if (config.mcp_servers.empty()) {
            std::cout << "(未配置)";
        } else {
            std::cout << config.mcp_servers.size() << " 个服务器";
        }
        std::cout << "\n";
    }
    // websearch:search 段同样只从配置文件来。api_key 照例打码,provider
    // 直接亮出来——配了这一段 web_search 工具才会注册。
    {
        std::cout << "  search             = ";
        if (!config.search.Configured()) {
            std::cout << "(未配置,web_search 工具不注册)";
        } else {
            std::cout << config.search.provider
                      << " (api_key " << lubancode::config::MaskApiKey(config.search.api_key) << ")";
        }
        std::cout << "\n";
    }
    // 模型目录(models.json):路径 + 条目数,以及当前模型命没命中。
    if (catalog != nullptr) {
        std::cout << "  模型目录           = ";
        if (catalog->source_path.empty()) {
            const auto expected = lubancode::config::ModelCatalogPath();
            std::cout << "(未配置," << (expected.has_value() ? *expected : std::string("<找不到主目录>/.lubancode/models.json"))
                       << " 不存在)";
        } else {
            std::cout << catalog->source_path << "(" << catalog->models.size() << " 个条目)";
        }
        std::cout << "\n";
        const std::string& current = session_model.has_value() ? *session_model : config.model;
        const auto* entry = catalog->FindBySlug(current);
        std::cout << "  当前模型命中目录   = ";
        if (current.empty()) {
            std::cout << "(model 未设置)";
        } else if (entry != nullptr) {
            std::cout << "是(" << entry->slug
                       << (entry->display_name.empty() ? std::string() : ",display_name: " + entry->display_name)
                       << ")";
        } else {
            std::cout << "否(" << current << " 不在目录里,一切按现状)";
        }
        std::cout << "\n";
    }
    if (session_model.has_value()) {
        std::cout << "\n  本会话实际在用的 model = " << *session_model;
        if (*session_model != config.model) {
            std::cout << "  (只在本会话生效,尚未写入配置文件)";
        }
        std::cout << "\n";
    }
}

// 当前工作目录,转成 UTF-8 字符串(拼进系统提示词里给模型看)。
std::string CurrentDirUtf8() {
    const std::filesystem::path cwd = std::filesystem::current_path();
    const std::u8string u8 = cwd.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// 包一层 Backend:发起真正的网络请求前起一个"思考中"转轮(cli::Spinner),
// 收到第一个流事件就停。转轮跟着 send_stream 这一次调用走——AgentLoop 一次
// Run() 里可能因为工具调用来回好几趟,每趟各自单独调一次 send_stream,
// 工具执行发生在两次 send_stream 之间(loop.cpp 里,不在这层包装范围内),
// 天然满足"工具执行期间不转,发下一轮请求再转"这条要求,不用改
// agent/loop.cpp 一个字。spinner_enabled 由调用方按"stdout 是不是真控制台"
// 算好传进来——管道模式下这层直接透传,不起线程、不输出任何转轮字符。
class SpinnerBackend : public lubancode::api::Backend {
public:
    SpinnerBackend(lubancode::api::Backend& inner, const lubancode::cli::Theme& theme, bool spinner_enabled)
        : inner_(inner), theme_(theme), spinner_enabled_(spinner_enabled) {}

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        lubancode::cli::Spinner spinner(theme_, spinner_enabled_);
        bool stopped = false;
        const auto wrapped = [&](const lubancode::api::StreamEvent& event) {
            if (!stopped) {
                spinner.Stop();
                stopped = true;
            }
            on_event(event);
        };
        return inner_.send_stream(request, wrapped, cancel);
        // spinner 在这里析构,Stop() 兜底再调一次也是安全的(空操作)——
        // 万一 send_stream 直接失败、一个事件都没吐(比如连都没连上),
        // 转轮不会一直转着。
    }

private:
    lubancode::api::Backend& inner_;
    const lubancode::cli::Theme& theme_;
    bool spinner_enabled_;
};

// M11(0.10.0):输入/输出分界线。用户回车提交、模型真要开始作答那一刻打
// 一条,回合结束的统计行之后再打一条,把一问一答从视觉上框出来——纯粹
// 是一条线,不带文字、不带花边。is_console 为假(管道/重定向)时直接
// 什么都不打,不污染被重定向的输出。宽度用 cli::DetectConsoleWidth()
// 现测,测不到就交给 cli::BuildDividerLine 自己按 80 列兜底。颜色用
// theme.stats(跟 token 统计行、cwd 提示同一档"淡色信息"),plain 主题下
// theme.stats 本来就是空串,着色形同虚设,不用另外判断。
void PrintDivider(const lubancode::cli::Theme& theme, bool is_console) {
    if (!is_console) {
        return;
    }
    const std::optional<int> width = lubancode::cli::DetectConsoleWidth();
    const bool plain = theme.reset.empty();
    const std::string line = lubancode::cli::BuildDividerLine(width.value_or(80), plain);
    if (line.empty()) {
        return;
    }
    std::cout << theme.stats << line << theme.reset << "\n";
    std::cout.flush();
}

// 交互模式启动横幅:一眼看全版本、wire、当前模型、工作目录,两行,不啰嗦。
void PrintBanner(const lubancode::config::Config& config, const lubancode::cli::Theme& theme) {
    const std::string wire_str = config.wire == lubancode::config::Wire::Responses ? "responses" : "anthropic";
    std::cout << theme.banner << "lubancode " << kVersion << "  [" << wire_str << "] " << config.model << theme.reset
              << "\n";
    std::cout << theme.stats << "cwd: " << CurrentDirUtf8() << "  ·  输入问题回车发送,exit 退出,/help 看命令"
              << theme.reset << "\n";
}

// 基础工具集(不含 "agent" 自己)。子代理的工具表就是这一份原样一份——
// 防递归:子代理没法再委托一个孙代理,深度硬限 1。主循环的工具表在这份
// 基础上再多注册一个 "agent" 工具,两份各自独立构建(调用方各自新建一份,
// 每次调用都新建各工具实例,互不共享状态——这些工具本来就是无状态的,
// 多建几份不影响行为,只是各自持有自己的资源句柄)。
//
// 注:main_registry 里的 agent 工具会持有 sub_registry 的引用,调用方
// (InteractiveLoop / AskOnce)必须把两份都声明成同一层级的局部变量、
// sub_registry 声明在前 main_registry 声明在后——这样析构顺序自然反过来,
// 不会有悬垂引用;千万不能把它们塞进一个按值返回的结构体里再传出来,
// 那样 move/copy 一趟,agent 工具里存的引用就废了。
// skills:M9 新增,main.cpp 启动时(或 InteractiveLoop/AskOnce 入口)扫描一次
// 的技能清单,原样传进来注册成 "skill" 工具——子代理、主代理各自建的
// registry 都要有这个工具(技能对子代理同样有用),所以调用方每次调用都
// 传同一份清单进来。
// search_config:websearch 用。web_fetch 无条件注册;web_search 只在配置文件
// 写了 search 段(provider + api_key 齐活)时才注册——没配就不挂,模型的
// 工具表里压根没有这一项,不会瞎调。两个都进基础表,子代理也能用(子代理
// 干"搜了再读再总结"正合适)。
lubancode::tools::ToolRegistry BuildBaseToolRegistry(const std::vector<lubancode::tools::SkillMeta>& skills,
                                                       const lubancode::config::SearchConfig& search_config) {
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<lubancode::tools::ReadFileTool>());
    registry.Register(std::make_unique<lubancode::tools::RunCommandTool>());
    registry.Register(std::make_unique<lubancode::tools::WriteFileTool>());
    registry.Register(std::make_unique<lubancode::tools::EditFileTool>());
    registry.Register(std::make_unique<lubancode::tools::SearchTool>());
    registry.Register(std::make_unique<lubancode::tools::SkillTool>(skills));
    registry.Register(std::make_unique<lubancode::tools::WebFetchTool>("lubancode/" + std::string(kVersion)));
    if (search_config.Configured()) {
        registry.Register(std::make_unique<lubancode::tools::WebSearchTool>(search_config));
    }
    return registry;
}

// M8:一个已经跑起来的 MCP 服务器运行时状态——协议客户端本体,加上握手时
// 拿到的工具清单。/mcp 命令、注册进 registry 都要用这个。
// client 用 unique_ptr 而不是直接存 mcp::Client 对象:McpTool 持有
// mcp::Client& 引用,这份 runtime 要塞进 vector,vector 扩容/搬移只挪
// unique_ptr 本身(一个指针),Client 对象的地址不变,McpTool 里存的引用
// 不会失效。
struct McpServerRuntime {
    std::string name;
    std::unique_ptr<lubancode::mcp::Client> client;
    std::vector<lubancode::mcp::ToolInfo> tools;
};

// 按配置逐个起 MCP 服务器:起子进程 + initialize 握手 + tools/list。单个
// 服务器出岔子(起不来、握手超时、tools/list 失败……)只打一行警告就跳过,
// 不阻塞整个会话——只有真正跑通全流程的服务器才会进返回的 vector。
// mcpServers 没配(config.mcp_servers 是空 map)时,这个函数循环零次,
// 直接返回空 vector,天然满足"只有配了才起"这条要求,不用另外判断。
std::vector<McpServerRuntime> StartMcpServers(
    const std::map<std::string, lubancode::config::McpServerConfig>& configs, const lubancode::cli::Theme& theme) {
    std::vector<McpServerRuntime> out;
    for (const auto& [name, server_config] : configs) {
        auto client = std::make_unique<lubancode::mcp::Client>(name);
        const auto start_result = client->StartProcess(server_config.command, server_config.args, server_config.env);
        if (!start_result.success) {
            std::cout << theme.error << "[mcp] " << name << ": 启动失败 - " << start_result.error << theme.reset
                       << "\n";
            continue;
        }
        const auto init_result = client->Initialize();
        if (!init_result.has_value()) {
            std::cout << theme.error << "[mcp] " << name << ": 握手失败 - " << init_result.error() << theme.reset
                       << "\n";
            continue;
        }
        auto tools_result = client->ListTools();
        if (!tools_result.has_value()) {
            std::cout << theme.error << "[mcp] " << name << ": 获取工具清单失败 - " << tools_result.error()
                       << theme.reset << "\n";
            continue;
        }

        McpServerRuntime runtime;
        runtime.name = name;
        runtime.tools = std::move(*tools_result);
        std::cout << "[mcp] " << name << ": " << runtime.tools.size() << " 个工具已挂载\n";
        runtime.client = std::move(client);
        out.push_back(std::move(runtime));
    }
    return out;
}

// 把每个 MCP 服务器握手拿到的工具包成 McpTool,注册进 registry——主循环表、
// 子代理表都要各调一遍(MCP 工具对子代理同样有用),两份各自独立的
// McpTool 实例,但底下持的是同一个 mcp::Client&(工具背后是同一个子进程,
// 不会因为注册了两份就多起一个进程)。
void RegisterMcpTools(std::vector<McpServerRuntime>& mcp_servers, lubancode::tools::ToolRegistry& registry) {
    for (auto& runtime : mcp_servers) {
        for (const auto& tool_info : runtime.tools) {
            // tool_search:MCP 工具裹一层 DeferredTool 标成延迟挂载(mcp/
            // 目录不动,没法直接在 McpTool 上加 override)。阈值没超时延迟
            // 机制整个不启用,这层包装只是纯转发,行为不变。
            registry.Register(std::make_unique<lubancode::tools::DeferredTool>(
                std::make_unique<lubancode::mcp::McpTool>(*runtime.client, runtime.name, tool_info)));
        }
    }
}

// M7:一条插件工具的挂载记录,/plugins 命令展示用。
struct PluginMountInfo {
    std::string tool_name;  // 完整名(plugin__<名>__<工具>),跟模型看到的一致
    std::string kind;       // "DLL" 或 "lua"
};

// M7:扫两类插件(<主目录>/.lubancode/plugins 下的 *.dll 和 *.lua),挂进
// 主 registry——子代理表不挂,短命跑腿不用外挂。每个插件打一行
// "[plugin] 名: N 个工具";坏 DLL / 坏 lua 打警告跳过,不崩。
// plugin_host 由调用方持有,且必须声明在 registry 之前(PluginTool 手里的
// luban_tool_def* 指向 DLL 静态数据,模块要活得比 registry 久,析构反序那
// 一套,理由同 mcp_servers);LuaTool 连 lua_State 整个搬进 registry,没有
// 这层讲究。mounted/warnings 由调用方持有,交互模式给 /plugins 命令用。
void MountPlugins(lubancode::tools::PluginHost& plugin_host, lubancode::tools::ToolRegistry& registry,
                  const lubancode::cli::Theme& theme, std::vector<PluginMountInfo>& mounted,
                  std::vector<std::string>& warnings) {
    const auto home_dir = lubancode::config::HomeLubancodeDir();
    if (!home_dir.has_value()) {
        return;  // 找不到主目录,也就没有插件目录可扫
    }
    const std::filesystem::path plugins_dir =
        std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(home_dir->data()), home_dir->size())) /
        "plugins";

    // C ABI DLL 插件
    std::vector<std::string> new_warnings = plugin_host.LoadDirectory(plugins_dir);
    auto wrapped_plugins = plugin_host.WrapTools(new_warnings);
    for (auto& warning : new_warnings) {
        std::cout << theme.error << warning << theme.reset << "\n";
        warnings.push_back(std::move(warning));
    }
    for (auto& wrapped : wrapped_plugins) {
        std::cout << "[plugin] " << wrapped.stem << ": " << wrapped.tools.size() << " 个工具\n";
        for (auto& tool : wrapped.tools) {
            mounted.push_back({tool->name(), "DLL"});
            registry.Register(std::move(tool));
        }
    }

    // Lua 插件
    auto lua_result = lubancode::tools::LoadLuaPlugins(plugins_dir);
    for (auto& warning : lua_result.warnings) {
        std::cout << theme.error << warning << theme.reset << "\n";
        warnings.push_back(std::move(warning));
    }
    for (auto& tool : lua_result.tools) {
        std::cout << "[plugin] " << tool->stem() << ": 1 个工具\n";
        mounted.push_back({tool->name(), "lua"});
        registry.Register(std::move(tool));
    }
}

// /plugins 命令:列已挂载的插件工具(完整工具名 + 类别)和启动时的加载
// 警告;一个都没有时打印目录约定,顺带说明两类插件各自怎么写。
void PrintPluginsCommand(const std::vector<PluginMountInfo>& mounted, const std::vector<std::string>& warnings) {
    if (mounted.empty() && warnings.empty()) {
        const auto home_dir = lubancode::config::HomeLubancodeDir();
        const std::string dir =
            (home_dir.has_value() ? *home_dir : std::string("<找不到主目录>/.lubancode")) + "/plugins";
        std::cout << "没有挂载任何插件工具。\n\n"
                   << "插件目录约定(放进去,下次启动即挂载):\n"
                   << "  C ABI DLL: " << dir << "/*.dll\n"
                   << "      导出 luban_plugin_entry(见仓库 include/luban_plugin.h),示例在\n"
                   << "      examples/plugins/hello_plugin/。注意:DLL 跟宿主同进程,插件里崩了\n"
                   << "      整个程序一起完蛋,装谁的插件风险自担。\n"
                   << "  Lua:       " << dir << "/*.lua\n"
                   << "      每个文件 return { name=..., description=..., input_schema=...,\n"
                   << "      execute=function(input) ... end } 一张表,示例在 examples/plugins/word_count.lua。\n";
        return;
    }
    if (!mounted.empty()) {
        std::cout << "已挂载 " << mounted.size() << " 个插件工具:\n";
        for (const auto& info : mounted) {
            std::cout << "  - " << info.tool_name << "  (" << info.kind << ")\n";
        }
    }
    if (!warnings.empty()) {
        std::cout << "加载警告(这些没挂上):\n";
        for (const auto& warning : warnings) {
            std::cout << "  - " << warning << "\n";
        }
    }
}

// /mcp 命令:每个服务器一行状态(运行中/已退出)+ 工具数,底下缩进列出
// 完整工具名(mcp__服务器名__工具名,跟模型实际看到的名字一致)。
void PrintMcpCommand(const std::vector<McpServerRuntime>& mcp_servers) {
    if (mcp_servers.empty()) {
        std::cout << "没有挂载任何 MCP 服务器(config.json 里没写 mcpServers,或者配了但全部启动失败)。\n";
        return;
    }
    for (const auto& runtime : mcp_servers) {
        const bool alive = runtime.client != nullptr && runtime.client->Alive();
        std::cout << "  - " << runtime.name << ": " << (alive ? "运行中" : "已退出") << ", " << runtime.tools.size()
                   << " 个工具\n";
        for (const auto& tool_info : runtime.tools) {
            std::cout << "      mcp__" << runtime.name << "__" << tool_info.name << "\n";
        }
    }
}

// /lsp 命令:每个配置了的语言一行状态(未启动/运行中/已闲置关停/已退出)。
// StatusList() 要顺手收割闲置进程(改内部状态),所以入参是可变引用,
// 不装 const。
void PrintLspCommand(std::optional<lubancode::lsp::Manager>& lsp_manager) {
    if (!lsp_manager.has_value()) {
        std::cout << "没有配置任何 LSP 服务器(config.json 里没写 lsp 段,lsp 工具也没注册)。\n";
        return;
    }
    const auto statuses = lsp_manager->StatusList();
    std::cout << "已配置 " << statuses.size() << " 个 LSP 服务器:\n";
    for (const auto& status : statuses) {
        std::cout << "  - " << status.language << " (" << status.command << "): " << status.state << "\n";
    }
}

// 打印一段文本的前几行,超过就注明省略了多少行。给确认前的改动摘要用。
void PrintFirstLines(const std::string& text, int max_lines) {
    std::vector<std::string> lines;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }
    if (lines.empty() && !text.empty()) {
        lines.push_back(text);  // 没有换行符的单行内容
    }
    const int total = static_cast<int>(lines.size());
    for (int i = 0; i < total && i < max_lines; ++i) {
        std::cout << "      " << lines[static_cast<std::size_t>(i)] << "\n";
    }
    if (total > max_lines) {
        std::cout << "      ...(共 " << total << " 行,已省略其余)\n";
    }
}

// 确认前把工具的入参打印清楚,好让人一眼看明白将要发生什么:
// write_file/edit_file 显示路径和内容/改动的前几行摘要,run_command 显示
// 完整命令,别的按通用 JSON 打印兜底。
void PrintConfirmDetails(const std::string& name, const nlohmann::json& input) {
    if (name == "write_file") {
        const std::string path = input.value("path", std::string());
        const std::string content = input.value("content", std::string());
        std::cout << "    路径: " << path << "\n";
        std::cout << "    内容(" << content.size() << " 字节),前几行:\n";
        PrintFirstLines(content, 5);
    } else if (name == "edit_file") {
        const std::string path = input.value("path", std::string());
        const std::string old_s = input.value("old_string", std::string());
        const std::string new_s = input.value("new_string", std::string());
        const bool replace_all = input.value("replace_all", false);
        std::cout << "    路径: " << path << (replace_all ? "  (replace_all=true,全部替换)" : "") << "\n";
        std::cout << "    - 旧文本:\n";
        PrintFirstLines(old_s, 3);
        std::cout << "    + 新文本:\n";
        PrintFirstLines(new_s, 3);
    } else if (name == "run_command") {
        const std::string command = input.value("command", std::string());
        const std::string shell = input.value("shell", std::string("powershell"));
        std::cout << "    命令(" << shell << "): " << command << "\n";
    } else {
        std::cout << "    参数: " << input.dump() << "\n";
    }
    std::cout.flush();
}

// ---------------------------------------------------------------------------
// UI-B(0.12.0):工具条目的控制台原地改写。
//
// 纯渲染(条目长什么样)在 cli/transcript.cpp 的 FormatTranscriptItem 里,
// 这里只管"画在哪、怎么改"的 Win32 锚点记账:每画一个条目记下起始行号和
// 行数;工具结束后回到起始行,整块清掉重画成终态。工具执行期间没有别的
// 输出流(流式文本只在 API 响应期),原地回写是安全的——唯一会往下垫内容
// 的两个场景(确认交互块、子代理条目画在 agent 条目下面)各有对策:
//   1. 确认块:待确认态画好后先 ReserveRows 在缓冲区底部预留足够行,免得
//      交互期间自然滚屏把锚点推歪;用户答完 TrimBelow 把确认块整个擦掉,
//      条目重新成为屏幕最后的内容,后续改写照常。
//   2. agent 条目:终态摘要固定一行,行数跟执行中帧一致,原地改写不用长高;
//      万一要长高而下面垫着内容(比如 ESC 打断提示),砍到原有行数——完整
//      信息反正都在 full_output 里,UI-C 的 Ctrl+E 能看全。
// enabled=false(管道/重定向)时所有方法都是空操作,管道模式的稳定纯文本
// 输出由 ToolDisplay 另走一条路。
class TranscriptPainter {
public:
    // expanded:UI-D(0.16.0)紧凑/详细全局开关的会话级状态(InteractiveLoop
    // 持有,Ctrl+O 翻转),指针判空兜底(AskOnce 不传,恒紧凑)。详细态下
    // 新条目/终态改写直接按展开版画(完整参数 + full_output 全文,行数多就
    // 整屏往下铺,滚动自然发生);FormatTranscriptItem 的 width>0 截断保证
    // 每行绝不物理折行,锚点记账照旧成立。
    TranscriptPainter(const lubancode::cli::Theme& theme, bool enabled, const bool* expanded = nullptr)
        : theme_(theme), enabled_(enabled), expanded_(expanded) {}

    TranscriptPainter(const TranscriptPainter&) = delete;
    TranscriptPainter& operator=(const TranscriptPainter&) = delete;

    // 画一个新条目(跟前面的输出空一行分隔),登记锚点。
    void PaintNew(const lubancode::cli::TranscriptItem& item) {
        if (!enabled_) {
            return;
        }
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
#ifdef _WIN32
        const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO info{};
        const std::string text = Render(item);
        if (!GetConsoleScreenBufferInfo(h_out, &info)) {
            std::cout << "\n" << text;
            std::cout.flush();
            return;
        }
        if (info.dwCursorPosition.X > 0) {
            std::cout << "\n";  // 流式正文多半没换行收尾,先把光标归位到行首
        }
        std::cout << "\n";  // 空行分隔
        std::cout.flush();
        if (!GetConsoleScreenBufferInfo(h_out, &info)) {
            std::cout << text;
            std::cout.flush();
            return;
        }
        int start_row = info.dwCursorPosition.Y;
        const int rows = lubancode::cli::CountLines(text);
        EnsureRoom(h_out, start_row, rows);
        SetConsoleCursorPosition(h_out, COORD{0, static_cast<SHORT>(start_row)});
        std::cout << text;
        std::cout.flush();
        anchors_.push_back(Anchor{item.id, start_row, rows});
#else
        std::cout << "\n" << Render(item);
        std::cout.flush();
#endif
    }

    // 原地改写一个已登记的条目(执行中 -> 待确认 -> 终态)。
    void Repaint(const lubancode::cli::TranscriptItem& item) {
        if (!enabled_) {
            return;
        }
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
#ifdef _WIN32
        Anchor* anchor = Find(item.id);
        if (anchor == nullptr) {
            return;
        }
        const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(h_out, &info)) {
            return;
        }
        const SHORT buffer_width = info.dwSize.X;
        const COORD saved_cursor = info.dwCursorPosition;
        std::string text = Render(item);
        int new_rows = lubancode::cli::CountLines(text);

        // 条目是不是屏幕上最后的内容(光标正停在条目下一行行首)。不是的话
        // 不能长高——往下多画会盖住垫在下面的输出,只能砍到原有行数。
        const bool at_tail =
            saved_cursor.X == 0 && static_cast<int>(saved_cursor.Y) == anchor->start_row + anchor->rows;
        if (!at_tail && new_rows > anchor->rows) {
            text = FirstNLines(text, anchor->rows);
            new_rows = anchor->rows;
        }

        const int rows_to_clear = (std::max)(anchor->rows, new_rows);
        if (at_tail) {
            int start_row = anchor->start_row;
            EnsureRoom(h_out, start_row, rows_to_clear);  // 里头会同步平移所有锚点
        }
        DWORD written = 0;
        for (int r = 0; r < rows_to_clear; ++r) {
            const COORD row_pos{0, static_cast<SHORT>(anchor->start_row + r)};
            FillConsoleOutputCharacterW(h_out, L' ', static_cast<DWORD>(buffer_width), row_pos, &written);
        }
        SetConsoleCursorPosition(h_out, COORD{0, static_cast<SHORT>(anchor->start_row)});
        std::cout << text;
        std::cout.flush();
        anchor->rows = new_rows;
        if (!at_tail) {
            SetConsoleCursorPosition(h_out, saved_cursor);  // 下面还垫着别的内容,光标放回去
        }
#else
        std::cout << Render(item);
        std::cout.flush();
#endif
    }

    // 把条目末尾到当前光标之间的行全部擦掉、光标回到条目末尾——确认交互块
    // (参数详情 + [y/a/N] 提示)答完之后收尾用,让条目重新成为屏幕最后的
    // 内容,后续原地改写不受它牵连。
    void TrimBelow(int item_id) {
        if (!enabled_) {
            return;
        }
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
#ifdef _WIN32
        Anchor* anchor = Find(item_id);
        if (anchor == nullptr) {
            return;
        }
        const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(h_out, &info)) {
            return;
        }
        const int end_row = anchor->start_row + anchor->rows;
        if (static_cast<int>(info.dwCursorPosition.Y) < end_row) {
            return;
        }
        DWORD written = 0;
        for (int r = end_row; r <= static_cast<int>(info.dwCursorPosition.Y); ++r) {
            const COORD row_pos{0, static_cast<SHORT>(r)};
            FillConsoleOutputCharacterW(h_out, L' ', static_cast<DWORD>(info.dwSize.X), row_pos, &written);
        }
        SetConsoleCursorPosition(h_out, COORD{0, static_cast<SHORT>(end_row)});
#endif
    }

    // 在缓冲区底部预留 rows 行(必要时主动滚屏、同步平移所有锚点)。确认
    // 交互开始前调一次,免得交互期间的自然滚屏让锚点失效。
    void ReserveRows(int rows) {
        if (!enabled_) {
            return;
        }
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
#ifdef _WIN32
        const HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(h_out, &info)) {
            return;
        }
        const COORD saved = info.dwCursorPosition;
        int row = saved.Y;
        const int before = row;
        EnsureRoom(h_out, row, rows);
        if (row != before) {
            SetConsoleCursorPosition(h_out, COORD{saved.X, static_cast<SHORT>(row)});
        }
#else
        (void)rows;
#endif
    }

private:
    struct Anchor {
        int item_id = 0;
        int start_row = 0;
        int rows = 0;
    };

    std::string Render(const lubancode::cli::TranscriptItem& item) const {
        const int width = lubancode::cli::DetectConsoleWidth().value_or(80);
        const bool expanded = expanded_ != nullptr && *expanded_;
        return lubancode::cli::FormatTranscriptItem(item, theme_, width, expanded);
    }

    static std::string FirstNLines(const std::string& text, int n) {
        std::string out;
        int count = 0;
        std::size_t pos = 0;
        while (pos < text.size() && count < n) {
            const std::size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) {
                out += text.substr(pos);
                out += "\n";
                return out;
            }
            out += text.substr(pos, nl - pos + 1);
            pos = nl + 1;
            ++count;
        }
        return out;
    }

#ifdef _WIN32
    Anchor* Find(int item_id) {
        for (auto& anchor : anchors_) {
            if (anchor.item_id == item_id) {
                return &anchor;
            }
        }
        return nullptr;
    }

    // 从 start_row 起要写 rows_needed 行,会不会撞到缓冲区最后一行?会的话
    // 先主动滚够行数,再把所有登记过的锚点(和调用方手里这个 start_row)
    // 一起往上平移——跟 console_input.cpp 的 EnsureRoomForRows 同一套账。
    void EnsureRoom(HANDLE h_out, int& start_row, int rows_needed) {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(h_out, &info)) {
            return;
        }
        const int buffer_height = info.dwSize.Y;
        const int needed_bottom = start_row + rows_needed - 1;
        if (needed_bottom < buffer_height) {
            return;
        }
        const int overflow = needed_bottom - buffer_height + 1;
        SetConsoleCursorPosition(h_out, COORD{0, static_cast<SHORT>(buffer_height - 1)});
        for (int i = 0; i < overflow; ++i) {
            std::cout << "\n";
        }
        std::cout.flush();
        for (auto& anchor : anchors_) {
            anchor.start_row = (std::max)(0, anchor.start_row - overflow);
        }
        start_row = (std::max)(0, start_row - overflow);
    }
#endif

    const lubancode::cli::Theme& theme_;
    bool enabled_;
    const bool* expanded_ = nullptr;  // UI-D:紧凑/详细会话级开关,见构造函数注释
    std::vector<Anchor> anchors_;
};

// 统计一个磁盘文件现在有多少行(write_file 覆盖前掐一下旧行数,给 +N -M
// 摘要用)。读不到(不存在/是目录/打不开)给 nullopt——"新文件"场景。
std::optional<int> FileLineCount(const std::string& path_utf8) {
    const std::filesystem::path path(
        std::u8string(reinterpret_cast<const char8_t*>(path_utf8.data()), path_utf8.size()));
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || std::filesystem::is_directory(path, ec)) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return lubancode::cli::CountLines(content);
}

// UI-C(0.13.0):读文件全文(二进制读入,当 UTF-8 字节串用),给 diff
// 预览当"旧内容"。读不到(不存在/是目录/打不开)给 nullopt——write_file
// 按新文件处理(全 + 新增),edit_file 走回退对比,绝不因此崩。
std::optional<std::string> ReadFileBytes(const std::string& path_utf8) {
    const std::filesystem::path path(
        std::u8string(reinterpret_cast<const char8_t*>(path_utf8.data()), path_utf8.size()));
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || std::filesystem::is_directory(path, ec)) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// UI-C:预览截断双限——超 400 行或 32KiB 就截,截了标注省略行数,完整版
// 存进 TranscriptItem.full_output(那边另有 64KB 的库容上限)。
constexpr int kDiffPreviewMaxLines = 400;
constexpr std::size_t kDiffPreviewMaxBytes = 32 * 1024;

// UI-C:终态条目里留存的 diff 行数上限——git 那种效果,diff 直接挂在
// ⎿ 摘要底下不擦,但条目不能无限铺屏,超了标注省略(Ctrl+E 看全)。
constexpr int kDiffFinalMaxLines = 24;
constexpr std::size_t kDiffFinalMaxBytes = 8 * 1024;

// UI-C:一份拼装好的 diff 预览。colored 是直接可打印的整块(路径行 +
// diff 标题行 + diff 正文,每行缩进四空格、带主题色、按宽/行/字节截断);
// full 是 plain 全量(不截行、不截字节、不带色),终态并进 full_output
// 给 Ctrl+E 聚焦查看看全;final_lines 是终态条目摘要里留存的 diff
// (行数收紧到 kDiffFinalMaxLines,预先按宽截好——夹 ANSI 的行渲染层
// 不再截宽,物理折行会毁掉原地改写的行数记账)。
struct FileDiffPreview {
    std::string colored;
    std::string full;
    std::vector<std::string> final_lines;
    int line_count = 0;  // colored 的行数,ReserveRows 记账用
};

// 按 edit_file/write_file 的入参拼 diff 预览;别的工具给 nullopt。读旧
// 文件在这一层做(tools/ 层一字不动):edit_file 拿真文件内容做整文替换
// 后对比(变更处自带 ±3 行真实上下文和行号),找不到 old_string 回退成
// 只比 old/new 两段;write_file 旧文件存在做行级对比,不存在全部算新增。
std::optional<FileDiffPreview> BuildFileDiffPreview(const std::string& name, const nlohmann::json& input,
                                                     const lubancode::cli::Theme& theme) {
    namespace cli = lubancode::cli;
    if (name != "write_file" && name != "edit_file") {
        return std::nullopt;
    }
    const std::string path = input.value("path", std::string());
    const std::optional<std::string> old_content = ReadFileBytes(path);

    std::vector<cli::DiffLine> diff;
    std::string header;
    if (name == "edit_file") {
        const bool replace_all = input.value("replace_all", false);
        auto edit = cli::BuildEditDiff(old_content.value_or(std::string()), input.value("old_string", std::string()),
                                        input.value("new_string", std::string()), replace_all);
        diff = std::move(edit.lines);
        if (!edit.located) {
            header = "diff(old_string 在文件里没找到,只对比新旧两段):";
        } else if (replace_all) {
            header = "diff(replace_all,替换 " + std::to_string(edit.replaced_count) + " 处):";
        } else {
            header = "diff:";
        }
    } else {
        diff = cli::BuildWriteDiff(old_content, input.value("content", std::string()));
        header = old_content.has_value() ? "diff(覆盖已有文件):" : "diff(新文件,全部新增):";
    }

    const int width = cli::DetectConsoleWidth().value_or(80);
    // 每行缩四空格,diff 行本身的宽度上限就得让出这四列(再留一列,免得
    // 顶格写到最后一格触发控制台自动换行、毁掉行数记账)。
    const std::string body = cli::FormatDiff(diff, theme, width - 5, kDiffPreviewMaxLines, kDiffPreviewMaxBytes);

    FileDiffPreview out;
    out.full = header + "\n" +
               cli::FormatDiff(diff, cli::BuiltinTheme("plain"), /*width=*/0, /*max_lines=*/0, /*max_bytes=*/0);

    // 终态条目里留存的那份:行数收紧,宽度给 ⎿ 前缀让出十列(子代理条目
    // 再缩四格也够用)。
    {
        const std::string final_body =
            cli::FormatDiff(diff, theme, width - 10, kDiffFinalMaxLines, kDiffFinalMaxBytes);
        std::size_t p = 0;
        while (p < final_body.size()) {
            std::size_t nl = final_body.find('\n', p);
            if (nl == std::string::npos) {
                nl = final_body.size();
            }
            out.final_lines.push_back(final_body.substr(p, nl - p));
            p = nl + 1;
        }
    }

    const std::string block = "路径: " + path + "\n" + header + "\n" + body;
    std::string indented;
    std::size_t pos = 0;
    while (pos < block.size()) {
        std::size_t nl = block.find('\n', pos);
        if (nl == std::string::npos) {
            nl = block.size();
        }
        indented += "    " + block.substr(pos, nl - pos) + "\n";
        pos = nl + 1;
    }
    out.colored = std::move(indented);
    out.line_count = cli::CountLines(out.colored);
    return out;
}

// UI-B(0.12.0):一轮 Run() 里工具调用的展示总管。回调层(BuildCallbacks)
// 只管把事件转进来,这里统一负责:
//   - 建/更新 TranscriptItem(会话级 transcript vector 持有,UI-C/D 的
//     Ctrl+E 全文查看、回放都要用,full_output 现在就存好);
//   - 真控制台:走 TranscriptPainter 画条目、原地改写状态;
//   - 管道/重定向:保持稳定纯文本——启动一行 "[工具] name {...}"(现状),
//     结束一行 "[工具完成] name: 摘要"(新增,不回写、不夹 ANSI)。
// 计时(run_command 耗时)、行统计(write/edit 的 +N -M)全在这一层做,
// tools/ 层一个字不动。
struct ToolDisplay {
    ToolDisplay(std::vector<lubancode::cli::TranscriptItem>& transcript_ref, const lubancode::cli::Theme& theme_ref,
                bool console, std::shared_ptr<lubancode::tools::TodoListState> todo,
                const std::atomic<bool>* cancel, const bool* expanded = nullptr)
        : transcript(transcript_ref),
          theme(theme_ref),
          is_console(console),
          painter(theme_ref, console, expanded),
          todo_state(std::move(todo)),
          cancel_flag(cancel) {}

    std::vector<lubancode::cli::TranscriptItem>& transcript;
    const lubancode::cli::Theme& theme;
    bool is_console;
    TranscriptPainter painter;
    std::shared_ptr<lubancode::tools::TodoListState> todo_state;
    const std::atomic<bool>* cancel_flag = nullptr;

    // 主工具、子代理内层工具都是严格串行的,各留一个"进行中"槽位就够
    // (agent 工具执行期间 active_main 指着 agent 条目,active_sub 指着
    // 它肚子里正在跑的那个)。
    int active_main = -1;  // transcript 下标,-1 = 没有进行中的主工具
    int active_sub = -1;
    nlohmann::json main_input;
    nlohmann::json sub_input;
    std::optional<int> main_write_old_lines;
    std::optional<int> sub_write_old_lines;
    int agent_rounds = 0;
    int agent_sub_tools = 0;
    // UI-C:确认前 diff 预览的记账。*_diff_full 存 plain 全量,终态并进
    // full_output;*_diff_final 是终态条目摘要里留存的 diff 行(git 那种
    // 效果,成功后 diff 挂在 ⎿ 块底下不消失);*_preview_below 标记"自动
    // 放行路子里预览还垫在条目下面",工具执行完 TrimBelow 擦掉(确认路子
    // 的预览由 OnConfirmAnswered 的 TrimBelow 顺手带走,不用这个标记)。
    std::string main_diff_full;
    std::string sub_diff_full;
    std::vector<std::string> main_diff_final;
    std::vector<std::string> sub_diff_final;
    bool main_preview_below = false;
    bool sub_preview_below = false;

    void OnToolStart(const std::string& name, const nlohmann::json& input) {
        if (!is_console) {
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            std::cout << "\n" << theme.tool_line << "[工具] " << name << " " << input.dump() << theme.reset << "\n";
            std::cout.flush();
        }
        main_input = input;
        main_write_old_lines =
            name == "write_file" ? FileLineCount(input.value("path", std::string())) : std::nullopt;
        main_diff_full.clear();
        main_diff_final.clear();
        main_preview_below = false;
        if (name == "agent") {
            agent_rounds = 0;
            agent_sub_tools = 0;
        }
        active_main = NewItem(lubancode::cli::TranscriptKind::Tool, name, input);
        if (is_console) {
            painter.PaintNew(transcript[static_cast<std::size_t>(active_main)]);
        }
    }

    void OnToolDone(const std::string& name, const lubancode::tools::Tool::Result& result) {
        if (active_main < 0) {
            return;
        }
        auto& item = transcript[static_cast<std::size_t>(active_main)];
        // UI-C:自动放行时垫在条目下面的 diff 预览,执行完了就擦——终态只
        // 留 +N -M 简短摘要,不铺屏(确认路子的预览已被 OnConfirmAnswered
        // 的 TrimBelow 带走,标记不会是 true)。
        if (main_preview_below && is_console) {
            painter.TrimBelow(item.id);
        }
        main_preview_below = false;
        FinalizeItem(item, name, main_input, result, main_write_old_lines, agent_rounds, agent_sub_tools,
                      main_diff_full, main_diff_final);
        if (is_console) {
            painter.Repaint(item);
        } else {
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            std::cout << "[工具完成] " << name << ": " << PipeSummary(item, name) << "\n";
            // 管道模式沿用 M11 的行为:todo_write 成功后紧跟着把清单打出来,
            // 重定向日志里"计划走到哪一步了"仍然可读。
            if (name == "todo_write" && !result.is_error && todo_state) {
                std::cout << lubancode::cli::FormatTodoList(todo_state->items, theme);
            }
            std::cout.flush();
        }
        active_main = -1;
    }

    void OnSubToolStart(const std::string& name, const nlohmann::json& input) {
        agent_sub_tools += 1;
        if (!is_console) {
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            std::cout << "\n"
                       << theme.stats << "  [子代理·工具] " << name << " " << input.dump() << theme.reset << "\n";
            std::cout.flush();
        }
        sub_input = input;
        sub_write_old_lines =
            name == "write_file" ? FileLineCount(input.value("path", std::string())) : std::nullopt;
        sub_diff_full.clear();
        sub_diff_final.clear();
        sub_preview_below = false;
        active_sub = NewItem(lubancode::cli::TranscriptKind::SubTool, name, input);
        if (is_console) {
            painter.PaintNew(transcript[static_cast<std::size_t>(active_sub)]);
        }
    }

    // 子工具真执行完(agent 工具转发的 post_tool 钩子)——终态回写。拒绝
    // 那条路走不到这里(post_tool 只在真执行后触发),由 OnConfirmAnswered
    // 定格成 Cancelled。
    void OnSubToolResult(const std::string& name, const nlohmann::json& input,
                          const lubancode::tools::Tool::Result& result) {
        (void)input;
        if (active_sub < 0) {
            return;
        }
        auto& item = transcript[static_cast<std::size_t>(active_sub)];
        if (sub_preview_below && is_console) {
            painter.TrimBelow(item.id);  // 理由同 OnToolDone
        }
        sub_preview_below = false;
        FinalizeItem(item, name, sub_input, result, sub_write_old_lines, 0, 0, sub_diff_full, sub_diff_final);
        if (is_console) {
            painter.Repaint(item);
        }
        active_sub = -1;
    }

    // 子工具被 pre_tool 钩子拦截(post_tool 不会再来了)——条目定格成失败态。
    void OnSubBlocked(const std::string& message) {
        if (active_sub < 0) {
            return;
        }
        auto& item = transcript[static_cast<std::size_t>(active_sub)];
        item.status = lubancode::cli::TranscriptStatus::Error;
        item.summary_lines = lubancode::cli::ErrorSummaryLines(item.tool_name, message);
        item.full_output = lubancode::cli::TruncateUtf8Bytes(message, lubancode::cli::kFullOutputCapBytes);
        item.end_time = std::chrono::steady_clock::now();
        if (is_console) {
            painter.Repaint(item);
        }
        active_sub = -1;
    }

    // UI-C:edit_file/write_file 确认前的统一 diff 预览,画在当前条目
    // (子工具优先)下面。trim_on_done=true 是自动放行那条路(auto/yolo/
    // --yes/选过 a)——打完不等确认,工具执行完 OnToolDone/OnSubToolResult
    // 里 TrimBelow 擦掉;false 是确认路子,预览随确认块一起被
    // OnConfirmAnswered 的 TrimBelow 带走。管道模式(is_console 为假)
    // 整个不打,保持稳定纯文本输出。
    void ShowDiffPreview(const std::string& name, const nlohmann::json& input, bool trim_on_done) {
        if (!is_console) {
            return;
        }
        const auto preview = BuildFileDiffPreview(name, input, theme);
        if (!preview.has_value()) {
            return;
        }
        const bool sub = active_sub >= 0;
        (sub ? sub_diff_full : main_diff_full) = preview->full;
        (sub ? sub_diff_final : main_diff_final) = preview->final_lines;
        // 预览可能有几百行,先在缓冲区底部把行数留够(外加确认块的余量),
        // 免得打印期间自然滚屏把锚点推歪。ReserveRows 自己拿 stdout 锁,
        // 不能包在下面那把锁里(std::mutex 不可重入)。
        painter.ReserveRows(preview->line_count + 24);
        {
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            std::cout << preview->colored;
            std::cout.flush();
        }
        if (trim_on_done) {
            (sub ? sub_preview_below : main_preview_below) = true;
        }
    }

    // 确认真的要问出口了(自动放行的几条路都没走到)——条目改成"待确认"态,
    // 确认块(参数详情 + [y/a/N])跟在条目下面打。返回该条目的 transcript
    // 下标,答完交回 OnConfirmAnswered。
    int OnConfirmRequest() {
        const int idx = active_sub >= 0 ? active_sub : active_main;
        if (idx >= 0) {
            auto& item = transcript[static_cast<std::size_t>(idx)];
            item.status = lubancode::cli::TranscriptStatus::Pending;
            item.summary_lines = {"待确认"};
            if (is_console) {
                painter.Repaint(item);
                // 确认块 + 编辑器提示行撑死二十来行,先在缓冲区底部预留好,
                // 免得交互期间自然滚屏把锚点推歪。
                painter.ReserveRows(24);
            }
        }
        return idx;
    }

    void OnConfirmAnswered(int idx, bool allowed) {
        if (idx < 0) {
            return;
        }
        auto& item = transcript[static_cast<std::size_t>(idx)];
        if (is_console) {
            painter.TrimBelow(item.id);  // 确认块用完就擦,条目回到屏幕末尾
        }
        if (allowed) {
            item.status = lubancode::cli::TranscriptStatus::Running;
            item.summary_lines = {"Running..."};
        } else {
            item.status = lubancode::cli::TranscriptStatus::Cancelled;
            item.summary_lines = {"Cancelled"};
            if (idx == active_sub) {
                active_sub = -1;  // 子工具拒绝后 post_tool 钩子不会再来,槽位在这儿收掉
            }
        }
        if (is_console) {
            painter.Repaint(item);
        }
    }

private:
    int NewItem(lubancode::cli::TranscriptKind kind, const std::string& name, const nlohmann::json& input) {
        lubancode::cli::TranscriptItem item;
        item.id = static_cast<int>(transcript.size()) + 1;
        item.kind = kind;
        item.tool_name = name;
        item.title = lubancode::cli::BuildToolTitle(name, input);
        // UI-D:完整入参存档,展开版("参数: {...}")和 Ctrl+E 聚焦查看用。
        item.input_json = input.is_null() ? std::string() : input.dump();
        item.status = lubancode::cli::TranscriptStatus::Running;
        item.summary_lines = {"Running..."};
        item.start_time = std::chrono::steady_clock::now();
        transcript.push_back(std::move(item));
        return static_cast<int>(transcript.size()) - 1;
    }

    // 终态归档:状态 + 摘要 + full_output + 计时,一处算完。摘要规则:
    // run_command 退出码+耗时;read_file 行数;write/edit +N -M;search
    // 命中数;agent 子代理轮数/子工具次数;todo_write 接现成清单渲染;
    // MCP 和其余工具取结果第一行。失败态固定 "Error: ..." 开头,拒绝态
    // (确认回调里已定格)不再覆盖,ESC 打断标成 Interrupted。
    void FinalizeItem(lubancode::cli::TranscriptItem& item, const std::string& name, const nlohmann::json& input,
                       const lubancode::tools::Tool::Result& result, std::optional<int> write_old_lines,
                       int rounds, int sub_tools, const std::string& diff_full = std::string(),
                       const std::vector<std::string>& diff_final = {}) {
        namespace cli = lubancode::cli;
        item.end_time = std::chrono::steady_clock::now();
        // UI-C:有 diff 预览的(edit_file/write_file),完整 plain diff 跟着
        // 工具结果一起进 full_output——屏上的预览是要被擦掉的,Ctrl+E
        // 聚焦查看从这儿看全。
        item.full_output = cli::TruncateUtf8Bytes(
            diff_full.empty() ? result.content : result.content + "\n\n" + diff_full, cli::kFullOutputCapBytes);
        const double seconds = std::chrono::duration<double>(item.end_time - item.start_time).count();

        if (item.status == cli::TranscriptStatus::Cancelled) {
            return;  // 确认回调里已经定格成拒绝态,别拿 "用户拒绝执行该工具" 再盖一遍
        }
        if (result.is_error) {
            if (result.content == "用户拒绝执行该工具") {
                item.status = cli::TranscriptStatus::Cancelled;
                item.summary_lines = {"Cancelled"};
                return;
            }
            item.status = cli::TranscriptStatus::Error;
            item.summary_lines = cli::ErrorSummaryLines(name, result.content);
            return;
        }
        if (cancel_flag != nullptr && cancel_flag->load()) {
            item.status = cli::TranscriptStatus::Interrupted;
            item.summary_lines = {"Interrupted"};
            return;
        }

        item.status = cli::TranscriptStatus::Ok;
        if (name == "run_command") {
            item.summary_lines = {cli::RunCommandDoneSummary(result.content, seconds)};
        } else if (name == "read_file") {
            item.summary_lines = {cli::ReadFileDoneSummary(result.content)};
        } else if (name == "write_file") {
            item.summary_lines = {
                cli::WriteDiffSummary(cli::CountLines(input.value("content", std::string())), write_old_lines)};
            // UI-C:终态把 diff 留在条目里(git 那种效果)——首行 +N -M,
            // 底下接 diff 正文(建预览时已按宽截好、行数收紧,超长有
            // Ctrl+E 标注)。管道模式没建预览,diff_final 是空的,摘要
            // 保持一行 +N -M 不变。
            item.summary_lines.insert(item.summary_lines.end(), diff_final.begin(), diff_final.end());
        } else if (name == "edit_file") {
            item.summary_lines = {
                cli::WriteDiffSummary(cli::CountLines(input.value("new_string", std::string())),
                                       cli::CountLines(input.value("old_string", std::string())))};
            item.summary_lines.insert(item.summary_lines.end(), diff_final.begin(), diff_final.end());
        } else if (name == "search") {
            item.summary_lines = {cli::SearchDoneSummary(result.content)};
        } else if (name == "agent") {
            item.summary_lines = {cli::AgentDoneSummary(rounds, sub_tools)};
        } else if (name == "todo_write" && todo_state) {
            // 沿用现有清单渲染,清单接在 ⎿ 之后(FormatTodoList 每行自带的
            // 两空格缩进剥掉,条目渲染自己管缩进)。
            item.summary_lines.clear();
            const std::string rendered = cli::FormatTodoList(todo_state->items, theme);
            std::size_t pos = 0;
            while (pos < rendered.size()) {
                std::size_t nl = rendered.find('\n', pos);
                if (nl == std::string::npos) {
                    nl = rendered.size();
                }
                std::string line = rendered.substr(pos, nl - pos);
                if (line.compare(0, 2, "  ") == 0) {
                    line.erase(0, 2);
                }
                if (!line.empty()) {
                    item.summary_lines.push_back(std::move(line));
                }
                pos = nl + 1;
            }
        } else {
            // MCP(mcp__server__tool)和其余工具:结果前一行当摘要。
            std::string first_line = result.content.substr(0, result.content.find('\n'));
            if (first_line.empty()) {
                first_line = "Done";
            }
            item.summary_lines = {std::move(first_line)};
        }
    }

    // 管道模式那行 "[工具完成] name: 摘要" 的摘要——不回写、不夹 ANSI,
    // 取条目摘要第一行;todo_write 的摘要是清单本身,换成一句人话。
    std::string PipeSummary(const lubancode::cli::TranscriptItem& item, const std::string& name) const {
        if (name == "todo_write" && item.status == lubancode::cli::TranscriptStatus::Ok && todo_state) {
            return "已更新待办清单(" + std::to_string(todo_state->items.size()) + " 项)";
        }
        if (item.summary_lines.empty()) {
            return "Done";
        }
        return item.summary_lines.front();
    }
};

// 一次 Run() 内(可能因为工具调用来回好几趟)的 token 用量累计:输入、
// 输出 tokens 各自求和,再数一下总共发了几次独立请求。RunTurn() 结束后
// 打一行,不跨多次用户提问累计——一问一答算一次统计。
struct UsageStats {
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    int request_count = 0;
};

// 交互循环、单发模式共用的回调:文本打字机打印(正文保持原色,不着色),
// 工具调用打一行提示,needs_confirm 的工具按 auto_confirm 决定是自动放行
// 还是问用户一句(三选:y 本次允许 / a 本会话总是允许该工具 / N 拒绝)。
// always_allowed_tools 由调用方持有,跨多轮 Run() 保留,选过 a 的工具本
// 会话内不会再问。usage_stats 由调用方持有,只在这一次 Run() 范围内累计
// (RunTurn() 每次都会传一份新的进来)。registry 是这一轮实际在用的工具表——
// 如果里面注册了 "agent" 工具,这里顺带把这一轮现算好的确认/记账/打印
// 逻辑通过 SetHooks 灌给它,子代理被调用时就能用上同一套(详见
// tools/agent_tool.hpp 顶部注释)。
// display:UI-B(0.12.0)新增,这一轮的工具条目展示总管(建条目、原地
// 改写状态、管道模式的 [工具]/[工具完成] 稳定纯文本),todo_state 也归它
// 持有。回调层只管把事件原样转进去。
lubancode::agent::Callbacks BuildCallbacks(bool auto_confirm, std::set<std::string>& always_allowed_tools,
                                            const lubancode::cli::Theme& theme, UsageStats& usage_stats,
                                            lubancode::cli::ContextTracker& context_tracker,
                                            lubancode::tools::ToolRegistry& registry,
                                            const lubancode::config::HooksConfig& hooks_config,
                                            ToolDisplay& display) {
    lubancode::agent::Callbacks callbacks;

    // M9:hooks_config 四个数组全空是常态(没配 hooks 的用户占多数),这时候
    // 干脆不设这两个回调——跟"没有 hooks 系统"时行为完全一样,也省得每次
    // 工具调用都白跑一趟"遍历空数组"。
    if (!hooks_config.Empty()) {
        callbacks.on_pre_tool_hook = [&hooks_config](const std::string& name,
                                                       const nlohmann::json& input) -> std::optional<std::string> {
            const auto outcome = lubancode::tools::RunPreToolHooks(hooks_config, name, input);
            if (outcome.intercepted) {
                return outcome.block_message;
            }
            return std::nullopt;
        };
        callbacks.on_post_tool_hook = [&hooks_config](const std::string& name, const nlohmann::json& input,
                                                        const lubancode::tools::Tool::Result& result) {
            lubancode::tools::RunPostToolHooks(hooks_config, name, input, result);
        };
    }

    // M10:这几处 std::cout 写都发生在"流式期间"(Run() 还没返回),跟
    // TurnInputListener 监听线程自己打的 "[已打断]"/"[已排队] ..." 抢同一份
    // stdout——拿 StdoutWriteMutex 包一下,两边错开,免得真终端上偶发交错
    // 把画面弄花。管道模式下监听线程压根不起,这把锁形同虚设,不影响行为。
    callbacks.on_text_delta = [](const std::string& text) {
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        std::cout << text;
        std::cout.flush();
    };

    // UI-B:工具条目化渲染,建条目/画条目/管道行全在 ToolDisplay 里。
    callbacks.on_tool_start = [&display](const std::string& name, const nlohmann::json& input) {
        display.OnToolStart(name, input);
    };

    callbacks.on_tool_confirm = [auto_confirm, &always_allowed_tools, &theme,
                                  &display](const std::string& name, const nlohmann::json& input) -> bool {
        // 会话级确认模式(Shift+Tab 循环切),跟 --yes/auto_confirm 叠加:
        //   yolo  —— 全自动放行(auto_confirm 本来就是这个语义,这里再查一遍
        //             CurrentConfirmMode() 是为了让 Shift+Tab 中途切到 yolo
        //             也立刻生效,不用等下一轮 --yes)
        //   auto  —— write_file/edit_file 自动放行,run_command 之类仍然要问
        //   confirm(默认)—— 老规矩,needs_confirm 的工具逐个问
        const lubancode::cli::ConfirmMode mode = lubancode::cli::CurrentConfirmMode();
        const bool file_tool = name == "write_file" || name == "edit_file";
        const bool auto_pass = auto_confirm || mode == lubancode::cli::ConfirmMode::Yolo ||
                               (mode == lubancode::cli::ConfirmMode::Auto && file_tool) ||
                               always_allowed_tools.count(name) != 0;
        if (auto_pass) {
            // UI-C:自动放行(--yes/yolo/auto 档的文件工具/选过 a)也把统一
            // diff 预览打出来——用户看得见将要发生什么,但不停下等确认,
            // 打完即执行;执行完预览被 TrimBelow 擦掉,条目只留 +N -M。
            // 管道模式 ShowDiffPreview 内部直接返回,输出照旧是稳定纯文本。
            if (file_tool) {
                display.ShowDiffPreview(name, input, /*trim_on_done=*/true);
            }
            return true;
        }
        // UI-B:真的要问了——条目先改成"待确认"态(黄灯 + 待确认),确认块
        // (参数详情 + [y/a/N] 提示)跟在条目下面;答完确认块整个擦掉,
        // 拒绝则条目原地改灰 Cancelled,允许则改回 Running 等终态。
        // UI-C:edit_file/write_file 在真控制台下,参数详情换成统一 diff
        // 预览(路径 + 行级 diff,- 红 + 绿),answered 后随确认块一起擦;
        // 管道模式沿用老的参数摘要,不打 diff。
        const int pending_idx = display.OnConfirmRequest();
        if (file_tool && display.is_console) {
            display.ShowDiffPreview(name, input, /*trim_on_done=*/false);
        } else {
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            PrintConfirmDetails(name, input);
        }
        // M10:esc_rejects=true——按 Esc 直接当这次确认提交了一个空串,
        // 走到下面 "不是 y/Y/a/A 就算拒绝" 那条老路,不用另加判断分支。
        const std::optional<std::string> answer = lubancode::cli::ReadLine(
            theme.confirm + "[y] 本次允许  [a] 本会话总是允许(该工具)  [N] 拒绝: " + theme.reset, theme,
            /*esc_rejects=*/true);
        bool allowed = false;
        if (answer.has_value()) {  // 读到 EOF 按拒绝处理,不要在这儿卡住
            if (*answer == "a" || *answer == "A") {
                always_allowed_tools.insert(name);
                allowed = true;
            } else {
                allowed = (*answer == "y" || *answer == "Y");
            }
        }
        display.OnConfirmAnswered(pending_idx, allowed);
        return allowed;
    };

    callbacks.on_tool_done = [&display](const std::string& name, const lubancode::tools::Tool::Result& result) {
        display.OnToolDone(name, result);
    };

    callbacks.on_usage = [&usage_stats, &context_tracker](const lubancode::api::Usage& usage) {
        usage_stats.input_tokens += usage.input_tokens;
        usage_stats.output_tokens += usage.output_tokens;
        usage_stats.cache_read_tokens += usage.cache_read_tokens;
        usage_stats.request_count += 1;
        // ContextTracker 只认"最近一次请求"的真实用量,整个覆盖,不跟着
        // usage_stats 一起累加——语义区别见 cli/context_tracker.hpp 文件头。
        context_tracker.Update(usage);
    };

    // agent 工具(注册了的话)需要这一轮现算好的转发逻辑:确认回调直接
    // 转发父级那份(三档确认模式照管子代理);usage 累进 usage_stats(统计
    // 行的请求次数、输入输出 token 都要算上子代理那几次请求)但不动
    // context_tracker——子代理是完全独立的上下文,它的用量跟"主对话历史
    // 占用多大"是两回事,冲进去反而会把 /context 的数字带偏成子代理那次
    // 请求的大小,而不是主对话真实占用。
    if (auto* agent_tool = dynamic_cast<lubancode::tools::AgentTool*>(registry.Find("agent"));
        agent_tool != nullptr) {
        lubancode::tools::AgentTool::Hooks hooks;
        hooks.on_tool_confirm = callbacks.on_tool_confirm;
        // UI-B:子代理内层工具也走条目样式(前缀缩进四空格),状态同样原地
        // 更新——启动靠 on_sub_tool_start,终态靠下面包了一层的 post_tool
        // 钩子(agent 工具没有单独的"子工具结束"回调,post_tool 正好在真
        // 执行完之后带着 Result 触发一次,借它回写;拒绝那条路在确认回调里
        // 已经定格,pre_tool 拦截另包一层)。
        hooks.on_sub_tool_start = [&display](const std::string& name, const nlohmann::json& input) {
            display.OnSubToolStart(name, input);
        };
        hooks.on_usage = [&usage_stats, &display](const lubancode::api::Usage& usage) {
            usage_stats.input_tokens += usage.input_tokens;
            usage_stats.output_tokens += usage.output_tokens;
            usage_stats.cache_read_tokens += usage.cache_read_tokens;
            usage_stats.request_count += 1;
            display.agent_rounds += 1;  // 子代理每一次独立请求算一轮,agent 条目终态摘要用
        };
        // M9:pre_tool/post_tool 钩子照旧转发给父级同一份;UI-B 在外面再包
        // 一层,给子工具条目回写终态/拦截态用。
        if (callbacks.on_pre_tool_hook) {
            hooks.on_pre_tool_hook = [&display, base = callbacks.on_pre_tool_hook](
                                          const std::string& name,
                                          const nlohmann::json& input) -> std::optional<std::string> {
                std::optional<std::string> blocked = base(name, input);
                if (blocked.has_value()) {
                    display.OnSubBlocked(*blocked);
                }
                return blocked;
            };
        }
        hooks.on_post_tool_hook = [&display, base = callbacks.on_post_tool_hook](
                                       const std::string& name, const nlohmann::json& input,
                                       const lubancode::tools::Tool::Result& result) {
            if (base) {
                base(name, input, result);
            }
            display.OnSubToolResult(name, input, result);
        };
        agent_tool->SetHooks(std::move(hooks));
    }

    return callbacks;
}

// RunTurn() 的结果:status 沿用老语义(0 成功、非 0 出错);cancelled 标记
// 这一轮是不是被 ESC 打断的(打断不算错误,status 照样是 0);queued_lines
// 是这一轮"流式期间"(监听线程存活的窗口)攒下的排队消息,按落队顺序,
// 交给 InteractiveLoop 追加进它自己的队列,下一轮循环逐条自动发出。
struct RunTurnResult {
    int status = 0;
    bool cancelled = false;
    std::vector<std::string> queued_lines;
};

// 发一轮用户输入,走 agent loop(可能会有若干次工具调用来回),流式打字机
// 打印回复,结束后打一行 token 用量统计(暗色/淡色,plain 主题下就是空
// 前后缀)。always_allowed_tools 由调用方持有,记录本会话内选过"总是允许"
// 的工具。registry 是这一轮实际在用的工具表,传给 BuildCallbacks 好给里头
// 的 agent 工具(如果有)灌这一轮的转发钩子。
//
// M10:这里起一条 TurnInputListener,存活区间正好是"发出请求到本轮 Run()
// 结束"——ESC 打断、消息排队都靠它。真控制台之外(管道/重定向)监听器
// 构造函数自己判断不起线程,行为跟 0.7.0 完全一致。
// is_console:M11(0.10.0)新增,决定要不要打输入/输出分界线(管道/重定向
// 模式恒为假,分界线完全不出现,不污染被重定向的输出)。todo_state 同样
// M11 新增,转发给 BuildCallbacks 给 on_tool_done 用;留空指针表示这一轮
// 的 registry 没注册 todo_write(目前两个调用点都注册了,这个默认值只是
// 留个口子)。
// transcript:UI-B(0.12.0)新增,会话级工具条目存档(InteractiveLoop/
// AskOnce 各持有一份,跨多轮累积),UI-C/D 的 Ctrl+E 全文查看要用。
// transcript_expanded:UI-D(0.16.0)紧凑/详细会话级开关(Ctrl+O 翻转,
// InteractiveLoop 持有),详细态下这一轮新画的条目直接按展开版画;AskOnce
// 不传(nullptr),恒紧凑。
RunTurnResult RunTurn(lubancode::agent::AgentLoop& loop, const std::string& user_input, bool auto_confirm,
                       std::set<std::string>& always_allowed_tools, const lubancode::cli::Theme& theme,
                       lubancode::cli::ContextTracker& context_tracker, lubancode::tools::ToolRegistry& registry,
                       const lubancode::config::HooksConfig& hooks_config, bool is_console,
                       std::vector<lubancode::cli::TranscriptItem>& transcript,
                       std::shared_ptr<lubancode::tools::TodoListState> todo_state = nullptr,
                       const bool* transcript_expanded = nullptr) {
    UsageStats usage_stats;
    // cancel_flag 先于 display/callbacks 建:ToolDisplay 要拿它判断"这一轮
    // 是不是被 ESC 打断的"(打断态条目标 Interrupted)。
    std::atomic<bool> cancel_flag{false};
    ToolDisplay display(transcript, theme, is_console, todo_state, &cancel_flag, transcript_expanded);
    const lubancode::agent::Callbacks callbacks =
        BuildCallbacks(auto_confirm, always_allowed_tools, theme, usage_stats, context_tracker, registry,
                        hooks_config, display);

    lubancode::cli::TurnInputListener listener(cancel_flag, theme);

    // 用户这一行已经提交、真要开始等模型作答了——分界线打在这儿,紧跟在
    // 提示符那一行之后、模型正文开始打字机输出之前。
    PrintDivider(theme, is_console);

    const auto result = loop.Run(user_input, callbacks, &cancel_flag);

    // Run() 已经返回,不管是不是被打断——立刻收线程,保证下一次 ReadLine()
    // (排队回显的 "> " 或者下一轮主提示符)开始之前监听线程已经彻底退出。
    listener.Stop();

    RunTurnResult out;
    out.queued_lines = listener.TakeQueuedLines();

    std::cout << "\n";

    if (!result.has_value()) {
        std::cerr << theme.error << "[错误] " << result.error() << theme.reset << "\n";
        out.status = 1;
        return out;
    }
    out.cancelled = result->cancelled;

    if (usage_stats.request_count > 0) {
        // 0.17.0:token 数字统一 k 化(cli::FormatTokenCount),超过 10k 的
        // 数字不再铺一长串数位。
        std::cout << theme.stats << "[tokens] 输入 " << lubancode::cli::FormatTokenCount(usage_stats.input_tokens);
        if (usage_stats.cache_read_tokens > 0) {
            std::cout << "(缓存命中 " << lubancode::cli::FormatTokenCount(usage_stats.cache_read_tokens) << ")";
        }
        std::cout << " · 输出 " << lubancode::cli::FormatTokenCount(usage_stats.output_tokens) << " · 请求 "
                   << usage_stats.request_count << " 次"
                   << " · context " << context_tracker.UsagePercent() << "%" << theme.reset << "\n";
    }
    // 回合正常结束(不是上面那条 !result.has_value() 的报错早退)——统计行
    // 之后再打一条分界线,跟开头那条首尾呼应,把这一问一答框完整。
    PrintDivider(theme, is_console);
    return out;
}

// 初次配置向导:接 cli::ReadLine 做输入、std::cout 做输出、api::ListModels
// 做模型列表拉取。用户中途 EOF(Ctrl+Z / 管道读尽)放弃时返回 std::nullopt。
// 用户选择保存时,把保存后的路径写进 out_config_file_path,好让接下来的
// /model 命令知道"有配置文件"。
std::optional<lubancode::config::Config> RunInitialSetupWizard(std::optional<std::string>& out_config_file_path) {
    lubancode::cli::WizardIO io;
    io.print = [](const std::string& line) {
        std::cout << line << "\n";
        std::cout.flush();
    };
    // prompt 已经由向导自己通过 print 打出来了,这里传空串,别让 ReadLine 再打一遍。
    io.read_line = []() -> std::optional<std::string> { return lubancode::cli::ReadLine(""); };
    io.fetch_models = [](lubancode::config::Wire wire, const std::string& base_url, const std::string& api_key) {
        return lubancode::api::ListModels(wire, base_url, api_key);
    };

    const auto home_lubancode_dir = lubancode::config::HomeLubancodeDir();
    io.home_config_display_path =
        (home_lubancode_dir.has_value() ? *home_lubancode_dir : std::string("<找不到主目录>/.lubancode")) +
        "/config.json";

    const auto outcome = lubancode::cli::RunSetupWizard(io);
    if (!outcome.has_value()) {
        return std::nullopt;
    }

    if (outcome->save_requested) {
        const auto saved = lubancode::config::SaveConfigFile(outcome->config);
        if (saved.has_value()) {
            std::cout << "已保存到 " << *saved << "\n";
            out_config_file_path = *saved;
        } else {
            std::cout << "保存失败: " << saved.error() << "(不影响本次继续用,只是这份配置这次没记住)\n";
        }
    }
    return outcome->config;
}

void PrintSlashHelp() {
    std::cout << "可用命令:\n"
              << "  /help           列出所有命令\n"
              << "  /model          拉取模型列表,编号选择切换(默认第一个)\n"
              << "  /model 名字     直接切到指定模型名,不用拉列表\n"
              << "  /config         打印当前生效配置(api_key 打码),外加本会话实际在用的 model\n"
              << "  /clear          清空对话历史\n"
              << "  /context        看当前上下文占用;/context 256k|512k|1m 临时改窗口大小\n"
              << "  /compact        手动压缩历史;/compact 重点说明 可指定这次额外保留什么\n"
              << "  /think          看当前推理强度;/think 档位 切档位,档位以服务商为准(/effort 同义)\n"
              << "  /skills         列出扫描到的技能(主目录级 + 项目级)\n"
              << "  /mcp            列出挂载的 MCP 服务器状态和工具清单\n"
              << "  /lsp            列出各语言 LSP 服务器状态(未启动/运行中/已闲置关停)\n"
              << "  /todos          查看当前待办清单(todo_write 工具维护的那份)\n"
              << "  /plugins        列出挂载的插件工具(DLL + lua)和加载警告\n"
              << "  /tools          列工具三态:核心(恒在)/已加载/延迟未加载(tool_search 延迟挂载)\n"
              << "  /sessions       列最近 20 场会话存档(时间倒序编号)\n"
              << "  /resume 编号或id  载入该场存档历史续聊,后续消息追加写回同一文件\n"
              << "  /export [路径]  当前会话导出 Markdown(默认 sessions/<id>.md)\n"
              << "  /sessions       列本目录最近 20 场会话存档(时间倒序编号);/sessions all 列全部目录\n"
              << "  /resume 编号或id  载入该场存档历史续聊(编号按本目录列表数),后续消息追加写回同一文件\n"
              << "  /export [路径]  当前会话导出 Markdown(默认 sessions/<id>.md;全量流水,压缩点带标注)\n"
              << "  /title [标题]   看/设本场会话标题,/sessions 列表和 /export 大标题都用它\n"
              << "  /soul           列出可用的魂;/soul 名字 切换(即时生效),/soul off 关,\n"
              << "                  /soul default 回 SOUL.md\n"
              << "  /prompt         看当前法(系统提示词)的来源和字数;/prompt reset 还原 system_prompt.md\n"
              << "  /exit           退出(裸词 exit/quit 也认)\n"
              << "多行输入:Shift+Enter 插换行(Alt+Enter 同义,但 Windows Terminal 默认把它绑成全屏\n"
              << "切换、会吞掉,推荐 Shift+Enter);Enter 发送整段;多行时首行的 / 是正文,不当命令。\n"
              << "候选菜单:/ 开头时按 ↓ 进入直选(↓↑ 循环移动,Enter 执行选中命令、已敲的参数尾巴\n"
              << "原样保留;打字/退格/ESC 回普通编辑);Tab 补全/轮转照旧。\n"
              << "条目查看:Ctrl+O 紧凑/详细全局切换(详细 = 完整参数 + 输出/diff 全文,整块重打);\n"
              << "Shift+Tab 任何时候都是切确认档(confirm/auto/yolo,状态行实时显示);输入框为空时\n"
              << "Tab 进入焦点态选最近一条,焦点态内 Tab 往旧走、Shift+Tab 往新走(这时不切档),\n"
              << "ESC/Enter 退出焦点态回编辑,有内容时 Tab 维持补全现职;Ctrl+E 聚焦查看焦点条目\n"
              << "全文(无焦点则最近一条),再按 Ctrl+E 或 ESC 返回;流式输出期间这些键不响应。\n";
}

// /skills 命令:列出扫描到的技能;一个都没有时打印两处目录路径,顺带说明
// 怎么造一份(SKILL.md 起手 frontmatter 的最小样例)。
void PrintSkillsCommand(const std::vector<lubancode::tools::SkillMeta>& skills, const std::string& project_dir,
                         const std::optional<std::string>& home_dir) {
    if (skills.empty()) {
        std::cout << "还没有扫描到任何技能。\n\n"
                   << "技能目录约定(先建目录,再放一份 <技能名>/SKILL.md):\n"
                   << "  项目级: " << project_dir << "/.lubancode/skills/<技能名>/SKILL.md\n"
                   << "  主目录级: " << (home_dir.has_value() ? *home_dir : std::string("<找不到主目录>"))
                   << "/.lubancode/skills/<技能名>/SKILL.md\n\n"
                   << "SKILL.md 起手要有 YAML frontmatter(name/description 两个字段,后面跟正文):\n"
                   << "  ---\n"
                   << "  name: 技能名\n"
                   << "  description: 一句话说明这个技能是干什么的、什么时候该用\n"
                   << "  ---\n"
                   << "  正文写具体怎么做。\n";
        return;
    }
    std::cout << "已扫描到 " << skills.size() << " 个技能:\n";
    for (const auto& skill : skills) {
        std::cout << "  - " << skill.name << " [" << skill.source_level << "]: "
                   << (skill.description.empty() ? "(没写说明)" : skill.description) << "\n";
        std::cout << "      " << skill.dir_path << "\n";
    }
}

// 粗略估算一段历史占用了多少"token"——不真调分词器,按字符数打个折扣
// (中英文混排,经验上大致两个字符算一个 token),仅供 /compact 报告
// "压缩前后省了多少"用,数字前带 ~ 提醒这是估算值,不是真实用量(真实
// 用量要靠 usage.input_tokens,那个得等实际发一次请求才知道)。
std::size_t EstimateHistoryChars(const std::vector<lubancode::api::Message>& history) {
    std::size_t total = 0;
    for (const auto& message : history) {
        for (const auto& block : message.content) {
            std::visit(
                [&total](const auto& b) {
                    using T = std::decay_t<decltype(b)>;
                    if constexpr (std::is_same_v<T, lubancode::api::TextBlock>) {
                        total += b.text.size();
                    } else if constexpr (std::is_same_v<T, lubancode::api::ToolUseBlock>) {
                        total += b.name.size() + b.input.dump().size();
                    } else if constexpr (std::is_same_v<T, lubancode::api::ToolResultBlock>) {
                        total += b.content.size();
                    }
                },
                block);
        }
    }
    return total;
}

std::size_t EstimateTokens(std::size_t chars) { return (chars + 1) / 2; }

// /context 命令:不带参数看当前占用,带参数(256k/512k/1m/裸数字)临时改
// 窗口大小,只本会话生效,不改配置文件。
void HandleContextCommand(const std::string& args, lubancode::cli::ContextTracker& context_tracker) {
    if (args.empty()) {
        std::cout << "上下文占用: "
                   << lubancode::cli::FormatTokenCount(static_cast<std::int64_t>(context_tracker.current_tokens()))
                   << " / "
                   << lubancode::cli::FormatTokenCount(static_cast<std::int64_t>(context_tracker.window_tokens()))
                   << " tokens (" << context_tracker.UsagePercent() << "%)";
        if (context_tracker.ShouldAutoCompact()) {
            std::cout << "  —— 接近上限了,建议 /compact 一下";
        }
        std::cout << "\n";
        return;
    }
    const auto parsed = lubancode::config::ParseContextWindowTokens(args);
    if (!parsed.has_value()) {
        std::cout << parsed.error() << "\n";
        return;
    }
    context_tracker.set_window_tokens(*parsed);
    std::cout << "上下文窗口已改成 " << *parsed << " tokens(只本会话生效,没改配置文件)。\n";
}

// /compact 命令:把当前历史整段发给模型换一份压缩存档,顶替掉中间那段
// 老对话,只留 archive + 最近一轮完整对话。backend 传裸的、没包
// ModelOverrideBackend 的那份——Compact() 会自己把 compact_model 写进
// request.model,要是走了 ModelOverrideBackend,会被强制换回当前会话
// model,压缩模型这个字段就形同虚设了。
// 压缩成功时返回对应的 compact 事件(archive + kept_from),调用方追加写进
// 存档流水,/resume 才能回放出压缩后的活状态;失败/没得压给 nullopt。
std::optional<lubancode::agent::CompactEvent> HandleCompactCommand(
    const std::string& args, lubancode::agent::AgentLoop& loop, lubancode::api::Backend& raw_backend,
    const std::string& compact_model, const lubancode::cli::Theme& theme, bool spinner_enabled) {
    const std::vector<lubancode::api::Message>& history = loop.History();
    if (history.empty()) {
        std::cout << "当前没有对话历史,不用压缩。\n";
        return std::nullopt;
    }
    const std::size_t before_tokens = EstimateTokens(EstimateHistoryChars(history));
    const std::size_t old_size = history.size();

    lubancode::cli::Spinner spinner(theme, spinner_enabled);
    const auto result = lubancode::agent::Compact(raw_backend, compact_model, history, args);
    spinner.Stop();

    if (!result.has_value()) {
        std::cout << theme.error << "压缩失败: " << result.error().message << theme.reset << "\n";
        return std::nullopt;
    }

    const auto new_history = lubancode::agent::BuildCompactedHistory(history, *result);
    const auto event = lubancode::agent::MakeCompactEvent(old_size, new_history);
    loop.ReplaceHistory(new_history);
    const std::size_t after_tokens = EstimateTokens(EstimateHistoryChars(new_history));
    std::cout << "压缩前 ~" << before_tokens << " tokens → 压缩后 ~" << after_tokens << " tokens\n";
    return event;
}

// /think(/effort 同义)命令:不带参数看当前档位,带参数切档位(本会话
// 生效)。M10 把档位放开成任意字符串——不在这儿拦,认不认得留给发请求
// 那一刻(responses 原样递,anthropic 查映射表、映射不上打警告)去判断,
// 原样存,不强制转小写(anthropic 那张映射表自己做大小写不敏感匹配,
// responses 要"原样递",这里转了小写反而破坏这条承诺)。
// entry:当前模型在模型目录(models.json)里的条目,没有就是 nullptr。
// 有条目且声明了 supported_think_levels → 裸敲列真实档位带描述,设了表外
// 档位只提示"目录未声明,仍会发送",不拦;没有条目 → 维持现状提示。
void HandleThinkCommand(const std::string& args, const std::shared_ptr<std::string>& current_think,
                         const lubancode::config::ModelCatalogEntry* entry = nullptr) {
    const std::vector<std::string> hint_lines = lubancode::config::ThinkLevelHintLines(entry);
    if (args.empty()) {
        std::cout << "当前推理强度: " << (current_think->empty() ? "(未设置,不发这个参数)" : *current_think) << "\n";
        if (!hint_lines.empty()) {
            std::cout << "模型目录声明的档位(" << entry->slug << "):\n";
            for (const auto& line : hint_lines) {
                std::cout << line << "\n";
            }
        } else {
            std::cout << "支持哪些档位以服务商为准。\n";
        }
        return;
    }
    *current_think = args;
    std::cout << "推理强度已切到 " << args << "(本会话生效)。";
    if (!hint_lines.empty()) {
        if (!lubancode::config::ThinkLevelDeclared(*entry, args)) {
            std::cout << "提示: 模型目录未声明该档,仍会发送。";
        }
        std::cout << "\n";
    } else {
        std::cout << "支持哪些档位以服务商为准。\n";
    }
}

// 把模型目录条目应用到会话状态:/model 切换(两个 explicit 都传 false,
// 目录声明了就用)和交互模式启动(explicit 按 Source 判断,用户显式配过的
// 不动)共用这一段。改 current_think / 会话窗口 / base_instructions,干了
// 什么就打一行;模型不在目录时 ComputeCatalogApplication 给回一份"全空"
// 的应用——think/窗口不动,base_instructions 清空(旧模型的指令不再发),
// 一切回退现状,不打任何多余的话。
void ApplyModelCatalog(const lubancode::config::ModelCatalog& catalog, const std::string& slug,
                        bool think_explicit, bool window_explicit,
                        const std::shared_ptr<std::string>& current_think,
                        lubancode::cli::ContextTracker& context_tracker,
                        const std::shared_ptr<std::string>& current_model_instructions) {
    const auto apply =
        lubancode::config::ComputeCatalogApplication(catalog, slug, think_explicit, window_explicit);
    if (apply.think.has_value()) {
        *current_think = *apply.think;
        std::cout << "think→" << *apply.think << "(目录默认)\n";
    }
    if (apply.context_window_tokens.has_value()) {
        context_tracker.set_window_tokens(*apply.context_window_tokens);
        std::cout << "上下文窗口→" << *apply.context_window_tokens << " tokens(目录声明)\n";
    }
    if (*current_model_instructions != apply.base_instructions) {
        *current_model_instructions = apply.base_instructions;
        if (!apply.base_instructions.empty()) {
            std::cout << "base_instructions 已注入系统提示(目录条目 " << slug << ",下一轮请求生效)\n";
        }
    }
}

// /model 命令的执行逻辑:带参数直接切;不带参数拉列表编号选。切完了,
// 有配置文件才问"写进配置文件?",没有就只提示本会话生效。
// catalog:模型目录——列表里优先显示目录条目的 display_name(其次接口
// 给的 display_name,最后 id 兜底);切换成功后按目录条目应用
// default_think / context_window / base_instructions(见 ApplyModelCatalog)。
void HandleModelCommand(const std::string& args, const lubancode::config::Config& config,
                         const std::shared_ptr<std::string>& current_model,
                         std::optional<std::string>& config_file_path,
                         const lubancode::config::ModelCatalog& catalog,
                         const std::shared_ptr<std::string>& current_think,
                         lubancode::cli::ContextTracker& context_tracker,
                         const std::shared_ptr<std::string>& current_model_instructions) {
    std::string chosen;

    if (!args.empty()) {
        chosen = args;
    } else {
        const auto list_result = lubancode::api::ListModels(config.wire, config.base_url, config.auth_token);
        if (!list_result.has_value()) {
            std::cout << "拉取模型列表失败: " << list_result.error().message << "\n";
            return;
        }
        if (list_result->empty()) {
            std::cout << "接口返回的模型列表是空的。\n";
            return;
        }
        for (std::size_t i = 0; i < list_result->size(); ++i) {
            const auto& m = (*list_result)[i];
            const auto* entry = catalog.FindBySlug(m.id);
            std::string label;
            if (entry != nullptr && !entry->display_name.empty()) {
                // 目录条目的 display_name 优先,后面括号带上 slug——选完切换
                // 用的还是 API 模型名,展示名和真名对得上号。
                label = entry->display_name + "(" + m.id + ")";
            } else {
                label = m.display_name.empty() ? m.id : m.display_name;
            }
            std::cout << "  " << (i + 1) << ") " << label << "\n";
        }
        const std::optional<std::string> selection = lubancode::cli::ReadLine("选择模型编号 [1]: ");
        if (!selection.has_value()) {
            return;
        }
        std::size_t idx = 0;
        if (!selection->empty()) {
            try {
                std::size_t consumed = 0;
                const int n = std::stoi(*selection, &consumed);
                if (consumed != selection->size() || n < 1 || static_cast<std::size_t>(n) > list_result->size()) {
                    std::cout << "编号不对,取消切换。\n";
                    return;
                }
                idx = static_cast<std::size_t>(n - 1);
            } catch (...) {
                std::cout << "没听懂,取消切换。\n";
                return;
            }
        }
        chosen = (*list_result)[idx].id;
    }

    *current_model = chosen;
    std::cout << "已切换到模型: " << chosen << "(本会话生效)\n";

    // 模型目录应用:主动切换,目录声明了就用(两个 explicit 都传 false);
    // 切到目录外的名字时这一步什么都不动(base_instructions 清空),回退现状。
    ApplyModelCatalog(catalog, chosen, /*think_explicit=*/false, /*window_explicit=*/false, current_think,
                       context_tracker, current_model_instructions);

    if (config_file_path.has_value()) {
        const std::optional<std::string> answer =
            lubancode::cli::ReadLine("写进配置文件 " + *config_file_path + "? [y/N]: ");
        if (answer.has_value() && (*answer == "y" || *answer == "Y")) {
            const auto updated = lubancode::config::UpdateModelInConfigFile(*config_file_path, chosen);
            if (updated.has_value()) {
                std::cout << "已更新 " << *config_file_path << "\n";
            } else {
                std::cout << "更新失败: " << updated.error() << "\n";
            }
        }
    } else {
        std::cout << "当前没有生效的配置文件,只在本会话生效。\n";
    }
}

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
// -> SOUL.md;别的名字 -> souls/<名字>.md。文件缺了返回空串(= 无效果),
// warn 为真时打一行说明。启动读一次、/soul 切换即时重读,都走这一个函数。
std::string LoadSoulContentByName(const std::string& name, bool warn) {
    if (name == "off") {
        return std::string();
    }
    const auto luban_dir = lubancode::config::HomeLubancodeDir();
    if (!luban_dir.has_value()) {
        return std::string();
    }
    const std::string path = (name.empty() || name == "default")
                                  ? lubancode::config::SoulFilePath(*luban_dir)
                                  : lubancode::config::SoulPathByName(*luban_dir, name);
    const auto content = lubancode::config::ReadTextFileIfExists(path);
    if (!content.has_value()) {
        if (warn) {
            std::cout << "[soul] 找不到 " << path << ",魂不生效。\n";
        }
        return std::string();
    }
    return *content;
}

// /soul 命令:裸敲列出 souls/*.md + default(SOUL.md)+ 当前生效的;
// /soul 名字 切换(会话级即时生效,下一轮请求换新系统提示,然后问要不要
// 写进配置);/soul off 本会话关魂;/soul default 回 SOUL.md(这两个只管
// 本会话,不问写配置)。切换即时重读所选文件——用户改了魂文件,切一下
// 就生效,不用重启。
void HandleSoulCommand(const std::string& args, const std::shared_ptr<std::string>& current_soul,
                        std::string& current_soul_name, const std::optional<std::string>& config_file_path) {
    const auto luban_dir = lubancode::config::HomeLubancodeDir();
    if (!luban_dir.has_value()) {
        std::cout << "找不到用户主目录,魂文件没处安身,/soul 用不了。\n";
        return;
    }

    if (args.empty()) {
        const std::vector<std::string> souls = lubancode::config::ListSouls(*luban_dir);
        std::cout << "可用的魂(风格叠加层,注入在系统提示最后):\n";
        std::cout << "  - default(主目录 SOUL.md)\n";
        for (const auto& name : souls) {
            std::cout << "  - " << name << "\n";
        }
        std::cout << "当前生效: " << current_soul_name;
        if (lubancode::agent::StripPromptComments(*current_soul).empty() && current_soul_name != "off") {
            std::cout << "(内容空白,无效果)";
        }
        std::cout << "\n用法:/soul 名字 切换;/soul off 本会话关魂;/soul default 回 SOUL.md。\n";
        return;
    }

    if (args == "off") {
        current_soul->clear();
        current_soul_name = "off";
        std::cout << "魂已关(本会话生效,下一轮请求换新系统提示)。\n";
        return;
    }

    if (args == "default") {
        *current_soul = LoadSoulContentByName("default", /*warn=*/true);
        current_soul_name = "default";
        std::cout << "已切回 SOUL.md";
        if (lubancode::agent::StripPromptComments(*current_soul).empty()) {
            std::cout << "(内容空白,无效果)";
        }
        std::cout << "。\n";
        return;
    }

    const std::string path = lubancode::config::SoulPathByName(*luban_dir, args);
    const auto content = lubancode::config::ReadTextFileIfExists(path);
    if (!content.has_value()) {
        std::cout << "找不到魂 " << args << "(" << path << ")。/soul 裸敲能看可用列表。\n";
        return;
    }
    *current_soul = *content;
    current_soul_name = args;
    std::cout << "已切换魂: " << args << "(本会话即时生效,下一轮请求换新系统提示)\n";

    if (config_file_path.has_value()) {
        const std::optional<std::string> answer = lubancode::cli::ReadLine("写进配置? [y/N]: ");
        if (answer.has_value() && (*answer == "y" || *answer == "Y")) {
            const auto updated = lubancode::config::UpdateSoulInConfigFile(*config_file_path, args);
            if (updated.has_value()) {
                std::cout << "已更新 " << *config_file_path << "\n";
            } else {
                std::cout << "更新失败: " << updated.error() << "\n";
            }
        }
    } else {
        std::cout << "当前没有生效的配置文件,只在本会话生效。\n";
    }
}

// /prompt 命令:裸敲显示当前法(人格段)的来源和字数;/prompt reset 带
// 二次确认,把 system_prompt.md 还原成内置默认(旧文件挪成 .bak)。
// persona 是本会话实际在用的人格段(空串 = 内置默认);law_source 是启动时
// 算好的来源说明(CLI 参数/文件/内置)。
void HandlePromptCommand(const std::string& args, const std::string& law_source, const std::string& persona) {
    if (args.empty()) {
        const std::string& effective = persona.empty() ? lubancode::agent::DefaultPersona() : persona;
        std::cout << "当前的法(系统提示词人格段):\n"
                   << "  来源: " << law_source << "\n"
                   << "  字数: " << CountUtf8Chars(effective) << "\n"
                   << "用法:/prompt reset 把 system_prompt.md 还原成内置默认(旧文件留 .bak)。\n";
        return;
    }
    if (args != "reset") {
        std::cout << "用法:/prompt 看当前法的来源;/prompt reset 还原 system_prompt.md。\n";
        return;
    }

    const std::optional<std::string> answer = lubancode::cli::ReadLine("确定还原? [y/N]: ");
    if (!answer.has_value() || (*answer != "y" && *answer != "Y")) {
        std::cout << "取消还原。\n";
        return;
    }
    const auto luban_dir = lubancode::config::HomeLubancodeDir();
    if (!luban_dir.has_value()) {
        std::cout << "找不到用户主目录,没法还原。\n";
        return;
    }
    const auto reset_result =
        lubancode::config::ResetSystemPromptFile(*luban_dir, lubancode::agent::DefaultPersona());
    if (!reset_result.has_value()) {
        std::cout << "还原失败: " << reset_result.error() << "\n";
        return;
    }
    std::cout << "已把 " << lubancode::config::SystemPromptFilePath(*luban_dir) << " 还原成内置默认";
    if (!reset_result->empty()) {
        std::cout << ",旧文件在 " << *reset_result;
    }
    std::cout << "。\n本会话的法不变,下次启动按新文件生效。\n";
}

// ---------------------------------------------------------------------------
// 会话存档与续聊(0.13.x):/sessions //resume //export 和 --continue 的
// 执行逻辑。序列化/成对修补/导出全在 agent/session_store 里(纯函数),
// 这里只做"接命令、找文件、打印"这层壳。
// ---------------------------------------------------------------------------

// /sessions:列最近 20 场(id、开始时间、标题或首句摘要、消息数),时间
// 倒序编号。默认只列 meta.cwd 是当前目录的场子;/sessions all 列全部目录,
// 每条带目录路径(过长中间省略)。
void PrintSessionsCommand(const std::string& sessions_dir, const std::string& args) {
    if (sessions_dir.empty()) {
        std::cout << "找不到用户主目录,会话存档不可用。\n";
        return;
    }
    const bool all = args == "all";
    if (!args.empty() && !all) {
        std::cout << "用法:/sessions(本目录)或 /sessions all(全部目录)\n";
        return;
    }
    const auto entries =
        lubancode::agent::ListSessions(sessions_dir, 20, all ? std::string() : CurrentDirUtf8());
    if (entries.empty()) {
        if (all) {
            std::cout << "还没有会话存档(" << sessions_dir << " 下没有 .jsonl)。\n";
        } else {
            std::cout << "本目录还没有会话存档(/sessions all 看全部目录)。\n";
        }
        return;
    }
    std::cout << "最近 " << entries.size() << " 场会话("
              << (all ? "全部目录" : "本目录,/sessions all 看全部")
              << ";时间倒序,/resume 编号或id 续聊):\n";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        // 标题优先,没设过标题回退首句摘要。
        const std::string& label = !entry.title.empty() ? entry.title : entry.first_user_text;
        std::cout << "  " << (i + 1) << ") " << entry.id << "\n"
                   << "      " << (entry.started_at.empty() ? "(开始时间未知)" : entry.started_at) << " · "
                   << entry.message_count << " 条 · "
                   << (label.empty() ? "(没有用户文本)" : lubancode::agent::TruncateUtf8Chars(label, 40))
                   << "\n";
        if (all) {
            std::cout << "      目录: "
                       << (entry.cwd.empty() ? "(未知)"
                                              : lubancode::agent::AbbreviateUtf8Middle(entry.cwd, 48))
                       << "\n";
        }
    }
}

// /resume <编号或id> 和 --continue 共用的执行逻辑。target 是编号(按
// ListSessions 的倒序编号)、会话 id、或空串(--continue:最近一场)。
// 编号和"最近一场"都只在**本目录**(meta.cwd == 当前 cwd)的场子里数;
// 直接给 id 的仍然全局能找(拼路径兜底),跨目录恢复留了这条明路。
// 成功:回放事件 + 成对修补 + ReplaceHistory + 接管文件继续追加,返回 true;
// session_title 同步成存档里最后一条 title 事件(没有就清空)。
// quiet_if_none:--continue 本目录找不到任何存档时不报错、安静开新会话。
bool ResumeSession(const std::string& target, const std::string& sessions_dir,
                    lubancode::agent::AgentLoop& loop, lubancode::agent::SessionStore& store,
                    std::size_t& persisted_count, lubancode::agent::SessionMeta& session_meta,
                    std::string& session_title, const std::string& wire_str, const std::string& current_model,
                    const lubancode::cli::Theme& theme, bool quiet_if_none) {
    if (sessions_dir.empty()) {
        std::cout << "找不到用户主目录,会话存档不可用。\n";
        return false;
    }
    const auto entries = lubancode::agent::ListSessions(sessions_dir, 20, CurrentDirUtf8());

    std::string id;
    std::string file_path;
    bool all_digits = !target.empty();
    for (const char c : target) {
        if (c < '0' || c > '9') {
            all_digits = false;
            break;
        }
    }
    if (target.empty()) {
        // --continue:本目录最近一场;一场都没有就按 quiet_if_none 处理。
        if (entries.empty()) {
            if (!quiet_if_none) {
                std::cout << "本目录还没有会话存档,没什么可恢复(/sessions all 看全部目录)。\n";
            }
            return false;
        }
        id = entries.front().id;
        file_path = entries.front().file_path;
    } else if (all_digits) {
        std::size_t n = 0;
        try {
            n = static_cast<std::size_t>(std::stoul(target));
        } catch (...) {
            n = 0;
        }
        if (n < 1 || n > entries.size()) {
            std::cout << "编号 " << target << " 超出范围(本目录现有 " << entries.size()
                       << " 场,/sessions 看列表)。\n";
            return false;
        }
        id = entries[n - 1].id;
        file_path = entries[n - 1].file_path;
    } else {
        // 按 id 找:先在列表里对,不在(比 20 场更老)就直接拼路径试。
        for (const auto& entry : entries) {
            if (entry.id == target) {
                id = entry.id;
                file_path = entry.file_path;
                break;
            }
        }
        if (id.empty()) {
            id = target;
            file_path = sessions_dir + "/" + target + ".jsonl";
        }
    }

    const auto content = lubancode::agent::ReadSessionFileBytes(file_path);
    if (!content.has_value()) {
        std::cout << "读不到存档 " << file_path << "。\n";
        return false;
    }
    auto session = lubancode::agent::ParseSessionFile(*content);
    if (!session.has_value()) {
        std::cout << "存档 " << file_path << " 首行不是合法 meta,认不得这个格式。\n";
        return false;
    }

    loop.ReplaceHistory(session->messages);
    persisted_count = session->messages.size();
    if (!store.ResumeAt(file_path, id)) {
        std::cout << theme.error << "[会话存档] 接管 " << file_path << " 失败,恢复的历史只在内存里,本场不再落盘。"
                   << theme.reset << "\n";
    }
    session_meta = session->meta;
    session_title = session->title;

    if (session->compact_count > 0) {
        // 经过压缩的场子:恢复的是回放出来的有效态,不是全量流水。
        std::cout << "已恢复 " << id << ",有效 " << session->messages.size() << " 条(全量 "
                   << session->all_messages.size() << " 条,经 " << session->compact_count << " 次压缩)";
    } else {
        std::cout << "已恢复 " << id << "," << session->messages.size() << " 条消息";
    }
    if (session->repaired > 0) {
        std::cout << "(补了 " << session->repaired << " 条缺失的工具结果)";
    }
    if (session->skipped_lines > 0) {
        std::cout << "(跳过 " << session->skipped_lines << " 行解析不动的存档)";
    }
    std::cout << "。\n";
    // context 记账:真实 usage 得等恢复后第一次请求才校准,这里先按字符
    // 粗估打一行,心里有数。
    std::cout << "上下文占用(按字符粗估): ~"
               << EstimateTokens(EstimateHistoryChars(session->messages)) << " tokens,首轮请求后以真实用量为准。\n";
    if (!session->meta.model.empty() && session->meta.model != current_model) {
        std::cout << theme.stats << "[提醒] 存档时用的 model 是 " << session->meta.model << ",当前是 "
                   << current_model << ",继续聊没问题,风格可能有差。" << theme.reset << "\n";
    }
    if (!session->meta.wire.empty() && session->meta.wire != wire_str) {
        std::cout << theme.stats << "[提醒] 存档时用的 wire 是 " << session->meta.wire << ",当前是 " << wire_str
                   << "。" << theme.reset << "\n";
    }
    return true;
}

// /export [路径]:当前会话导出 Markdown,默认写 sessions/<id>.md。
// 有存档文件就从文件读**全量流水**导出(压缩不丢内容,发生点插一行标注);
// 没有存档文件(没落过盘)退回导内存里这份历史。/title 设过的标题当大标题。
void HandleExportCommand(const std::string& args, const lubancode::agent::AgentLoop& loop,
                          const lubancode::agent::SessionStore& store, const std::string& sessions_dir,
                          const lubancode::agent::SessionMeta& session_meta, const std::string& session_title) {
    const auto& history = loop.History();
    if (history.empty()) {
        std::cout << "当前会话还没有内容,没什么可导出。\n";
        return;
    }
    const std::string id =
        !store.session_id().empty() ? store.session_id() : lubancode::agent::NowIdTimestamp() + "-export";
    std::string out_path = args;
    if (out_path.empty()) {
        if (sessions_dir.empty()) {
            std::cout << "找不到用户主目录,请显式给个路径:/export 路径\n";
            return;
        }
        out_path = sessions_dir + "/" + id + ".md";
    }

    // 全量优先:存档文件在,就按文件里的流水导(含压缩标注);读不动再退
    // 回内存这份(此时没有压缩位置可标)。
    std::string markdown;
    bool exported_from_file = false;
    if (!store.file_path().empty()) {
        if (const auto content = lubancode::agent::ReadSessionFileBytes(store.file_path());
            content.has_value()) {
            if (const auto session = lubancode::agent::ParseSessionFile(*content); session.has_value()) {
                const std::string& title = !session->title.empty() ? session->title : session_title;
                markdown = lubancode::agent::ExportSessionMarkdown(session->meta, session->all_messages, id,
                                                                     /*max_result_lines=*/30, title,
                                                                     session->compact_positions);
                exported_from_file = true;
            }
        }
    }
    if (!exported_from_file) {
        markdown = lubancode::agent::ExportSessionMarkdown(session_meta, history, id,
                                                             /*max_result_lines=*/30, session_title);
    }

    const std::filesystem::path path(
        std::u8string(reinterpret_cast<const char8_t*>(out_path.data()), out_path.size()));
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        std::cout << "写不进 " << out_path << "。\n";
        return;
    }
    file << markdown;
    file.close();
    std::cout << "已导出 Markdown: " << out_path << "\n";
}

// 没带参数时的交互循环:读一行、问一句,exit/quit 或 EOF 退出。
// 空行不退出——只是重新给一次提示符,继续等下一行(老规则"空行退出"跟
// Windows 控制台偶发读空串的老毛病撞在一块,会把读空串误当成用户要退出,
// 改成只认 exit/quit/EOF 才靠谱)。
// AgentLoop 用 std::optional 包着,存一份在循环外面,历史跨轮保留;
// /clear 需要清空历史,而 AgentLoop 的历史是私有成员、没有 Clear()(agent
// 层现有文件不让动),唯一的办法是就地重新构造一份全新的 AgentLoop——
// optional::emplace 走的是构造而不是赋值,AgentLoop 引用成员导致的
// "不可赋值"不影响这条路。
// always_allowed_tools 同样在循环外面建一次,"本会话总是允许"才能真的跨
// 多轮用户输入生效,/clear 不清空它(清没清对话历史跟工具授权是两码事)。
// config_result.config.model 是四级合并出来的初始 model,current_model 是
// 真正"这一刻发请求用哪个 model"的会话级状态,两者可能因为 /model 切换而
// 不一致——ModelOverrideBackend 在真正发请求前把 Request.model 换成
// *current_model,这样 /model 切换才能不碰 agent/loop.hpp/.cpp 就真正生效。
// continue_last:--continue 启动参数,进循环前先自动 /resume 最近一场;
// 一场存档都没有就正常开新会话,不报错。
// law_source:魂法分家(0.16.x)新增,启动时算好的"法从哪儿来"说明
// (CLI 参数/文件/内置),/prompt 裸敲展示用,不参与任何逻辑。
void InteractiveLoop(lubancode::config::ConfigResult config_result, bool auto_confirm,
                      const lubancode::cli::Theme& theme, const std::string& persona, bool spinner_enabled,
                      const lubancode::config::ModelCatalog& model_catalog, bool continue_last = false,
                      const std::string& law_source = "内置默认") {
    const lubancode::config::Config& config = config_result.config;

    // M9:技能扫描一次,主代理、子代理、系统提示词、/skills 命令共用同一份
    // 结果——扫描本身只在启动时做一次,不在每轮对话里重复读磁盘。
    const std::optional<std::string> home_dir = lubancode::config::HomeDir();
    const std::vector<lubancode::tools::SkillMeta> skills = lubancode::tools::LoadSkills(CurrentDirUtf8(), home_dir);
    const std::string skills_segment = lubancode::tools::BuildSkillsPromptSegment(skills);

    std::unique_ptr<lubancode::api::Backend> real_backend = BuildBackend(config);
    auto current_model = std::make_shared<std::string>(config.model);
    auto current_think = std::make_shared<std::string>(config.think);
    // 模型目录 base_instructions 的会话级状态:启动/切换模型时由
    // ApplyModelCatalog 填,ModelInstructionsBackend 发请求前现拼进
    // Request.system;空串 = 不追加,零破坏。
    auto current_model_instructions = std::make_shared<std::string>();
    // 魂(0.16.x):启动按配置的 soul 名读一次(空 = SOUL.md),/soul 切换
    // 即时重读、改的就是这块内存。SoulOverlayBackend 放在 instructions 层
    // 更内侧,魂在系统提示里永远压轴(见类注释)。
    std::string current_soul_name = config.soul.empty() ? "default" : config.soul;
    auto current_soul = std::make_shared<std::string>(LoadSoulContentByName(current_soul_name, /*warn=*/true));
    ModelOverrideBackend model_backend(*real_backend, current_model);
    ThinkOverrideBackend think_backend(model_backend, current_think);
    SoulOverlayBackend soul_backend(think_backend, current_soul);
    ModelInstructionsBackend instructions_backend(soul_backend, current_model_instructions);
    // SpinnerBackend 包最外层:每次 send_stream 起转轮,收到第一个流事件就
    // 停,Model/Think/ModelInstructions 各层分别负责 /model、/think、目录
    // base_instructions 切换生效,几层包装顺序不影响语义(补丁都在更内层
    // 做,转轮只关心"发出去了没有第一个字节回来")。/compact 触发的压缩
    // 请求不走这几层包装,直接用 *real_backend——理由见
    // HandleCompactCommand 注释。
    SpinnerBackend wrapped_backend(instructions_backend, theme, spinner_enabled);

    // ContextTracker:会话级"上下文占用"记账,/context、自动 compact 都靠它。
    lubancode::cli::ContextTracker context_tracker(config.context_window_tokens);

    PrintBanner(config, theme);

    // 模型目录:启动时当前模型就在目录里,同样应用 default_think /
    // context_window / base_instructions——但用户显式配过的字段(Source
    // 不是内置默认值)不动,目录只是"该模型的出厂默认",压不过用户自己
    // 的配置。打印紧跟横幅,干了什么一眼看全。
    ApplyModelCatalog(model_catalog, *current_model,
                       /*think_explicit=*/config_result.sources.think != lubancode::config::Source::Default,
                       /*window_explicit=*/config_result.sources.context_window_tokens !=
                           lubancode::config::Source::Default,
                       current_think, context_tracker, current_model_instructions);

    // M8:mcp_servers 声明在 sub_registry/registry 之前——函数退出时按声明的
    // 反序析构,sub_registry/registry(里头的 McpTool 持有 mcp::Client& 引用)
    // 会先于 mcp_servers(真正拥有 Client/子进程)析构,不会有悬垂引用。
    // 起服务器放在 PrintBanner 之后、第一次给提示符之前,"[mcp] xxx: N 个
    // 工具已挂载" 这行紧跟着横幅打出来。
    std::vector<McpServerRuntime> mcp_servers = StartMcpServers(config.mcp_servers, theme);

    // M7:插件宿主声明在 sub_registry/registry 之前(析构反序,PluginTool
    // 引用的 DLL 静态数据才不会先没),真正扫描挂载在 registry 建好之后。
    lubancode::tools::PluginHost plugin_host;
    std::vector<PluginMountInfo> plugin_mounted;
    std::vector<std::string> plugin_warnings;
    // LSP:lsp_manager 声明在两份 registry 之前(析构反序,LspTool 持有的
    // Manager& 不会悬垂,理由同上面的 mcp_servers)。配置了 lsp 段才构造,
    // 构造本身不起任何进程(懒启动,首次用到某语言才拉),析构时把还活着
    // 的服务器按 shutdown/exit + 2s 兜底的规矩全关掉。
    std::optional<lubancode::lsp::Manager> lsp_manager;
    if (!config.lsp_servers.empty()) {
        lsp_manager.emplace(config.lsp_servers, CurrentDirUtf8());
    }

    // 两份工具表:sub_registry 只有基础工具,喂给 agent 工具当"子代理能用
    // 什么"(不含 agent 自己,防递归);registry 是主循环真正用的那份,
    // 基础工具之外多注册了 agent 工具本身。两者都是这个函数的局部变量,
    // sub_registry 声明在前、registry 在后,函数退出时按声明的反序析构,
    // agent 工具持有的 sub_registry 引用不会悬垂。MCP 工具同样注册进两份
    // (子代理也能用外部工具)。
    lubancode::tools::ToolRegistry sub_registry = BuildBaseToolRegistry(skills, config.search);
    lubancode::tools::ToolRegistry registry = BuildBaseToolRegistry(skills, config.search);
    RegisterMcpTools(mcp_servers, sub_registry);
    RegisterMcpTools(mcp_servers, registry);
    // lsp 工具:主表 + 子代理表都挂(语义查询对子代理同样有用),两份
    // LspTool 实例共享同一个 Manager(背后同一批服务器进程,不会因为注册
    // 两份就多起进程)。没配 lsp 段就完全不注册——不配置 = 不启用。
    if (lsp_manager.has_value()) {
        sub_registry.Register(std::make_unique<lubancode::tools::LspTool>(*lsp_manager));
        registry.Register(std::make_unique<lubancode::tools::LspTool>(*lsp_manager));
    }
    registry.Register(std::make_unique<lubancode::tools::AgentTool>(
        wrapped_backend, sub_registry, CurrentDirUtf8(), config.model, /*default_max_turns=*/15, skills_segment));

    // M11(0.10.0):todo_write 只挂主注册表(registry),绝不挂 sub_registry——
    // 子代理是短命的一次性跑腿,不该有权限乱写主会话的待办清单。todo_state
    // 这份 shared_ptr 同时交给 /todos 命令、RunTurn(转给 on_tool_done 渲染)。
    auto todo_state = std::make_shared<lubancode::tools::TodoListState>();
    registry.Register(std::make_unique<lubancode::tools::TodoWriteTool>(todo_state));

    // M7:插件工具只挂主 registry(不挂 sub_registry,短命跑腿不用外挂),
    // 挂载行紧跟 [mcp] 那几行打出来。
    MountPlugins(plugin_host, registry, theme, plugin_mounted, plugin_warnings);

    // -----------------------------------------------------------------------
    // tool_search(延迟挂载):全部工具(MCP/插件/LSP/agent/todo)都注册完了
    // 才数总数、定启停——阈值判定看的是"这一场会话工具表实际多大"。
    // loaded 集合是会话级的(/clear 不清:工具挂载与对话历史无关),主会话
    // 与子代理共享同一份(挂载一次两边可用)。主表/子表各自按各自的总数
    // 判定,同一个阈值;启用的那张表才注册 tool_search 自身(注册前先数,
    // tool_search 不算在阈值账里)。没启用(默认阈值 20,不挂一堆外挂
    // 工具到不了)时:不注册 tool_search、不设过滤谓词、索引段恒空——
    // 跟 0.16.0 现状完全一致。
    // -----------------------------------------------------------------------
    auto loaded_tools = std::make_shared<std::set<std::string>>();
    const int tool_search_threshold = config.tool_search_threshold;
    const bool main_deferral = lubancode::tools::DeferralEnabled(registry.All().size(), tool_search_threshold);
    const bool sub_deferral = lubancode::tools::DeferralEnabled(sub_registry.All().size(), tool_search_threshold);
    if (main_deferral) {
        registry.Register(std::make_unique<lubancode::tools::ToolSearchTool>(registry, loaded_tools));
    }
    if (sub_deferral) {
        sub_registry.Register(std::make_unique<lubancode::tools::ToolSearchTool>(sub_registry, loaded_tools));
    }
    std::function<bool(const lubancode::tools::Tool&)> tool_filter;
    if (main_deferral || sub_deferral) {
        // 谓词本身不区分主表/子表——放行"非延迟"或"已挂载",两边通用。
        tool_filter = [loaded_tools](const lubancode::tools::Tool& tool) {
            return !tool.deferred() || loaded_tools->count(tool.name()) != 0;
        };
    }
    if (auto* agent_tool = dynamic_cast<lubancode::tools::AgentTool*>(registry.Find("agent"));
        agent_tool != nullptr && sub_deferral) {
        agent_tool->SetToolFilter(tool_filter);
        agent_tool->SetDeferredIndexProvider([&sub_registry, loaded_tools]() {
            return lubancode::tools::BuildDeferredToolsIndexSegment(sub_registry, *loaded_tools);
        });
    }
    if (main_deferral) {
        std::cout << theme.stats << "[tool_search] 工具超过阈值 " << tool_search_threshold
                   << ",MCP/插件等外挂工具改为延迟挂载(/tools 看三态)" << theme.reset << "\n";
    }
    // 主 AgentLoop 的索引段:发请求前现算现拼(见 DeferredIndexBackend 注释)。
    // 未启用时 provider 恒给空串,这层包装纯透传。
    DeferredIndexBackend index_backend(
        wrapped_backend, [&registry, loaded_tools, main_deferral]() {
            return main_deferral ? lubancode::tools::BuildDeferredToolsIndexSegment(registry, *loaded_tools)
                                  : std::string();
        });

    // UI-B(0.12.0):会话级工具条目存档,跨多轮 RunTurn 累积。full_output
    // 现在就存好(截 64KB),UI-D 的 Ctrl+E 全文查看直接从这儿取。
    std::vector<lubancode::cli::TranscriptItem> transcript;

    // -----------------------------------------------------------------------
    // UI-D(0.16.0):Ctrl+O 紧凑/详细 + 焦点导航 + Ctrl+E 聚焦查看。
    // 三样会话级状态都在这儿;按键语义翻译在 LineEditorCore(composer 空不空、
    // 键是什么),转发管道在 console_input 的 SetTranscriptUiHandler,真正
    // 打印重画全在下面这个回调里。只在等输入时会被调(流式期间监听线程
    // 天然吞不进这些键);管道模式走不到逐键路径,整套无感。
    // -----------------------------------------------------------------------
    bool transcript_expanded = false;  // Ctrl+O 全局开关,RunTurn 里新条目也按它画
    int focus_index = -1;              // 焦点条目的 transcript 下标,-1 = 无焦点
    bool focus_view_active = false;    // 正在聚焦查看

    // 聚焦查看返回时的"简化重画":最近几条紧凑摘要(焦点标记照带)。
    const auto print_recent_items = [&transcript, &theme, &focus_index](std::size_t count) {
        const int width = lubancode::cli::DetectConsoleWidth().value_or(80);
        const std::size_t from = transcript.size() > count ? transcript.size() - count : 0;
        for (std::size_t i = from; i < transcript.size(); ++i) {
            std::cout << lubancode::cli::FormatTranscriptItem(transcript[i], theme, width, /*expanded=*/false,
                                                               static_cast<int>(i) == focus_index);
        }
    };

    lubancode::cli::SetTranscriptUiHandler([&](lubancode::cli::UiKeyAction action) -> bool {
        namespace cli = lubancode::cli;
        const int width = cli::DetectConsoleWidth().value_or(80);
        const int count = static_cast<int>(transcript.size());
        switch (action) {
            case cli::UiKeyAction::ToggleExpand: {
                // Ctrl+O:全局切换。简化方案:从当前光标处把全部条目按新档
                // 重打一遍,旧画面留在滚动历史里——跨轮条目的逐条锚点早就
                // 失效(TranscriptPainter 一轮一个),原地整体重排要重建
                // 全部行号记账,代价配不上收益,取舍见报告。聚焦查看态顺带
                // 退出,免得两种"整块铺屏"叠一块。
                transcript_expanded = !transcript_expanded;
                focus_view_active = false;
                std::cout << "\n" << theme.stats
                          << (transcript_expanded ? "—— 详细模式(Ctrl+O 切回紧凑)——" : "—— 紧凑模式 ——")
                          << theme.reset << "\n";
                if (count == 0) {
                    std::cout << "(本会话还没有工具条目)\n";
                    return true;
                }
                for (std::size_t i = 0; i < transcript.size(); ++i) {
                    std::cout << cli::FormatTranscriptItem(transcript[i], theme, width, transcript_expanded,
                                                            static_cast<int>(i) == focus_index);
                }
                return true;
            }
            case cli::UiKeyAction::FocusOlder:
            case cli::UiKeyAction::FocusNewer: {
                if (count == 0) {
                    return false;  // 没条目,键还回去(本来也无事发生)
                }
                if (focus_index < 0) {
                    focus_index = count - 1;  // 起手落在最近一条
                } else if (action == cli::UiKeyAction::FocusOlder) {
                    if (focus_index > 0) {
                        --focus_index;  // 到最老一条停住
                    }
                } else if (focus_index + 1 < count) {
                    ++focus_index;  // 到最新一条停住
                }
                std::cout << "\n" << theme.stats << "[焦点 " << (focus_index + 1) << "/" << count
                          << "] Tab 往旧 · Shift+Tab 往新 · Ctrl+E 查看全文" << theme.reset << "\n";
                std::cout << cli::FormatTranscriptItem(transcript[static_cast<std::size_t>(focus_index)], theme,
                                                        width, /*expanded=*/false, /*focused=*/true);
                return true;
            }
            case cli::UiKeyAction::FocusView: {
                if (focus_view_active) {
                    // 再按 Ctrl+E:返回。简化重画:横幅 + 最近几条摘要,
                    // 聚焦画面留在滚动历史里。
                    focus_view_active = false;
                    std::cout << "\n" << theme.stats << "—— 返回会话 ——" << theme.reset << "\n";
                    PrintBanner(config, theme);
                    print_recent_items(5);
                    return true;
                }
                if (count == 0) {
                    return false;
                }
                const int idx = focus_index >= 0 ? focus_index : count - 1;
                focus_view_active = true;
                std::cout << "\n" << theme.banner << "—— 聚焦查看 条目 " << (idx + 1) << "/" << count
                          << ",Ctrl+E 或 ESC 返回 ——" << theme.reset << "\n";
                // width=0:标题 + 完整参数 + full_output 全文如实铺,不截宽,
                // 超长靠终端自然折行/滚动(不真清屏——conhost 的滚回缓冲跟
                // 屏幕缓冲是同一块,真清会把历史一并抹掉,取舍见报告)。
                std::cout << cli::FormatTranscriptItem(transcript[static_cast<std::size_t>(idx)], theme,
                                                        /*width=*/0, /*expanded=*/true);
                return true;
            }
            case cli::UiKeyAction::Escape: {
                if (!focus_view_active) {
                    return false;  // 不在聚焦查看态:ESC 还给编辑器,维持"清空输入"老语义
                }
                focus_view_active = false;
                std::cout << "\n" << theme.stats << "—— 返回会话 ——" << theme.reset << "\n";
                PrintBanner(config, theme);
                print_recent_items(5);
                return true;
            }
        }
        return false;
    });
    // 回调抓着一堆局部引用,InteractiveLoop 返回前必须清掉(异常路径也算,
    // 所以用 RAII 不用手动调)。
    struct UiHandlerGuard {
        ~UiHandlerGuard() { lubancode::cli::SetTranscriptUiHandler(nullptr); }
    } ui_handler_guard;

    std::optional<lubancode::agent::AgentLoop> loop;
    const auto rebuild_loop = [&]() {
        // max_tokens=4096、max_turns=25 是 AgentLoop 自己的默认值,这里显式
        // 传出来是为了能把 config.max_context_chars 一起传进去。
        // tool_search:backend 换成 index_backend(索引段包装,未启用时纯
        // 透传);/clear 重建后过滤谓词要重新灌一遍——loaded 集合不清,
        // 已挂载的工具跨 /clear 仍然可用。
        loop.emplace(index_backend, registry, config.model,
                     lubancode::agent::BuildSystemPrompt(CurrentDirUtf8(), persona, skills_segment),
                     /*max_tokens=*/4096, /*max_turns=*/25, config.max_context_chars);
        if (main_deferral) {
            loop->SetToolFilter(tool_filter);
        }
    };
    rebuild_loop();

    std::set<std::string> always_allowed_tools;
    std::optional<std::string> config_file_path = config_result.config_file_path;

    // -----------------------------------------------------------------------
    // 会话存档(0.13.x):每轮结束把 history 里新增的消息逐条追加写
    // <主目录>/.lubancode/sessions/<会话id>.jsonl。文件在首条用户消息落地时
    // 才建(会话 id 的 slug 要用它),此前只记一个启动时间戳。找不到主目录
    // (sessions_dir 空)或建档失败,打一行警告后本场闭嘴,不拦着人聊。
    // 单发模式(AskOnce)不走这里,天然不落盘。
    // -----------------------------------------------------------------------
    const std::string wire_str = config.wire == lubancode::config::Wire::Responses ? "responses" : "anthropic";
    const auto home_lubancode = lubancode::config::HomeLubancodeDir();
    const std::string sessions_dir =
        home_lubancode.has_value() ? (*home_lubancode + "/sessions") : std::string();
    lubancode::agent::SessionStore session_store(sessions_dir);
    lubancode::agent::SessionMeta session_meta;  // /export 用;Begin/resume 时填
    std::string session_start_ts = lubancode::agent::NowIdTimestamp();
    std::size_t persisted_count = 0;   // history 里前多少条已经落过盘
    bool session_store_broken = false;  // 建档失败过,别每轮都再撞一次
    std::string session_title;          // /title 设的标题;resume 时取存档里最后一条
    bool session_title_pending = false;  // 建档前设了标题,建档成功后补写事件行

    // 把 history 里 persisted_count 之后的消息逐条追加落盘(append+flush,
    // 崩溃安全)。history 被 ReplaceHistory 换短(/compact)的场合由调用处
    // 先把 persisted_count 收到新长度,这里只管"只增不减"的常态。
    const auto persist_new_messages = [&]() {
        if (sessions_dir.empty() || session_store_broken) {
            return;
        }
        const auto& history = loop->History();
        if (history.size() <= persisted_count) {
            return;
        }
        if (!session_store.active()) {
            // 首条用户消息的第一段文本做 slug。
            std::string first_text;
            for (const auto& message : history) {
                if (message.role != lubancode::api::Role::User) {
                    continue;
                }
                for (const auto& block : message.content) {
                    if (const auto* tb = std::get_if<lubancode::api::TextBlock>(&block)) {
                        first_text = tb->text;
                        break;
                    }
                }
                break;
            }
            session_meta = lubancode::agent::SessionMeta{};
            session_meta.wire = wire_str;
            session_meta.model = *current_model;
            session_meta.cwd = CurrentDirUtf8();
            session_meta.started_at = lubancode::agent::NowTimestamp();
            if (!session_store.Begin(session_meta,
                                      lubancode::agent::MakeSessionId(session_start_ts, first_text))) {
                session_store_broken = true;
                std::cout << theme.error << "[会话存档] 在 " << sessions_dir
                           << " 建档失败,本场对话不落盘(不影响继续聊)。" << theme.reset << "\n";
                return;
            }
            // 建档前 /title 设过标题:现在有文件了,把事件行补上。
            if (session_title_pending && !session_title.empty()) {
                session_store.AppendTitleEvent(session_title);
            }
            session_title_pending = false;
        }
        for (std::size_t i = persisted_count; i < history.size(); ++i) {
            if (!session_store.AppendMessage(history[i])) {
                session_store_broken = true;
                std::cout << theme.error << "[会话存档] 追加写入失败,后续不再落盘(不影响继续聊)。"
                           << theme.reset << "\n";
                return;
            }
        }
        persisted_count = history.size();
    };

    // --continue:等价开场自动 /resume 本目录最近一场;本目录没有存档就
    // 安静开新会话。
    if (continue_last) {
        ResumeSession("", sessions_dir, *loop, session_store, persisted_count, session_meta, session_title,
                       wire_str, *current_model, theme, /*quiet_if_none=*/true);
    }

    // M10:排队消息队列——某一轮流式期间(RunTurn 内 TurnInputListener 存活
    // 那段窗口)敲了字回车,不会打断当前流,落进这里;本轮结束后逐条自动
    // 发出(包括 slash 命令),打法跟手输一模一样:打一行 "> <内容>" 再走
    // 下面同一套 process_line 逻辑。ESC 打断当前轮不影响这个队列——照样
    // 保留、照样发,跟"是不是被打断"完全解耦。
    std::deque<std::string> pending_queue;

    // 处理"确定不是空行、不是裸词 exit/quit"的一行输入,不管这行是刚
    // ReadLine() 读到的、还是从 pending_queue 里取出来的自动发送的——两条
    // 路径共用这一份 slash 分支 + 自动 compact 检查 + RunTurn 调用,行为
    // 完全一致(spec 要求"队列里是 slash 命令也认")。返回 false 表示这一行
    // 触发了 /exit,外层循环该退出了。
    const auto process_line = [&](const std::string& content) -> bool {
        const lubancode::cli::ParsedSlashCommand parsed = lubancode::cli::ParseSlashCommand(content);
        if (parsed.command != lubancode::cli::SlashCommand::NotSlash) {
            switch (parsed.command) {
                case lubancode::cli::SlashCommand::Help:
                    PrintSlashHelp();
                    break;
                case lubancode::cli::SlashCommand::Model:
                    HandleModelCommand(parsed.args, config, current_model, config_file_path, model_catalog,
                                        current_think, context_tracker, current_model_instructions);
                    break;
                case lubancode::cli::SlashCommand::Config:
                    PrintConfigDiagnostics(config_result, *current_model, &model_catalog);
                    break;
                case lubancode::cli::SlashCommand::Clear:
                    rebuild_loop();
                    // 存档跟着翻篇:旧文件留在磁盘上,新会话下一条消息另起
                    // 一份新文件(id 用新的时间戳)。标题属于旧场子,一并翻篇。
                    session_store.Reset();
                    session_start_ts = lubancode::agent::NowIdTimestamp();
                    persisted_count = 0;
                    session_store_broken = false;
                    session_title.clear();
                    session_title_pending = false;
                    std::cout << "已清空对话历史。\n";
                    break;
                case lubancode::cli::SlashCommand::Context:
                    HandleContextCommand(parsed.args, context_tracker);
                    break;
                case lubancode::cli::SlashCommand::Compact: {
                    const std::string compact_model = config.compact_model.empty() ? *current_model : config.compact_model;
                    const auto compact_event =
                        HandleCompactCommand(parsed.args, *loop, *real_backend, compact_model, theme, spinner_enabled);
                    // 压缩把 history 换短了(失败则原样):落盘基线收到新长度,
                    // 存档文件保持只追加——全量流水不动,补写一行 compact
                    // 事件,/resume 按事件回放出压缩后的活状态,/export 仍走
                    // 全量,不丢内容。
                    persisted_count = (std::min)(persisted_count, loop->History().size());
                    if (compact_event.has_value() && session_store.active() && !session_store_broken) {
                        session_store.AppendCompactEvent(*compact_event);
                    }
                    break;
                }
                case lubancode::cli::SlashCommand::Think:
                    // 目录条目按"此刻的会话模型"现查——/model 切过之后,
                    // /think 列的就是新模型声明的档位。
                    HandleThinkCommand(parsed.args, current_think, model_catalog.FindBySlug(*current_model));
                    break;
                case lubancode::cli::SlashCommand::Skills:
                    PrintSkillsCommand(skills, CurrentDirUtf8(), home_dir);
                    break;
                case lubancode::cli::SlashCommand::Mcp:
                    PrintMcpCommand(mcp_servers);
                    break;
                case lubancode::cli::SlashCommand::Lsp:
                    PrintLspCommand(lsp_manager);
                    break;
                case lubancode::cli::SlashCommand::Todos:
                    std::cout << lubancode::cli::FormatTodoList(todo_state->items, theme);
                    break;
                case lubancode::cli::SlashCommand::Plugins:
                    PrintPluginsCommand(plugin_mounted, plugin_warnings);
                    break;
                case lubancode::cli::SlashCommand::Tools:
                    PrintToolsCommand(registry, *loaded_tools, main_deferral, tool_search_threshold);
                    break;
                case lubancode::cli::SlashCommand::Sessions:
                    PrintSessionsCommand(sessions_dir, parsed.args);
                    break;
                case lubancode::cli::SlashCommand::Resume:
                    if (parsed.args.empty()) {
                        std::cout << "用法:/resume 编号或id(/sessions 看列表)\n";
                    } else {
                        ResumeSession(parsed.args, sessions_dir, *loop, session_store, persisted_count,
                                       session_meta, session_title, wire_str, *current_model, theme,
                                       /*quiet_if_none=*/false);
                        session_store_broken = false;  // 换了场,存档失败的旧账翻篇
                        session_title_pending = false;
                    }
                    break;
                case lubancode::cli::SlashCommand::Export:
                    HandleExportCommand(parsed.args, *loop, session_store, sessions_dir, session_meta,
                                         session_title);
                    break;
                case lubancode::cli::SlashCommand::Title:
                    if (parsed.args.empty()) {
                        std::cout << (session_title.empty() ? "本场还没设标题(/title 标题 起一个)。\n"
                                                             : "当前标题: " + session_title + "\n");
                    } else {
                        session_title = parsed.args;
                        if (session_store.active() && !session_store_broken) {
                            if (session_store.AppendTitleEvent(session_title)) {
                                std::cout << "标题已设为: " << session_title << "\n";
                            } else {
                                std::cout << theme.error << "[会话存档] 标题写入失败,只在本次会话内存里生效。"
                                           << theme.reset << "\n";
                            }
                        } else {
                            // 还没建档(首条消息才落盘):先记着,建档成功后
                            // 由 persist_new_messages 补写事件行。
                            session_title_pending = true;
                            std::cout << "标题已设为: " << session_title << "(首条消息落盘后写入存档)\n";
                        }
                    }
                    break;
                case lubancode::cli::SlashCommand::Soul:
                    HandleSoulCommand(parsed.args, current_soul, current_soul_name, config_file_path);
                    break;
                case lubancode::cli::SlashCommand::Prompt:
                    HandlePromptCommand(parsed.args, law_source, persona);
                    break;
                case lubancode::cli::SlashCommand::Exit:
                    return false;
                case lubancode::cli::SlashCommand::Unknown:
                    std::cout << "不认得命令 " << parsed.raw_word << ",试试 /help\n";
                    break;
                case lubancode::cli::SlashCommand::NotSlash:
                    break;  // 走不到这里,switch 外层已经排除了
            }
            return true;
        }

        // 自动压缩:发真正的用户输入前,占用超过阈值(80%)就先压一压。
        // 用裸的 *real_backend(理由同 /compact),失败只警告不拦——字符数
        // 硬安全网(TrimHistory)还在,不会真的爆掉。
        if (context_tracker.ShouldAutoCompact()) {
            std::cout << theme.stats << "[compact] 上下文接近上限,自动压缩中..." << theme.reset << "\n";
            lubancode::cli::Spinner spinner(theme, spinner_enabled);
            const std::string compact_model = config.compact_model.empty() ? *current_model : config.compact_model;
            const auto compact_result = lubancode::agent::Compact(*real_backend, compact_model, loop->History(), "");
            spinner.Stop();
            if (compact_result.has_value()) {
                const std::size_t old_size = loop->History().size();
                const auto new_history = lubancode::agent::BuildCompactedHistory(loop->History(), *compact_result);
                const auto compact_event = lubancode::agent::MakeCompactEvent(old_size, new_history);
                loop->ReplaceHistory(new_history);
                // 落盘基线收到新长度,补写 compact 事件,理由同 /compact 分支。
                persisted_count = (std::min)(persisted_count, loop->History().size());
                if (session_store.active() && !session_store_broken) {
                    session_store.AppendCompactEvent(compact_event);
                }
                std::cout << "[compact] 自动压缩完成。\n";
            } else {
                std::cout << theme.error << "[compact] 自动压缩失败: " << compact_result.error().message
                           << theme.reset << "(继续按原历史发送,字符数安全网仍会兜底)\n";
            }
        }

        // 人在聚焦查看画面里直接敲了正文发送:视为离开聚焦态(新一轮输出
        // 马上往下铺,聚焦画面已经不是"当前画面"了),下次 Ctrl+E 是重新
        // 聚焦,不是"返回"。
        focus_view_active = false;
        const RunTurnResult turn_result =
            RunTurn(*loop, content, auto_confirm, always_allowed_tools, theme, context_tracker, registry,
                    config.hooks, spinner_enabled, transcript, todo_state, &transcript_expanded);
        // 每轮结束(成功/出错/ESC 打断都算)把新增消息逐条追加落盘。
        persist_new_messages();
        for (auto& queued : turn_result.queued_lines) {
            pending_queue.push_back(std::move(queued));
        }
        return true;
    };

    while (true) {
        std::string content;
        if (!pending_queue.empty()) {
            // 队列非空:先把队列里排在最前面的这条自动发出去,不再等
            // ReadLine()——跟手输的视觉一致,打一行 "> <内容>" 再处理。
            content = std::move(pending_queue.front());
            pending_queue.pop_front();
            std::cout << theme.prompt << "> " << theme.reset << content << "\n";
        } else {
            // 0.17.0:每次给主提示符之前刷新常驻状态行数据——模型名跟着
            // /model 实时变,context 百分比每轮结束后就是新的,反正循环每圈
            // 都路过这里,不用另找刷新点。
            lubancode::cli::SetStatusLineData(
                *current_model, context_tracker.UsagePercent(),
                static_cast<long long>(context_tracker.current_tokens()),
                static_cast<long long>(context_tracker.window_tokens()));
            // UI-A:主提示符是唯一开 composer 的读取点——Alt/Shift+Enter 插
            // 换行、Enter 全发、全空白不发送。别的 ReadLine 调用点(确认提示、
            // /model 编号选择、向导)保持单行语义。
            const std::optional<std::string> line =
                lubancode::cli::ReadLine(theme.prompt + "> " + theme.reset, theme,
                                          /*esc_rejects=*/false, /*composer=*/true);
            if (!line.has_value()) {
                break;  // EOF:Ctrl+Z 或管道读尽
            }
            if (line->empty()) {
                continue;  // 空行不退出,重新给提示符
            }
            content = *line;
        }

        if (content == "exit" || content == "quit") {
            break;
        }
        if (!process_line(content)) {
            break;
        }
    }
}

// 单发模式(位置参数):也走 agent loop,同样支持工具,只是只问这一句。
// 管道/单发场景下 spinner_enabled 传进来的必然是 false(RunCli 里按
// DetectConsoleCapability().is_console 算好的),这里不用再判断一次。
// model_instructions:模型目录里当前模型的 base_instructions(RunCli 按
// 目录算好传进来,不在目录就是空串)。单发模式没有 /model,不用会话级
// 状态和包装层,构造 AgentLoop 时直接拼进系统提示,结构跟交互模式发出去
// 的一模一样;think/context_window 的目录应用同样由 RunCli 预先并进
// config,这里不重复判断——保持这个函数只管"按给定配置问一句"。
// soul_content(0.16.x 魂法分家):当前魂文件的原始内容(RunCli 按配置的
// soul 名读好传进来),单发模式没有 /soul,构造时直接叠加在系统提示最后
// (WithModelInstructions 之后,压轴),跟交互模式发出去的结构一模一样;
// 空串 = 不叠加。
int AskOnce(const lubancode::config::Config& config, const std::string& question, bool auto_confirm,
            const lubancode::cli::Theme& theme, const std::string& persona, bool spinner_enabled,
            const std::string& model_instructions = std::string(), const std::string& soul_content = std::string()) {
    // M9:技能扫描,理由同 InteractiveLoop——单发模式也该能用技能。
    const std::optional<std::string> home_dir = lubancode::config::HomeDir();
    const std::vector<lubancode::tools::SkillMeta> skills = lubancode::tools::LoadSkills(CurrentDirUtf8(), home_dir);
    const std::string skills_segment = lubancode::tools::BuildSkillsPromptSegment(skills);

    std::unique_ptr<lubancode::api::Backend> backend = BuildBackend(config);
    // 单发模式没有 /think 命令,current_think 构造后不会再变,等价于直接
    // 按配置里的 think 发一次。
    auto current_think = std::make_shared<std::string>(config.think);
    ThinkOverrideBackend think_backend(*backend, current_think);
    SpinnerBackend wrapped_backend(think_backend, theme, spinner_enabled);

    // M8:mcp_servers 声明在 sub_registry/registry 之前,理由同
    // InteractiveLoop(析构反序,MCP 工具持有的 Client& 不会悬垂)。单发
    // 模式没有横幅、没有 /mcp 命令,起服务器就在构建工具表之前干净利落地做。
    std::vector<McpServerRuntime> mcp_servers = StartMcpServers(config.mcp_servers, theme);

    // M7:插件宿主同样声明在 registry 之前(析构反序,理由见 MountPlugins
    // 注释)。单发模式没有 /plugins 命令,mounted/warnings 只是占位。
    lubancode::tools::PluginHost plugin_host;
    std::vector<PluginMountInfo> plugin_mounted;
    std::vector<std::string> plugin_warnings;
    // LSP:声明在两份 registry 之前,理由同 InteractiveLoop(析构反序,
    // LspTool 持有的 Manager& 不会悬垂)。单发模式没有 /lsp 命令可看,但
    // 工具本身照样能用;AskOnce 返回时 Manager 析构,起过的服务器全关。
    std::optional<lubancode::lsp::Manager> lsp_manager;
    if (!config.lsp_servers.empty()) {
        lsp_manager.emplace(config.lsp_servers, CurrentDirUtf8());
    }

    // 两份工具表,理由同 InteractiveLoop:sub_registry 喂给 agent 工具当
    // "子代理能用什么"(不含 agent 自己,防递归),registry 是主循环真正
    // 用的那份,基础工具之外多注册了 agent 工具本身。MCP 工具同样注册进
    // 两份。
    lubancode::tools::ToolRegistry sub_registry = BuildBaseToolRegistry(skills, config.search);
    lubancode::tools::ToolRegistry registry = BuildBaseToolRegistry(skills, config.search);
    RegisterMcpTools(mcp_servers, sub_registry);
    RegisterMcpTools(mcp_servers, registry);
    if (lsp_manager.has_value()) {
        sub_registry.Register(std::make_unique<lubancode::tools::LspTool>(*lsp_manager));
        registry.Register(std::make_unique<lubancode::tools::LspTool>(*lsp_manager));
    }
    registry.Register(std::make_unique<lubancode::tools::AgentTool>(
        wrapped_backend, sub_registry, CurrentDirUtf8(), config.model, /*default_max_turns=*/15, skills_segment));
    // M11(0.10.0):单发模式也挂 todo_write(只挂主 registry,不挂 sub_registry,
    // 理由同 InteractiveLoop)——单发问答一样可能是"先列计划再分步做"这种
    // 多步骤任务,没道理只有交互模式能用这个工具。没有 /todos 命令可看
    // (单发模式压根没有下一轮循环),但 on_tool_done 该渲染的时候照样渲染。
    auto todo_state = std::make_shared<lubancode::tools::TodoListState>();
    registry.Register(std::make_unique<lubancode::tools::TodoWriteTool>(todo_state));
    // M7:插件同样只挂主 registry,不挂 sub_registry。
    MountPlugins(plugin_host, registry, theme, plugin_mounted, plugin_warnings);

    // tool_search(延迟挂载):跟 InteractiveLoop 同一套接线,理由见那边的
    // 大段注释。单发模式没有 /tools 命令可看,机制本身照常生效。
    auto loaded_tools = std::make_shared<std::set<std::string>>();
    const int tool_search_threshold = config.tool_search_threshold;
    const bool main_deferral = lubancode::tools::DeferralEnabled(registry.All().size(), tool_search_threshold);
    const bool sub_deferral = lubancode::tools::DeferralEnabled(sub_registry.All().size(), tool_search_threshold);
    if (main_deferral) {
        registry.Register(std::make_unique<lubancode::tools::ToolSearchTool>(registry, loaded_tools));
    }
    if (sub_deferral) {
        sub_registry.Register(std::make_unique<lubancode::tools::ToolSearchTool>(sub_registry, loaded_tools));
    }
    std::function<bool(const lubancode::tools::Tool&)> tool_filter;
    if (main_deferral || sub_deferral) {
        tool_filter = [loaded_tools](const lubancode::tools::Tool& tool) {
            return !tool.deferred() || loaded_tools->count(tool.name()) != 0;
        };
    }
    if (auto* agent_tool = dynamic_cast<lubancode::tools::AgentTool*>(registry.Find("agent"));
        agent_tool != nullptr && sub_deferral) {
        agent_tool->SetToolFilter(tool_filter);
        agent_tool->SetDeferredIndexProvider([&sub_registry, loaded_tools]() {
            return lubancode::tools::BuildDeferredToolsIndexSegment(sub_registry, *loaded_tools);
        });
    }
    DeferredIndexBackend index_backend(
        wrapped_backend, [&registry, loaded_tools, main_deferral]() {
            return main_deferral ? lubancode::tools::BuildDeferredToolsIndexSegment(registry, *loaded_tools)
                                  : std::string();
        });

    lubancode::agent::AgentLoop loop(
        index_backend, registry, config.model,
        lubancode::agent::WithSoul(
            lubancode::agent::WithModelInstructions(
                lubancode::agent::BuildSystemPrompt(CurrentDirUtf8(), persona, skills_segment), model_instructions),
            soul_content),
        /*max_tokens=*/4096, /*max_turns=*/25, config.max_context_chars);
    if (main_deferral) {
        loop.SetToolFilter(tool_filter);
    }
    std::set<std::string> always_allowed_tools;
    lubancode::cli::ContextTracker context_tracker(config.context_window_tokens);

    // 单发模式没有下一轮循环好把排队消息接着发出去——AskOnce 只问这一句就
    // 退出,ESC/排队这套机制天生只对交互循环有意义(spec 也只要求交互模式
    // 的手测清单),这里只取 status,忽略 queued_lines/cancelled。
    std::vector<lubancode::cli::TranscriptItem> transcript;
    return RunTurn(loop, question, auto_confirm, always_allowed_tools, theme, context_tracker, registry,
                    config.hooks, spinner_enabled, transcript, todo_state)
        .status;
}

// M9:session_start/session_end 钩子的生命周期跟"这一次 CLI 进程真正进入了
// 一次会话"绑在一起——构造时跑 session_start,析构时跑 session_end。只在
// RunCli 真正要进 AskOnce/InteractiveLoop 那条路时构造(--config/--version/
// --help 这些提前 return 的路径不会走到这里,不该触发)。用的是最初
// LoadFromEnv() 读出来的那份 hooks 配置,不是初次配置向导之后的
// "effective"副本——向导只关心 wire/base_url/api_key/model 四个字段,压根
// 不知道 hooks 这回事,拿它的副本反而会把用户配置文件里写的 hooks 弄丢。
class SessionHookScope {
public:
    explicit SessionHookScope(const lubancode::config::HooksConfig& hooks) : hooks_(hooks) {
        lubancode::tools::RunSessionStartHooks(hooks_);
    }
    ~SessionHookScope() { lubancode::tools::RunSessionEndHooks(hooks_); }

    SessionHookScope(const SessionHookScope&) = delete;
    SessionHookScope& operator=(const SessionHookScope&) = delete;

private:
    lubancode::config::HooksConfig hooks_;
};

// 真正的入口逻辑,跟平台无关:args[0] 是程序名,args[1..] 是实参。
// Windows 下 argv 单独处理(见文件末尾的 wmain),是为了绕开 Windows
// 那套"窄字符 argv 按系统 ANSI 代码页解码"的老规矩——命令行里的中文字符
// 一旦经这条路转一圈,就会被拆成不合法的 UTF-8 字节,喂给 nlohmann::json
// 的 dump() 时直接抛 type_error(316: invalid UTF-8 byte)崩掉。
int RunCli(const std::vector<std::string>& args) {
    std::string positional;
    bool auto_confirm = false;
    bool print_config = false;
    bool continue_last = false;
    std::string system_prompt_file_arg;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--continue") {
            continue_last = true;
            continue;
        }
        if (arg == "--version") {
            PrintVersion();
            return 0;
        }
        if (arg == "--help") {
            PrintHelp();
            return 0;
        }
        if (arg == "--yes") {
            auto_confirm = true;
            continue;
        }
        if (arg == "--config") {
            print_config = true;
            continue;
        }
        if (arg == "--system-prompt") {
            if (i + 1 >= args.size()) {
                std::cerr << "--system-prompt 后面要跟一个文件路径\n";
                return 1;
            }
            system_prompt_file_arg = args[++i];
            continue;
        }
        if (arg == "--reset-system-prompt") {
            // 跟 /prompt reset 同效,只是不进交互、不二次确认(命令行参数
            // 本身就是明确意图),打结果就退。
            const auto luban_dir = lubancode::config::HomeLubancodeDir();
            if (!luban_dir.has_value()) {
                std::cerr << "找不到用户主目录(Windows 下是 %USERPROFILE%),没法还原 system_prompt.md\n";
                return 1;
            }
            const auto reset_result =
                lubancode::config::ResetSystemPromptFile(*luban_dir, lubancode::agent::DefaultPersona());
            if (!reset_result.has_value()) {
                std::cerr << "还原失败: " << reset_result.error() << "\n";
                return 1;
            }
            std::cout << "已把 " << lubancode::config::SystemPromptFilePath(*luban_dir) << " 还原成内置默认";
            if (!reset_result->empty()) {
                std::cout << ",旧文件在 " << *reset_result;
            }
            std::cout << "。\n";
            return 0;
        }
        if (!positional.empty()) {
            positional += " ";
        }
        positional += arg;
    }

    const auto config_result = lubancode::config::LoadFromEnv();
    if (!config_result.has_value()) {
        std::cerr << config_result.error() << "\n";
        return 1;
    }
    if (config_result->migration_notice.has_value()) {
        std::cout << *config_result->migration_notice << "\n";
    }

    // 魂法分家(0.16.x):每次启动查漏补缺——~/.lubancode/ 下的
    // system_prompt.md(法)/ SOUL.md(魂)/ souls/wenyan.md(示例)缺哪样
    // 补哪样,已存在的绝不覆盖。首启就是"三样全补"。静默做,不打输出
    // (单发/管道模式的输出常被重定向,不该混进这些话)。
    if (const auto luban_dir = lubancode::config::HomeLubancodeDir(); luban_dir.has_value()) {
        lubancode::config::EnsurePromptScaffold(*luban_dir, lubancode::agent::DefaultPersona());
    }

    // 模型目录(models.json):启动时读一次,坏 JSON/坏条目只打警告跳过,
    // 文件不存在就是空目录,一切回退现状,绝不拦人。
    const lubancode::config::ModelCatalog model_catalog = lubancode::config::LoadModelCatalog();
    for (const auto& warning : model_catalog.warnings) {
        std::cout << "[models.json 警告] " << warning << "\n";
    }

    if (print_config) {
        PrintConfigDiagnostics(*config_result, std::nullopt, &model_catalog);
        return 0;
    }

    // --system-prompt 命令行参数压过配置文件里的 system_prompt_file 字段
    // (四级合并已经把 config.system_prompt_file 算好了,这里只是命令行
    // 再压一级)。只替换人格段,工作目录、工具调用这些运行必需的上下文
    // (prompts.hpp 的 EnvironmentSegment)照样由 BuildSystemPrompt 追加,
    // 不受这里影响。
    const std::string effective_prompt_file =
        !system_prompt_file_arg.empty() ? system_prompt_file_arg : config_result->config.system_prompt_file;
    std::string persona;
    std::string law_source = "内置默认(DefaultPersona)";  // /prompt 裸敲展示用
    if (!effective_prompt_file.empty()) {
        const auto persona_result = lubancode::config::ReadSystemPromptFile(effective_prompt_file);
        if (!persona_result.has_value()) {
            std::cerr << persona_result.error() << "\n";
            return 1;
        }
        persona = *persona_result;
        law_source = (!system_prompt_file_arg.empty() ? std::string("CLI 参数 --system-prompt(")
                                                       : std::string("配置指定的人格文件(")) +
                     effective_prompt_file + ")";
    } else if (const auto luban_dir = lubancode::config::HomeLubancodeDir(); luban_dir.has_value()) {
        // 魂法分家:没有 CLI/配置指定的人格文件时,法从 ~/.lubancode/
        // system_prompt.md 来(顶部注释剥掉;文件缺失或剥完全空白,persona
        // 留空串,BuildSystemPrompt 自回退内置默认)。
        const std::string law_path = lubancode::config::SystemPromptFilePath(*luban_dir);
        const auto law_content = lubancode::config::ReadTextFileIfExists(law_path);
        persona = lubancode::agent::ResolvePersona(std::string(), law_content.value_or(std::string()));
        if (!persona.empty()) {
            law_source = "文件 " + law_path;
        }
    }

    // 主题、转轮开关这两样跟"配没配好模型"无关,不管走哪条路都先算好。
    // DetectConsoleCapability() 内部已经处理了 LUBANCODE_FORCE_COLOR 强制
    // 着色的情况(管道模式下也能测出 ANSI 序列),但转轮不受这个开关影响——
    // is_console 为假(管道)时无论如何都不转,免得转轮字符污染管道输出。
    const lubancode::cli::ConsoleCapability console_cap = lubancode::cli::DetectConsoleCapability();
    const lubancode::cli::Theme theme =
        lubancode::cli::ResolveTheme(config_result->config.theme, console_cap.colors_enabled);
    const bool spinner_enabled = console_cap.is_console;

    // --yes 等价于起手就把会话级确认模式切到 yolo(全自动,needs_confirm
    // 的工具一概放行)——单发模式(AskOnce)也一起设,虽然单发模式走不到
    // Shift+Tab 那条路,但 on_tool_confirm 统一查 CurrentConfirmMode(),
    // 这里设了才对得上。
    lubancode::cli::SetConfirmMode(auto_confirm ? lubancode::cli::ConfirmMode::Yolo
                                                 : lubancode::cli::ConfirmMode::Confirm);

    // M9:真正要进一次会话了(单发问答也算一次会话)——session_start 在这里
    // 跑,session_end 在这个作用域结束(RunCli 返回、或者中途抛异常被下面
    // catch 住之后自然析构)时跑。--config/--version/--help 提前 return,
    // 走不到这里,不会触发。
    const SessionHookScope session_hook_scope(config_result->config.hooks);

    // 兜底:JSON 编码、网络库内部等地方万一抛出没接住的异常,也不能让
    // 整个进程崩掉(崩掉的话用户只会看到一个莫名其妙的退出码)。
    try {
        if (!positional.empty()) {
            // 单发模式/管道模式:不进向导(没有交互终端可问),缺配置直接
            // 报可读的错,指路三条配置途径。
            const auto configured_check = lubancode::config::RequireConfigured(*config_result);
            if (!configured_check.has_value()) {
                std::cerr << configured_check.error() << "\n";
                return 1;
            }
            // 模型目录应用(单发模式):think/context_window 直接并进这份
            // 一次性的配置副本(用户显式配过的不动,跟交互模式同一条规矩),
            // base_instructions 单独传给 AskOnce 拼进系统提示。不打提示行——
            // 单发/管道模式的输出常被重定向,不该混进这些会话性的话。
            lubancode::config::Config once_config = config_result->config;
            const auto catalog_apply = lubancode::config::ComputeCatalogApplication(
                model_catalog, once_config.model,
                config_result->sources.think != lubancode::config::Source::Default,
                config_result->sources.context_window_tokens != lubancode::config::Source::Default);
            if (catalog_apply.think.has_value()) {
                once_config.think = *catalog_apply.think;
            }
            if (catalog_apply.context_window_tokens.has_value()) {
                once_config.context_window_tokens = *catalog_apply.context_window_tokens;
            }
            // 魂:按配置的 soul 名读一次(缺文件不警告——管道输出保持干净),
            // 拼在系统提示最后。
            const std::string soul_content =
                LoadSoulContentByName(once_config.soul.empty() ? "default" : once_config.soul, /*warn=*/false);
            return AskOnce(once_config, positional, auto_confirm, theme, persona, spinner_enabled,
                            catalog_apply.base_instructions, soul_content);
        }

        // 交互模式:base_url/api_key/model 有一个解不出来,就先走一遍初次
        // 配置向导——三个字段都没有内置默认值,任何一个空着都没法真的
        // 跟模型对话,不如趁手就问清楚(即便本次规矩里描述的触发条件只提了
        // base_url/api_key,这里多加一条 model 判断更稳妥,免得 env 只配了
        // base_url/api_key 漏了 model,走进会话却发不出请求)。
        lubancode::config::ConfigResult effective = *config_result;
        if (effective.config.base_url.empty() || effective.config.auth_token.empty() ||
            effective.config.model.empty()) {
            const auto wizard_config = RunInitialSetupWizard(effective.config_file_path);
            if (!wizard_config.has_value()) {
                std::cerr << "配置向导未完成,退出。\n";
                return 1;
            }
            effective.config = *wizard_config;
            // 向导给出的这份配置,来源标记简化成两种:保存了就算"配置文件"
            // 来源,没保存就算"内置默认值"(最接近"临时值,没有更合适的
            // 持久来源"这个语义)——/config 展示用,不影响实际发请求。
            const lubancode::config::Source marked = effective.config_file_path.has_value()
                                                          ? lubancode::config::Source::ConfigFile
                                                          : lubancode::config::Source::Default;
            effective.sources.wire = marked;
            effective.sources.base_url = marked;
            effective.sources.auth_token = marked;
            effective.sources.model = marked;
        }
        InteractiveLoop(effective, auto_confirm, theme, persona, spinner_enabled, model_catalog, continue_last,
                         law_source);
    } catch (const std::exception& e) {
        std::cerr << "[错误] 未预料的异常: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

}  // namespace

#ifdef _WIN32

// Windows 下用宽字符入口:窄字符 main(argc, char**) 的 argv 是 CRT 按
// "系统 ANSI 代码页"(不是 UTF-8)解码来的,中文命令行参数会被解码错。
// wmain 拿到的是原始的 UTF-16 参数,自己用 WideCharToMultiByte 转成
// UTF-8,才能跟程序内部统一按 UTF-8 处理的字符串对上。
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        const int utf8_len = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string utf8;
        if (utf8_len > 0) {
            utf8.resize(static_cast<std::size_t>(utf8_len - 1));  // 去掉结尾的 \0
            if (utf8_len > 1) {
                WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, utf8.data(), utf8_len, nullptr, nullptr);
            }
        }
        args.push_back(std::move(utf8));
    }
    return RunCli(args);
}

#else

int main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);
    return RunCli(args);
}

#endif
