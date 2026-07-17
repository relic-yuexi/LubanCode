// 魂法分家(0.16.x)的文件侧:~/.lubancode/ 下三样东西的生成、读取、还原。
//   system_prompt.md —— "法",内容就是内置默认人格段的原样副本(顶部带一行
//       注释说明),用户改它定制 lubancode 的行为;/prompt reset 一键还原。
//   SOUL.md          —— "魂",风格叠加层,默认只有一行注释、实际内容空白
//       (空白 = 无效果);写点风格指令就会注入在系统提示最后。
//   souls/           —— 备选魂的目录,附一个 wenyan.md 文言文示例;/soul 切换。
// 每次启动都过一遍 EnsurePromptScaffold:缺哪样补哪样,已存在的绝不覆盖。
//
// 注意分层:config 层不依赖 agent 层(依赖只许单向,cli -> agent -> api/tools,
// config 不该反过来牵扯 agent),所以内置默认人格段的原文由调用方传进来
// (default_persona 参数),这里只管文件怎么落地,不知道人格段写的是什么。

#pragma once

#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::config {

// 三样东西的路径(lubancode_dir 即 HomeLubancodeDir() 给的那个目录)。
std::string SystemPromptFilePath(const std::string& lubancode_dir);
std::string SoulFilePath(const std::string& lubancode_dir);
std::string SoulsDirPath(const std::string& lubancode_dir);
// souls/<名字>.md 的路径,名字就是 /soul 命令用的那个(文件名去扩展名)。
std::string SoulPathByName(const std::string& lubancode_dir, const std::string& name);

// 三份默认内容(生成、reset 共用同一份,不会出现两处写法不一致)。
std::string DefaultSystemPromptFileContent(const std::string& default_persona);
std::string DefaultSoulFileContent();
std::string DefaultWenyanSoulFileContent();

// 首启生成 + 每次启动查漏补缺:目录(含 souls/)不存在就建,三个文件
// 缺哪个补哪个,已存在的绝不覆盖(用户改过的内容一个字都不动)。
// 返回本次真正新建的文件路径列表(空 = 全都在,什么也没做)。写不进去
// (权限之类)不算错、不崩,跳过就是——法魂缺文件时运行期自有内置回退。
std::vector<std::string> EnsurePromptScaffold(const std::string& lubancode_dir, const std::string& default_persona);

// 读一个文本文件(UTF-8 原样读入)。不存在/打不开返回 std::nullopt——
// 不算错,缺失的语义(法回退内置、魂无效果)由调用方决定。
std::optional<std::string> ReadTextFileIfExists(const std::string& path);

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
