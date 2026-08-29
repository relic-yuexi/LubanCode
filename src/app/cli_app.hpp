// CLI 应用编排:版本/帮助打印、初次配置向导、参数解析与 RunCli——
// 装配好一切后选择交互会话或单发模式。
// 实现在 cli_app.cpp(编译边界):session hooks 作用域、启动装配等实现
// 细节不从头文件漏出去。ParseCliArgs 纯函数化是后续 commit 的事。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "app/cli_options.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"

namespace lubancode::app {

void PrintVersion();

// i18n:帮助文本按节进表(help.title/usage/options/scaffold/slash/config),
// 版本号、三个内置默认值走占位符。zh-CN 表的值与旧字面文案一致。
void PrintHelp();

// 初次启动页:可直接进 /provider add 同款目录向导，也可明选“暂时跳过”后
// 进入主界面。只有 EOF/Ctrl+C/Esc 才返回 nullopt；跳过会返回原配置，不再
// 被当作“向导未完成”。
std::optional<lubancode::config::ConfigResult> RunInitialSetupWizard(
    const lubancode::config::ConfigResult& current, const lubancode::cli::Theme& theme);

// 真正的入口逻辑,跟平台无关:args[0] 是程序名,args[1..] 是实参。
// Windows 下 argv 单独处理(见 main.cpp),是为了绕开 Windows 那套
// "窄字符 argv 按系统 ANSI 代码页解码"的老规矩——命令行里的中文字符
// 一旦经这条路转一圈,就会被拆成不合法的 UTF-8 字节,喂给 nlohmann::json
// 的 dump() 时直接抛 type_error(316: invalid UTF-8 byte)崩掉。
int RunCli(const std::vector<std::string>& args);

// app-server 子模式(无界面后台协议):装配前奏(配置/i18n/hooks)已在
// RunCli 里跑完,这里立服务进主循环。stdout 从此是协议专线。配了
// --app-server-ws 就走 WS 监听承载(stdout 不再是协议口)。
int RunAppServerMode(const lubancode::config::ConfigResult& config_result,
                     const CliOptions& cli_options);

// `lubancode plugin init <模板> [名字]` 子命令(plugins 单第 3 步):生成
// 插件脚手架后退出。i18n 已初始化,配置不加载——脚手架不该因为模型没配
// 好而拒绝干活。
int HandlePluginInitCommand(const PluginInitArgs& init);

// Plan 模式单:起手协作档的优先级(高到低):--mode >
// LUBANCODE_COLLABORATION_MODE > settings.local.json 的
// default_collaboration_mode > Default。CLI 的非法值已在解析层退 BadMode;
// env/settings 的非法值明报到 stderr 并按 Default 走。
bool ResolveStartupPlanMode(const CliOptions& cli_options, const config::SettingsLocal& settings_local);

}  // namespace lubancode::app
