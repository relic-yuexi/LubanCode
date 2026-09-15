// gateway_launch.hpp 的实现:cli_app.cpp 的 RunGateway 装配段原样搬来,
// 参数化成 GatewayLaunchPlan。打印文案一字不改(只挪结构)。
#include "app/gateway_launch.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>

#include "app/channel_gateway_wiring.hpp"
#include "app/tool_runtime.hpp"
#include "app/backend_stack.hpp"
#include "app/version.hpp"
#include "channel/manager.hpp"  // DefaultChannelsStateRoot
#include "cli/gateway_command.hpp"
#include "config/config.hpp"
#include "gateway/profile.hpp"
#include "gateway/reply_outbox.hpp"
#include "runtime/automation_pump.hpp"
#include "runtime/channel_automation.hpp"
#include "runtime/channel_work_pump.hpp"
#include "tools/path_utils.hpp"
#include "platform/paths.hpp"
#include "workspace/identity.hpp"

namespace lubancode::app {

config::Config FilterConfigForChannelAccount(const config::Config& config,
                                             const std::optional<ChannelAccountRef>& filter) {
    config::Config filtered = config;
    if (!filter.has_value()) {
        return filtered;
    }
    std::map<std::string, channel::ChannelUserConfig> channels;
    const auto channel_it = config.channels.find(filter->channel_id);
    if (channel_it != config.channels.end()) {
        channel::ChannelUserConfig channel = channel_it->second;
        std::map<std::string, channel::ChannelAccountUserConfig> accounts;
        const auto account_it = channel.accounts.find(filter->account_id);
        if (account_it != channel.accounts.end()) {
            accounts.emplace(filter->account_id, account_it->second);
        }
        channel.accounts = std::move(accounts);
        channels.emplace(filter->channel_id, std::move(channel));
    }
    filtered.channels = std::move(channels);
    return filtered;
}

int RunGatewayWithPlan(const GatewayLaunchPlan& plan) {
    cli::GatewayCommandArgs gateway_args;
    gateway_args.verb = "run";
    gateway_args.profile = plan.profile;

    // V1 主泵装配(app 层:backend/registry 是 app 的材料,engine
    // 不反向依赖)。模型/工具面按当前配置;工具授权 fail closed
    //(名单空 = needs_confirm 工具全拒,基础表里免确认的工具照走)。
    // P1 收尾(应用Worker接入单 §4.2):本段的路径来源统一走状态根
    // StateRootDir(应用根语义=数据根;个人布局=~/.lubancode 原样,
    // 行为逐字节不变),不再拿 HomeLubancodeDir 拼运行状态。
    const auto state_root = config::StateRootDir();
    if (!state_root.has_value()) {
        std::cerr << "gateway run: 状态根不可用(应用根变量坏或找不到主目录),"
                     "无法定位运行数据根\n";
        return 1;
    }
    const auto gateway_config = config::LoadFromEnv();
    if (!gateway_config.has_value()) {
        std::cerr << "gateway run: 配置装载失败——" << gateway_config.error() << "\n";
        return 1;
    }
    auto backend = app::BuildBackend(gateway_config->config);
    tools::ToolRegistry registry = app::BuildBaseToolRegistry({}, gateway_config->config.search);
    // workspace 身份:启动时冻结一次(Q2 §六第二项——渠道路的会话
    // 映射按它隔离,重启换 cwd 不误续别的项目上下文)。身份裁决的
    // home 止步=workspaces 树宿主根=状态根(与 app-server 同一口径,
    // server.cpp SessionCreateLedgerPath)。
    const std::filesystem::path gateway_cwd = std::filesystem::current_path();
    const auto workspace_identity =
        workspace::ResolveWorkspaceIdentity(gateway_cwd, tools::Utf8ToPath(*state_root));
    if (!workspace_identity.has_value()) {
        std::cerr << "gateway run: workspace 身份裁决失败——"
                  << workspace_identity.error() << "\n";
        return 1;
    }
    const std::filesystem::path workspaces_root =
        tools::Utf8ToPath(*state_root) / "workspaces";

    // 渠道过滤器:只活在本次进程的装配输入里(不改全局配置、不改别人
    // 的 enabled)。自动化泵吃全量配置——渠道过滤不缩自动任务(§6.0)。
    const config::Config wiring_config =
        FilterConfigForChannelAccount(gateway_config->config, plan.channel_account_filter);

    std::optional<runtime::GatewayAutomationPump> pump;
    gateway::DurableReplyOutbox standalone_outbox;
    bool automation_pump_open = false;
    {
        runtime::GatewayAutomationPump::Options pump_options;
        // gateway 状态根走唯一口(状态根/gateway;profile 树含
        // gateway.json 整树随状态根,见 gateway/profile.hpp 合同注释)。
        const std::filesystem::path gateway_root = gateway::DefaultGatewayRoot();
        const std::string profile_name =
            gateway_args.profile.empty()
                ? std::string(gateway::kDefaultGatewayProfile)
                : gateway_args.profile;
        const gateway::GatewayProfilePaths profile_paths =
            gateway::ResolveGatewayProfilePaths(gateway_root, profile_name);
        pump_options.paths = profile_paths;
        pump_options.workspaces_root = workspaces_root;
        pump_options.workspace_identity = *workspace_identity;
        pump_options.cwd_utf8 = platform::CurrentDirUtf8();
        pump_options.lubancode_version = std::string(app::kVersion);
        pump_options.wire_name = config::ProviderWireName(gateway_config->config.wire);
        pump_options.model = gateway_config->config.model;
        pump_options.max_steps_per_turn = 32;   // V1 生产缺省:预算三根
        pump_options.max_wall_secs = 600;       // 硬线至少步数+墙钟两根
        pump.emplace();
        const auto open = runtime::GatewayAutomationPump::Open(&*pump, *backend, registry,
                                                               std::move(pump_options));
        if (!open.ok) {
            // §6.0 结构修复:automation 泵不是渠道泵的前提。硬失败档
            // (gateway run)维持原语义;软档(outbox 独立开)继续起渠道。
            if (plan.require_automation_pump) {
                std::cerr << "gateway run: 业务泵开不了——" << open.error << "\n";
                return 1;
            }
            pump.reset();
            gateway::DurableReplyOutbox::Paths outbox_paths;
            outbox_paths.log_file = profile_paths.outbox_log;
            outbox_paths.replies_dir = profile_paths.replies_dir;
            outbox_paths.published_dir = profile_paths.published_dir;
            const auto outbox_open =
                gateway::DurableReplyOutbox::Open(&standalone_outbox, outbox_paths);
            if (!outbox_open.ok) {
                std::cerr << "gateway run: 自动任务泵与 outbox 都开不了——"
                          << outbox_open.error << "\n";
                return 1;
            }
        } else {
            automation_pump_open = true;
        }
        // ownerEpoch 不在这里预造:GatewayProcess 取到锁后把锁内
        // epoch(= boot_id)递进泵(process.cpp 的 set_owner_epoch)。
    }
    // QQ 机器人接入单 Q1:渠道装配(QQ 进程内直连,§十五定案)。
    // channels 段为空时零渠道行为,不挂副泵;装配失败的账号记
    // skipped 打给 stderr,Gateway 照常起来(渠道失败不拦主业务)。
    std::unique_ptr<app::ChannelGatewayWiring> channel_wiring;
    std::unique_ptr<app::CompositeGatewayPump> composite_pump;
    // Q5 聊天侧任务桥:渠道会话的模型经 create/list/cancel_reminder 落
    // automation 域命令(借用 automation 泵的同一本账,单写者)。store 缺席
    // (软档)时工具 fail closed。桥须活过 RunGatewayCommand(工具持它)。
    auto channel_automation = std::make_shared<runtime::ChannelAutomationBridge>(
        automation_pump_open ? pump->store() : nullptr);
    runtime::RegisterChannelAutomationTools(registry, channel_automation);
    {
        app::ChannelGatewayWiring::Options wiring_options;
        wiring_options.config = &wiring_config;
        // 渠道账号状态根走唯一口(状态根/channels;ingress 账/
        // account-status/sessions 映射/work 账/锁/qq spool 全在这棵
        // 树下,见 channel/manager.hpp 合同注释)。
        const std::filesystem::path wiring_channels_root =
            channel::DefaultChannelsStateRoot();
        wiring_options.channels_state_root = wiring_channels_root;
        // Q1b 配对控制面:profile 树的 control/(与 stop.json 同款通道),
        // 另一终端的 channel pairing approve/reject 命令从这里进来。
        wiring_options.gateway_control_dir =
            gateway::ResolveGatewayProfilePaths(gateway::DefaultGatewayRoot(),
                                                gateway_args.profile.empty()
                                                    ? std::string(
                                                          gateway::kDefaultGatewayProfile)
                                                    : gateway_args.profile)
                .control_dir;
        const std::filesystem::path channels_root = wiring_channels_root;
        channel_wiring = app::ChannelGatewayWiring::Create(std::move(wiring_options));
        if (channel_wiring != nullptr) {
            for (const std::string& line : channel_wiring->skipped()) {
                std::cerr << "[gateway] 渠道账号未装配: " << line << "\n";
            }
            // 连接状态单 §四(rebase 自 cli_app 内联装配段,#81):装配时检查
            // 信任根并明报来源(解析不到也说清楚,现场能定位第一处失败;
            // 具体连接状态由 wiring 的 reporter 每 tick 打印)。
            for (const std::string& line : channel_wiring->diagnostics()) {
                std::cerr << "[gateway] " << line << "\n";
            }
            // QQ 接入单 Q2:渠道 work 泵(V3 与 outbox 总装)挂进
            // wiring——桥泵之后每 tick 推进一轮;outbox 共享
            // automation 泵的同一本账(单写者,两泵同 tick 串行)。
            // automation 泵缺席时(§6.0 软档)outbox 用独立件。
            if (channel_wiring->manager() != nullptr &&
                channel_wiring->manager()->account_count() > 0) {
                runtime::ChannelWorkPump::Options work_options;
                work_options.manager = channel_wiring->mutable_manager();
                work_options.outbox = automation_pump_open ? pump->outbox() : &standalone_outbox;
                work_options.channels_state_root = channels_root;
                work_options.workspaces_root = workspaces_root;
                work_options.workspace_identity = *workspace_identity;
                work_options.cwd_utf8 = platform::CurrentDirUtf8();
                work_options.lubancode_version = std::string(app::kVersion);
                work_options.wire_name =
                    config::ProviderWireName(gateway_config->config.wire);
                work_options.model = gateway_config->config.model;
                work_options.max_steps_per_turn = 32;  // 与 automation 同款预算
                work_options.max_wall_secs = 600;
                // Q5:automation 账与任务桥递进(渠道任务认领/执行/补投 +
                // 聊天侧工具的渠道上下文)。
                work_options.automation_store =
                    automation_pump_open ? pump->store() : nullptr;
                work_options.automation_bridge = channel_automation;
                auto work_pump = std::make_unique<runtime::ChannelWorkPump>();
                const auto open = runtime::ChannelWorkPump::Open(work_pump.get(), *backend,
                                                                 registry, std::move(work_options));
                if (!open.ok) {
                    std::cerr << "[gateway] 渠道 work 泵开不了——" << open.error
                              << "\n";
                    // 渠道业务面不开:桥照跑(收信入账),Gateway 照常起。
                } else {
                    channel_wiring->set_work_pump(std::move(work_pump));
                }
            }
            composite_pump = std::make_unique<app::CompositeGatewayPump>(
                automation_pump_open ? &*pump : nullptr, std::move(channel_wiring));
            gateway_args.pump = composite_pump.get();
        } else {
            gateway_args.pump = automation_pump_open ? &*pump : nullptr;
        }
    }
    return cli::RunGatewayCommand(gateway_args);
}

}  // namespace lubancode::app
