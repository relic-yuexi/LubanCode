// 命令放行裁定(settings.local.json 的 permissions 前缀名单):一条 shell
// 命令过这里的黑/白名单——deny 前缀命中即 Deny,压过 allow、压过会话
// "总是允许";allow 前缀命中等价 command_safety 判 Safe(补充白名单);
// 都没命中给 None,交回别处(command_safety / 逐个问)决定。
//
// 骨架拆解反弹·问题 7 自 config.cpp/config.hpp 拆出独立小档:这是"这条
// 命令该不该自动放行"的安全决策,不是配置读写,不该长在 config 主体里。
// 没挪去 tools/command_safety 的理由:那边按命令内容做静态分析(只读/
// 写盘/联网/起进程,工具域的裁定),这边按用户手写的前缀名单叠加(配置
// 域的数据 + 裁定),输入来源与语义都不同,并成一份会让 command_safety
// 认识 settings.local。调用方(runtime/turn_runtime、tools/agent_tool、
// 单测)按需 include 本头,不再借 config.hpp 顺带捎来。
//
// 纯函数,零 IO。单测在 tests/unit/config/test_config.cpp。

#pragma once

#include <string>
#include <vector>

namespace lubancode::config {

// 一条命令过 settings.local 的 permissions 前缀判定后的裁定。deny 压过 allow
// (两边都命中时算 Deny)。None = 两个名单都没命中,交给别处(command_safety /
// 逐个问)决定。
enum class CommandPermission { None, Allow, Deny };

// 纯函数:命令去掉前导空白后,先看 deny_commands 有没有前缀命中(命中即
// Deny,压过一切),再看 allow_commands(命中即 Allow),都没命中 None。
// 空前缀跳过;原始命令串直接比,不做 shell 解析(前缀白/黑名单本就是"这几条
// 命令开头"的朴素约定,跟 command_safety 的保守解析各管一摊)。auto 分流处
// 叠加用:Deny → 永远问(mode 门槛在调用方),Allow → auto 档等价
// command_safety 的 Safe。
CommandPermission ClassifyCommandByPermissions(const std::string& command,
                                               const std::vector<std::string>& allow_commands,
                                               const std::vector<std::string>& deny_commands);

}  // namespace lubancode::config
