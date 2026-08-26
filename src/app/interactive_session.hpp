// 终端会话控制器的窄接口(原 interactive_session.hpp):头文件只露选项
// 与入口,具体类(整场终端状态、slash 分派、排队与跨会话消息、轮次发送
// 与存档落盘的终端接线)全藏在 interactive_session.cpp 的
// TerminalSessionController 里(原 InteractiveSession,显示系统剥离单第
// 六步更名:会话的内核账本已搬 runtime::SessionRuntime,这里只剩终端
// 控制器的活)。底层只认 agent/api/cli/config/memory/mcp/lsp/tools/
// platform 的抽象,不反被任何层 include。
//
// 依赖规矩:这里不 include MCP、LSP、具体 backend、终端 painter、nlohmann;
// config/model_catalog 等重类型只以前置声明借引用。
#pragma once

#include <string>

namespace lubancode::cli {
struct Theme;
}
namespace lubancode::config {
struct ConfigResult;
struct ModelCatalog;
struct SettingsLocal;
}
namespace lubancode::app {
struct SessionStack;  // app/session_stack.hpp(组合根装配件,会话终章)
}

namespace lubancode::app {

// RunInteractiveSession 的入参。config/theme/model_catalog/settings_local
// 是借用:调用方(RunCli)在 RunInteractiveSession 返回前保证存活,会话
// 内部自己留可变副本。law_source 是启动时算好的"法从哪儿来"说明
// (CLI 参数/文件/内置),/prompt 裸敲展示用;executable 是本程序绝对
// 路径(project memory 的 worker 重启用)。
struct InteractiveSessionOptions {
    const lubancode::config::ConfigResult& config_result;
    const lubancode::cli::Theme& theme;
    const lubancode::config::ModelCatalog& model_catalog;
    const lubancode::config::SettingsLocal& settings_local;
    bool auto_confirm = false;
    std::string persona;
    bool spinner_enabled = false;  // 真控制台才真(管道/重定向恒 false)
    bool continue_last = false;    // --continue:进循环前先自动 /resume 最近一场
    std::string law_source = "内置默认";
    std::string executable;
    // Plan 模式单:起手档(CLI/env/settings 已在 RunCli 按优先级算好)。
    // --continue 恢复出的档位压过它(resume 的 mode 事件才是真账)。
    bool start_in_plan = false;
    // 组合根装配件(会话终章):cli_app 调 BuildSessionStack 装好递进来,
    // 控制器只收装好的件。空 = 调用方没走组合根(单测/旧调用点),入口
    // 自己现装一份,两条路共用同一只 BuildSessionStack。
    SessionStack* stack = nullptr;
};

// 没带位置参数时的交互会话:读一行、问一句,exit/quit 或 EOF 退出。
// 空行不退出——只是重新给一次提示符。返回值沿用旧约定恒 0,出错路径由
// 内部自行报错并退场;对象构造/析构(含异常退场)由 cpp 内的
// TerminalSessionController 按所有权守门。
int RunInteractiveSession(const InteractiveSessionOptions& options);

}  // namespace lubancode::app
