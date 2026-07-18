// 魂法分家(0.16.x)的文件侧:~/.lubancode/ 下几样东西的生成、读取、还原。
//   system_prompt.md —— "法",内容就是内置默认人格段的原样副本(顶部带一行
//       注释说明),用户改它定制 lubancode 的行为;/prompt reset 一键还原。
//   SOUL.md          —— "魂",风格叠加层,默认只有一行注释、实际内容空白
//       (空白 = 无效果);写点风格指令就会注入在系统提示最后。
//   souls/           —— 备选魂的目录,附一个 wenyan.md 文言文示例;/soul 切换。
//   skills/lubancode-config/SKILL.md —— 内置配置手册,供模型按需加载。
//       0.21.x 起是"系统维护件":文件带管理标记(kManagedSkillMarker)且
//       内容不等于当前内置版时,启动时覆盖刷新(随版本自动更新);用户
//       删了标记(自建/深改过)就一个字不动。
//   prompts/{core,features,platforms}/*.md —— 0.21.x 提示词运行时模块,
//       内容播种自编译期嵌入版;用户改了,开新会话即生效(拼装逻辑在
//       agent/prompt_assembler,这里只管播种落地)。
// 每次启动都过一遍 EnsurePromptScaffold:缺哪样补哪样,已存在的绝不覆盖
// (唯一例外是上面说的带标记 SKILL.md 刷新)。
//
// 注意分层:config 层不依赖 agent 层(依赖只许单向,cli -> agent -> api/tools,
// config 不该反过来牵扯 agent),所以内置默认人格段的原文由调用方传进来
// (default_persona 参数),这里只管文件怎么落地,不知道人格段写的是什么。

#pragma once

#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lubancode::config {

// 三样东西的路径(lubancode_dir 即 HomeLubancodeDir() 给的那个目录)。
std::string SystemPromptFilePath(const std::string& lubancode_dir);
std::string SoulFilePath(const std::string& lubancode_dir);
std::string SoulsDirPath(const std::string& lubancode_dir);
// souls/<名字>.md 的路径,名字就是 /soul 命令用的那个(文件名去扩展名)。
std::string SoulPathByName(const std::string& lubancode_dir, const std::string& name);

// 默认内容(生成、reset 共用同一份,不会出现两处写法不一致)。
std::string DefaultSystemPromptFileContent(const std::string& default_persona);
std::string DefaultSoulFileContent();
std::string DefaultWenyanSoulFileContent();
// 内置配置手册(lubancode-config 技能)的当前版本正文,含管理标记。
std::string DefaultLubancodeConfigSkillContent();

// SKILL.md 的"系统维护件"标记:含此标记 = 归 lubancode 管,随版本自动
// 刷新;用户自建或删了标记的文件绝不动。
inline constexpr const char kManagedSkillMarker[] =
    "<!-- lubancode 系统维护,随版本自动更新;自定义请另建技能 -->";

// 首启生成 + 每次启动查漏补缺:目录(含 souls/、skills/、prompts/)不存在
// 就建,文件缺哪个补哪个,已存在的绝不覆盖(用户改过的内容一个字都不动;
// 唯一例外:lubancode-config/SKILL.md 带 kManagedSkillMarker 且内容不等于
// 当前内置版时覆盖刷新)。prompt_modules 是提示词运行时模块的播种清单
// {相对路径(如 core/10-identity.md), 正文},由调用方从
// agent::PromptModuleSeeds() 取来递进——config 层不认识 agent 层。
// 返回本次真正新建的文件路径列表(空 = 全都在,什么也没做;刷新不算新建)。
// 写不进去(权限之类)不算错、不崩,跳过就是——运行期自有内置回退。
std::vector<std::string> EnsurePromptScaffold(
    const std::string& lubancode_dir, const std::string& default_persona,
    const std::vector<std::pair<std::string, std::string>>& prompt_modules = {});

// 读一个文本文件(UTF-8 原样读入)。不存在/打不开返回 std::nullopt——
// 不算错,缺失的语义(法回退内置、魂无效果)由调用方决定。
std::optional<std::string> ReadTextFileIfExists(const std::string& path);

// SOUL.md 的专用读写口。ReadSoulFile 只收有效 UTF-8；文件缺失、打不开、
// 是目录或内容已坏都回 nullopt——启动方把它当"无魂"继续跑,不让一份坏
// 配置卡死整个会话。WriteSoulFile 会按需建 ~/.lubancode/，写入失败给可读
// 的错；ClearSoulFile 重写内置默认内容(只有说明注释,实际等于无魂)。
std::optional<std::string> ReadSoulFile(const std::string& lubancode_dir);
std::expected<void, std::string> WriteSoulFile(const std::string& lubancode_dir, const std::string& content);
std::expected<void, std::string> ClearSoulFile(const std::string& lubancode_dir);

// /prompt reset、--reset-system-prompt 共用:旧 system_prompt.md 先挪成
// system_prompt.md.bak(已有 .bak 就覆盖),再把 system_prompt.md 重写成
// 内置默认。成功返回 .bak 的路径(原文件本来就不存在时没有 .bak 可留,
// 返回空串);建目录/挪文件/写文件失败都报可读的错。
std::expected<std::string, std::string> ResetSystemPromptFile(const std::string& lubancode_dir,
                                                                const std::string& default_persona);

// 扫 souls/*.md,返回名字列表(文件名去 .md 扩展名),字典序。目录不存在
// 或没有 .md 文件就是空列表,不算错。
std::vector<std::string> ListSouls(const std::string& lubancode_dir);

}  // namespace lubancode::config
