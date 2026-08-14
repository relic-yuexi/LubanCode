// 提示词类 slash 命令:/soul(魂,风格叠加层)与 /prompt(法,行为骨架)
// 的接命令、找文件、打印、问一句这层壳。纯拼接/剥注释在 agent/prompts,
// 文件生成/还原/扫描在 config/prompt_files,这里不做逻辑。
//
// 搬家自 main.cpp,行为一字未改;依赖只认 agent/cli/config。


#pragma once

#include <memory>
#include <optional>
#include <string>

#include "cli/console_input.hpp"
#include "cli/i18n.hpp"
#include "cli/theme.hpp"
#include "config/prompt_files.hpp"

namespace lubancode::app {

using lubancode::cli::tr;
using lubancode::cli::trf;

// ---------------------------------------------------------------------------
// 魂法分家(0.16.x):/soul //prompt 的执行逻辑。纯拼接/剥注释在
// agent/prompts.hpp,文件生成/还原/扫描在 config/prompt_files,这里只做
// "接命令、找文件、打印、问一句"这层壳。
// ---------------------------------------------------------------------------

// 数一段 UTF-8 文本有多少个字符(码点)——/prompt 报"字数"用,字节数对
// 中文没意义。
std::size_t CountUtf8Chars(const std::string& text);


// 按魂名读内容(原始全文,注释留给注入时剥):"off" -> 空;空串/"default"
// -> SOUL.md;别的名字 -> souls/<名字>.md。默认 SOUL.md 走专用读口:缺失、
// 打不开或 UTF-8 已坏都降成空魂,不拦启动。warn 为真时打一行说明。启动读
// 一次、/soul 切换即时重读,都走这一个函数。
std::string LoadSoulContentByName(const std::string& name, bool warn);


// /soul 命令:裸敲看当前正文和可选旧魂;/soul clear 把 SOUL.md 还原成
// 默认空魂;/soul <内容> 直接写 SOUL.md、立刻生效,下回启动也会读回来。
// 兼容旧用法:参数恰好命中 souls/<名字>.md 时仍是选魂。off/default/
// <名字> 三条路都当场生效,并在有配置文件时问一句要不要持久化——答 y
// 才落盘,免得下次启动被配置里的旧值悄悄盖过去(或者悄悄留着没改)。
// clear 语义不同,是把 SOUL.md 本身还原成空魂,所以自动把配置里的选魂
// 项归位 default,不用问。
void HandleSoulCommand(const std::string& args, const std::shared_ptr<std::string>& current_soul,
                        std::string& current_soul_name, const std::optional<std::string>& config_file_path);


// /prompt 命令:裸敲显示当前法(人格段)的来源和字数,外加各提示词模块
// 的来源统计(用户文件/内置,0.21.x 运行时化);/prompt reset 带二次确认,
// 把 system_prompt.md 还原成内置默认(旧文件挪成 .bak)。
// persona 是本会话实际在用的人格段(空串 = 内置默认);law_source 是启动时
// 算好的来源说明(CLI 参数/文件/内置);prompts_dir 是用户模块目录
// (~/.lubancode/prompts,找不到主目录时空串)。
void HandlePromptCommand(const std::string& args, const std::string& law_source, const std::string& persona,
                          const std::string& prompts_dir);

}  // namespace lubancode::app
