// `lubancode channel setup <platform> [--account <名>]`(QQBot Windows 修复
// 单 §5.1):交互式渠道配置向导。显示目标账号与实际配置路径,依次问
// AppID/AppSecret(AppSecret 走隐藏输入),新账号复用 QQ 模板,已有账号
// 只改选定字段、保留权限路由与未知 JSON 字段。保存后明示"配置已保存/
// 尚未验证连接"与 gateway run 入口。
//
// 非交互终端(stdin 不是 TTY)不开向导:明报退出,自动化走配置文件里的
// secret_file/secret_env。密钥只留在向导进程内:不进 argv(没有任何
// --secret 旗标)、不进会话、不进 trace、不进 shell 历史(隐藏输入)。
#pragma once

#include <string>

namespace lubancode::cli {

struct ChannelSetupCommandArgs {
    std::string platform;  // qqbot
    std::string account;   // 空 = main
};

// 返回进程退出码:0 保存成功(或差异确认后取消);1 用法/环境/保存失败;
// 2 非交互终端拒开向导。
int RunChannelSetupCommand(const ChannelSetupCommandArgs& args);

}  // namespace lubancode::cli
