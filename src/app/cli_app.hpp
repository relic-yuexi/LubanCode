// CLI 应用编排:版本/帮助打印、初次配置向导、参数解析与 RunCli——
// 装配好一切后选择交互会话或单发模式。
// 实现在 cli_app.cpp(编译边界):session hooks 作用域、启动装配等实现
// 细节不从头文件漏出去。ParseCliArgs 纯函数化是后续 commit 的事。
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cli/theme.hpp"
#include "config/config.hpp"

namespace lubancode::app {

void PrintVersion();

// i18n:帮助文本按节进表(help.title/usage/options/scaffold/slash/config),
// 版本号、三个内置默认值走占位符。zh-CN 表的值与旧字面文案一致。
void PrintHelp();

// 初次配置向导:接 cli::ReadLine 做输入、std::cout 做输出、api::ListModels
// 做模型列表拉取。用户中途 EOF(Ctrl+Z / 管道读尽)放弃时返回 std::nullopt。
// 用户选择保存时,把保存后的路径写进 out_config_file_path,好让接下来的
// /model 命令知道"有配置文件"。
std::optional<lubancode::config::Config> RunInitialSetupWizard(std::optional<std::string>& out_config_file_path,
                                                                const lubancode::cli::Theme& theme);

// 真正的入口逻辑,跟平台无关:args[0] 是程序名,args[1..] 是实参。
// Windows 下 argv 单独处理(见 main.cpp),是为了绕开 Windows 那套
// "窄字符 argv 按系统 ANSI 代码页解码"的老规矩——命令行里的中文字符
// 一旦经这条路转一圈,就会被拆成不合法的 UTF-8 字节,喂给 nlohmann::json
// 的 dump() 时直接抛 type_error(316: invalid UTF-8 byte)崩掉。
int RunCli(const std::vector<std::string>& args);

}  // namespace lubancode::app
