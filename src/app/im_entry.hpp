// `lubancode im` 命令族(§六 6.1):统一 IM 选择与启动入口。
//
// ImEntryController 只做三件事:选目标(显式参数 > 最近选择 > default_
// account > 唯一账号,多条弹列表)、缺配置进向导(channel setup 复用同
// 一套表单/凭据服务)、把选择折成 GatewayLaunchPlan 交给共用启动服务。
// 它不写 JSON/ACL(那是 ChannelConfigService/CredentialStore)、不创建
// 适配器(那是 ChannelGatewayWiring)、不新增 IM 执行器。
//
// 非交互(管道/服务/无 TTY):不弹界面不等键盘——目标明确且配置完整直接
// 启动(不更新交互偏好);不唯一/缺配置返回稳定非零码与 setup 提示。
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "app/gateway_launch.hpp"
#include "channel/channel_config.hpp"
#include "config/im_preferences.hpp"

namespace lubancode::app {

// §6.1 选择规则的裁决结果(纯函数,册在 tests/unit/app)。
struct ImCandidate {
    ChannelAccountRef ref;
    bool enabled = false;  // 渠道与账号两层 enabled 都开(激活五闸的子集)
};

struct ImTargetResolution {
    enum class Status {
        ReadyUnique,        // 唯一可启动目标(显式/最近/默认/唯一)
        NeedSelect,         // 多候选:开选择列表(candidates 带全量)
        NeedSetup,          // 无候选:进平台向导
        DisabledAccount,    // 目标在册但停用:问"是否启用"(不暗改)
        UnknownPlatform,    // 平台名拼错:报错,不写空壳配置
        PlatformNotImplemented,  // 平台认得但没有可运行适配器
        MissingAccount,     // 显式指定的账号没配过(交互进向导/非交互报错)
    };
    Status status = Status::NeedSetup;
    std::optional<ChannelAccountRef> target;       // ReadyUnique/DisabledAccount 时有效
    std::vector<ImCandidate> candidates;           // NeedSelect 时的全量(含停用)
    std::string detail;                            // 人话/诊断
};

struct ImTargetQuery {
    std::optional<std::string> explicit_channel;  // `lubancode im qqbot`
    std::optional<std::string> explicit_account;  // --account main
    bool force_select = false;                    // --select
    const std::map<std::string, channel::ChannelUserConfig>* channels = nullptr;
    const config::ImPreferences* preferences = nullptr;
};

// 规则册(§6.1):
//   - 显式平台/账号为准;未知平台/未实现平台报错,不写空壳配置;
//   - 只指定平台:该平台最近账号 > default_account > 唯一账号;多个弹选择;
//     全局最近平台不能覆盖显式平台;
//   - 未指定:--select 开列表;有效最近选择直接用;最近无效时唯一可用账号
//     自动选;多个开列表;一个都没有进向导;
//   - 目标在册但停用:DisabledAccount(交互问启用,非交互报错)。
ImTargetResolution ResolveImTarget(const ImTargetQuery& query);

// 目标在配置里"在册"(平台已配、账号存在;enabled 与否另说)。
bool ImTargetConfigured(const std::map<std::string, channel::ChannelUserConfig>& channels,
                        const ChannelAccountRef& target);

// CLI 入口参数(lubancode im [--select] [平台] [--account 账号] [--profile 档]
// / lubancode im setup [平台] [--account 账号])。
struct ImCommandArgs {
    bool setup = false;      // im setup:只进配置管理,不启动
    bool select = false;     // --select:强制开选择列表
    std::string platform;    // 位置参数;空 = 不指定
    std::string account;     // --account;空 = 不指定
    std::string profile;     // --profile;空 = default
};

// 返回进程退出码:0 启动并干净退出;1 用法/环境/向导失败;2 非交互下
// 目标不唯一或未配置(附 setup 提示);其余透传 gateway run 的退出码合同。
int RunImCommand(const ImCommandArgs& args);

}  // namespace lubancode::app
