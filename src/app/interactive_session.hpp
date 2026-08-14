// 交互会话的窄接口:头文件只露选项与入口,具体类(整场状态、slash 分派、
// 排队与跨会话消息、轮次发送与存档落盘)全藏在 interactive_session.cpp
// 的 InteractiveSession 里。底层只认 agent/api/cli/config/memory/mcp/lsp/
// tools/platform 的抽象,不反被任何层 include。
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
};

// 没带位置参数时的交互会话:读一行、问一句,exit/quit 或 EOF 退出。
// 空行不退出——只是重新给一次提示符。返回值沿用旧约定恒 0,出错路径由
// 内部自行报错并退场;对象构造/析构(含异常退场)由 cpp 内的
// InteractiveSession 按所有权守门。
int RunInteractiveSession(const InteractiveSessionOptions& options);

}  // namespace lubancode::app
