// 提示词类 slash 命令:/soul(魂,风格叠加层)与 /prompt(法,行为骨架)
// 的接命令、找文件、打印、问一句这层壳。纯拼接/剥注释在 agent/prompts,
// 文件生成/还原/扫描在 config/prompt_files,这里不做逻辑。
//
// 搬家自 main.cpp,行为一字未改;依赖只认 agent/cli/config。


#pragma once

#include "app/commands/command_flow.hpp"  // CommandFlow(分派注册制)
#include "cli/slash_commands.hpp"          // ParsedSlashCommand(分派注册制)

#include <memory>
#include <optional>
#include <string>

#include "cli/console_input.hpp"
#include "cli/i18n.hpp"
#include "cli/theme.hpp"
#include "config/prompt_files.hpp"
#include "runtime/session_soul.hpp"  // SessionSoulSnapshot:Soul 会话冻结单 P0

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


// /soul 命令(Soul 会话冻结单 P0 后的新语义,单内 §5.2 表):
//   session_soul   —— 本会话快照(草稿或已锁定快照);锁定前可改,锁定后
//                     只读展示。
//   configured_content/configured_name —— configuredSoul 持久默认的内存
//                     映像(SOUL.md + 配置 soul: 项),供以后新建会话读取。
// 分支口径:
//   裸敲       展示本会话草稿/快照、默认值、锁定状态与差异;
//   <内容/名称> 先校验保存默认值,成功才更新草稿(锁定后只保存默认);
//   off        默认选择 off(不删正文);clear 才清空默认正文;
//   default    解析默认文件,保存选择;锁定后只改默认选择。
// 保存失败报错回滚,不宣称成功、不留内存/磁盘两份账;内容规范化后相同
// 提示"内容未变",不记虚假 pending/revision。首版没有 apply-now/force。
void HandleSoulCommand(const std::string& args, lubancode::runtime::SessionSoulSnapshot& session_soul,
                       const std::shared_ptr<std::string>& configured_content, std::string& configured_name,
                       const std::optional<std::string>& config_file_path);


// /prompt 命令:裸敲显示当前法(人格段)的来源和字数,外加各提示词模块
// 的来源统计(用户文件/内置,0.21.x 运行时化);/prompt reset 带二次确认,
// 把 system_prompt.md 还原成内置默认(旧文件挪成 .bak)。
// persona 是本会话实际在用的人格段(空串 = 内置默认);law_source 是启动时
// 算好的来源说明(CLI 参数/文件/内置);prompts_dir 是用户模块目录
// (~/.lubancode/prompts,找不到主目录时空串)。
void HandlePromptCommand(const std::string& args, const std::string& law_source, const std::string& persona,
                          const std::string& prompts_dir);

// ---------------------------------------------------------------------------
// 命令分派注册制(会话终章):魂/法域的分派位。case 体原样自
// interactive_session 的大 switch 搬来,材料经 SlashDispatchContext 递入。
// ---------------------------------------------------------------------------
struct SlashDispatchContext;
CommandFlow HandleSlashSoul(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);
CommandFlow HandleSlashPrompt(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
