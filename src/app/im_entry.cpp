// im_entry.hpp 的实现。选择界面走平台层原语(RawInputScope + KeyReader,
// VT 批重画;不支持 VT 的终端退数字选),不碰 console_input 的锁与面板
// 体系——这里是向导级一次性菜单,不是常驻 composer。
#include "app/im_entry.hpp"

#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "app/gateway_launch.hpp"
#include "app/version.hpp"
#include "channel/channel_setup.hpp"
#include "cli/channel_setup_command.hpp"
#include "config/config.hpp"
#include "platform/console.hpp"
#include "platform/hidden_input.hpp"
#include "platform/paths.hpp"

namespace lubancode::app {

namespace {

bool AccountUsable(const channel::ChannelUserConfig& channel,
                   const channel::ChannelAccountUserConfig& account) {
    return channel.enabled && account.enabled;
}

}  // namespace

ImTargetResolution ResolveImTarget(const ImTargetQuery& query) {
    ImTargetResolution resolution;
    const std::map<std::string, channel::ChannelUserConfig>& channels =
        query.channels != nullptr ? *query.channels
                                  : std::map<std::string, channel::ChannelUserConfig>{};

    const auto make_target_result = [&](const std::string& channel_id,
                                         const std::string& account_id) {
        const auto& channel = channels.at(channel_id);
        const auto& account = channel.accounts.at(account_id);
        resolution.target = ChannelAccountRef{channel_id, account_id};
        resolution.status = AccountUsable(channel, account)
                                ? ImTargetResolution::Status::ReadyUnique
                                : ImTargetResolution::Status::DisabledAccount;
        if (resolution.status == ImTargetResolution::Status::DisabledAccount) {
            resolution.detail = "账号 " + channel_id + "/" + account_id + " 已配置但停用";
        }
    };

    if (query.explicit_channel.has_value()) {
        // 显式平台:先对注册表,再对配置;拼错报错,不写空壳配置。
        const std::string& platform_id = *query.explicit_channel;
        const auto platform = channel::FindChannelSetupPlatform(platform_id);
        if (!platform.has_value()) {
            resolution.status = ImTargetResolution::Status::UnknownPlatform;
            resolution.detail = "未知平台: " + platform_id + "(认得: qqbot;feishu 尚未支持)";
            return resolution;
        }
        if (!platform->implemented) {
            resolution.status = ImTargetResolution::Status::PlatformNotImplemented;
            resolution.detail = platform->display_name + "(" + platform_id + ")尚未支持,不能选择";
            return resolution;
        }
        const auto channel_it = channels.find(platform_id);
        if (channel_it == channels.end() || channel_it->second.accounts.empty()) {
            resolution.status = ImTargetResolution::Status::MissingAccount;
            resolution.detail = "平台 " + platform_id + " 还没有已配置的账号";
            return resolution;
        }
        const channel::ChannelUserConfig& channel = channel_it->second;
        if (query.explicit_account.has_value()) {
            const auto account_it = channel.accounts.find(*query.explicit_account);
            if (account_it == channel.accounts.end()) {
                resolution.status = ImTargetResolution::Status::MissingAccount;
                resolution.detail = "账号 " + platform_id + "/" + *query.explicit_account +
                                    " 没配过;不能悄悄连上别的账号";
                return resolution;
            }
            make_target_result(platform_id, *query.explicit_account);
            return resolution;
        }
        // 只指定平台:平台最近账号 > default_account > 唯一账号;多个弹选择。
        if (query.preferences != nullptr) {
            const auto recent_it = query.preferences->recent_account.find(platform_id);
            if (recent_it != query.preferences->recent_account.end() &&
                channel.accounts.count(recent_it->second) != 0) {
                make_target_result(platform_id, recent_it->second);
                return resolution;
            }
        }
        if (!channel.default_account.empty() &&
            channel.accounts.count(channel.default_account) != 0) {
            make_target_result(platform_id, channel.default_account);
            return resolution;
        }
        if (channel.accounts.size() == 1) {
            make_target_result(platform_id, channel.accounts.begin()->first);
            return resolution;
        }
        resolution.status = ImTargetResolution::Status::NeedSelect;
        for (const auto& [account_id, account] : channel.accounts) {
            resolution.candidates.push_back(
                ImCandidate{ChannelAccountRef{platform_id, account_id},
                            AccountUsable(channel, account)});
        }
        resolution.detail = "平台 " + platform_id + " 有多个账号,请选择";
        return resolution;
    }

    // 未指定平台。
    std::vector<ImCandidate> all_candidates;
    for (const auto& [channel_id, channel] : channels) {
        for (const auto& [account_id, account] : channel.accounts) {
            all_candidates.push_back(
                ImCandidate{ChannelAccountRef{channel_id, account_id},
                            AccountUsable(channel, account)});
        }
    }
    if (!query.force_select && query.preferences != nullptr &&
        query.preferences->last.has_value()) {
        const config::ImRecentSelection& last = *query.preferences->last;
        const auto channel_it = channels.find(last.channel_id);
        if (channel_it != channels.end() &&
            channel_it->second.accounts.count(last.account_id) != 0) {
            make_target_result(last.channel_id, last.account_id);
            return resolution;  // 悬空(已删账号)则回退到下面的选择流程
        }
    }
    if (all_candidates.empty()) {
        resolution.status = ImTargetResolution::Status::NeedSetup;
        resolution.detail = "没有任何已配置的渠道账号";
        return resolution;
    }
    if (!query.force_select) {
        std::size_t usable = 0;
        std::size_t usable_index = 0;
        for (std::size_t i = 0; i < all_candidates.size(); ++i) {
            if (all_candidates[i].enabled) {
                ++usable;
                usable_index = i;
            }
        }
        if (usable == 1) {
            resolution.target = all_candidates[usable_index].ref;
            resolution.status = ImTargetResolution::Status::ReadyUnique;
            return resolution;
        }
        if (usable == 0 && all_candidates.size() == 1) {
            resolution.target = all_candidates[0].ref;
            resolution.status = ImTargetResolution::Status::DisabledAccount;
            resolution.detail = "唯一账号已配置但停用";
            return resolution;
        }
    }
    resolution.status = ImTargetResolution::Status::NeedSelect;
    resolution.candidates = std::move(all_candidates);
    resolution.detail = "有多个候选,请选择";
    return resolution;
}

bool ImTargetConfigured(const std::map<std::string, channel::ChannelUserConfig>& channels,
                        const ChannelAccountRef& target) {
    const auto channel_it = channels.find(target.channel_id);
    return channel_it != channels.end() &&
           channel_it->second.accounts.count(target.account_id) != 0;
}

// ---------------------------------------------------------------------------
// 终端选择菜单(首版:方向键 + 数字回退;Esc 取消)
// ---------------------------------------------------------------------------

namespace {

struct SelectMenuEntry {
    std::string label;        // "QQ · main"
    std::string annotation;   // "上次使用,已配置" / "已停用" / "尚未支持"
    bool selectable = true;   // 尚未支持的平台不可选
};

// 返回选中下标;nullopt = 取消。仅交互终端调用。
std::optional<std::size_t> RunSelectMenu(const std::string& title,
                                         const std::vector<SelectMenuEntry>& entries,
                                         std::size_t initial_cursor) {
    const std::size_t count = entries.size();
    if (count == 0) {
        return std::nullopt;
    }
    std::size_t cursor = initial_cursor < count ? initial_cursor : 0;
    const std::size_t total_lines = count + 2;  // 标题 + 条目 + 提示行

    auto render = [&]() {
        std::printf("%s\n", title.c_str());
        for (std::size_t i = 0; i < count; ++i) {
            const char marker = i == cursor ? '>' : ' ';
            std::printf("%c %lu. %s", marker, static_cast<unsigned long>(i + 1),
                        entries[i].label.c_str());
            if (!entries[i].annotation.empty()) {
                std::printf("    — %s", entries[i].annotation.c_str());
            }
            std::printf("\n");
        }
        std::printf("(上下键选择,回车确认,数字直达,Esc 取消)\n");
        std::fflush(stdout);
    };

    // VT 档:raw 模式 + 方向键;进不去(无 VT/无 raw)退数字回退。
    const auto probe = platform::ProbeStdoutConsole();
    if (probe.vt_enabled) {
        platform::RawInputScope raw;
        if (raw.ok()) {
            platform::KeyReader reader;
            render();
            while (true) {
                const auto key = reader.ReadOne();
                if (!key.has_value()) {
                    return std::nullopt;  // EOF
                }
                bool moved = false;
                switch (key->kind) {
                    case platform::KeyInput::Kind::Up:
                    case platform::KeyInput::Kind::ShiftTab:
                        cursor = cursor == 0 ? count - 1 : cursor - 1;
                        moved = true;
                        break;
                    case platform::KeyInput::Kind::Down:
                    case platform::KeyInput::Kind::Tab:
                        cursor = (cursor + 1) % count;
                        moved = true;
                        break;
                    case platform::KeyInput::Kind::Enter:
                    case platform::KeyInput::Kind::NewLine:
                        if (entries[cursor].selectable) {
                            return cursor;
                        }
                        break;
                    case platform::KeyInput::Kind::Char: {
                        if (key->ch >= U'1' && key->ch <= U'9') {
                            const std::size_t index =
                                static_cast<std::size_t>(key->ch - U'1');
                            if (index < count && entries[index].selectable) {
                                return index;
                            }
                        }
                        break;
                    }
                    case platform::KeyInput::Kind::Esc:
                    case platform::KeyInput::Kind::CtrlC:
                    case platform::KeyInput::Kind::CtrlD:
                        return std::nullopt;
                    default:
                        break;
                }
                if (moved) {
                    // 重画:光标退回块首再整块打印(VT 才这么搬光标)。
                    std::printf("\x1b[%luA\r", static_cast<unsigned long>(total_lines));
                    render();
                }
            }
        }
    }
    // 数字回退档(cooked):整块列表 + 读一行数字。
    render();
    std::string line;
    if (!std::getline(std::cin, line)) {
        return std::nullopt;
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    if (line.size() != 1 || line[0] < '1' || line[0] > '9') {
        return std::nullopt;  // 空行/Esc 语义之外的输入按取消;输错数字重问一次
    }
    const std::size_t index = static_cast<std::size_t>(line[0] - '1');
    if (index >= count || !entries[index].selectable) {
        return std::nullopt;
    }
    return index;
}

bool AskYesNoCooked(const std::string& prompt, bool default_yes) {
    std::printf("%s (%s):", prompt.c_str(), default_yes ? "Y/n" : "y/N");
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) {
        return default_yes;
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    if (line.empty()) {
        return default_yes;
    }
    return line == "y" || line == "Y" || line == "yes" || line == "是";
}

// 启用停用账号:走 ChannelConfigService(定点更新,与向导同一份合同)。
// 失败返回 false(detail 给人话)。
bool EnableChannelAccount(const ChannelAccountRef& target, std::string* error_out) {
    auto options = channel::ChannelConfigService::DefaultOptions();
    if (!options.has_value()) {
        *error_out = options.error().detail;
        return false;
    }
    channel::ChannelSetupCommitRequest request;
    request.channel_id = target.channel_id;
    request.account_id = target.account_id;
    request.ensure_enabled = true;
    const auto committed = channel::ChannelConfigService::Commit(options.value(), request);
    if (!committed.has_value()) {
        *error_out = committed.error().reason + ": " + committed.error().detail;
        return false;
    }
    return true;
}

void RememberSelection(const config::ImPreferenceStore& store, config::ImPreferences prefs,
                       const ChannelAccountRef& target) {
    prefs.last = config::ImRecentSelection{target.channel_id, target.account_id};
    prefs.recent_account[target.channel_id] = target.account_id;
    if (const auto saved = store.Save(prefs); !saved.has_value()) {
        std::fprintf(stderr, "[im] 保存最近选择偏好失败(不影响本次启动): %s\n",
                     saved.error().c_str());
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// RunImCommand
// ---------------------------------------------------------------------------

int RunImCommand(const ImCommandArgs& args) {
    const bool interactive = platform::StdinIsTerminal();

    // 平台名若给了,先过注册表(拼错立即报错,不写空壳配置)。
    if (!args.platform.empty()) {
        const auto platform = channel::FindChannelSetupPlatform(args.platform);
        if (!platform.has_value()) {
            std::fprintf(stderr, "im: 未知平台 \"%s\"。认得: qqbot(feishu 尚未支持)\n",
                         args.platform.c_str());
            return 1;
        }
        if (!platform->implemented && !args.setup) {
            std::fprintf(stderr, "im: %s(%s) 尚未支持,不能作为聊天平台启动\n",
                         platform->display_name.c_str(), platform->id.c_str());
            return 1;
        }
    }

    const auto config_result = config::LoadFromEnv();
    if (!config_result.has_value()) {
        std::fprintf(stderr, "im: 配置装载失败——%s\n", config_result.error().c_str());
        return 1;
    }
    std::optional<config::ImPreferenceStore> prefs_store;
    config::ImPreferences prefs;
    if (const auto prefs_file = config::ImPreferenceStore::DefaultFilePath();
        prefs_file.has_value()) {
        prefs_store.emplace(*prefs_file);
        prefs = prefs_store->Load();  // 坏档/无档 = 空偏好,回选择流程
    }

    // 目标裁决(§6.1 规则册)。
    ImTargetQuery query;
    query.channels = &config_result->config.channels;
    query.preferences = prefs_store.has_value() ? &prefs : nullptr;
    if (!args.platform.empty()) {
        query.explicit_channel = args.platform;
    }
    if (!args.account.empty()) {
        query.explicit_account = args.account;
    }
    query.force_select = args.select && !args.setup;
    const ImTargetResolution resolution = ResolveImTarget(query);

    // im setup:只进配置管理,不顺带启动。
    if (args.setup) {
        if (!interactive) {
            std::fprintf(stderr,
                         "im setup: 当前不是交互终端,无法运行配置向导。\n"
                         "  自动化配置请编辑全局 config.json 的 channels 段"
                         "(AppSecret 用 secret_file/secret_env)。\n");
            return 2;
        }
        std::string platform = args.platform;
        if (platform.empty()) {
            // 平台选择列表(含"尚未支持"的如实标注)。
            std::vector<SelectMenuEntry> entries;
            for (const channel::ChannelSetupPlatform& item : channel::ChannelSetupPlatforms()) {
                entries.push_back(SelectMenuEntry{item.display_name,
                                                  item.implemented ? "" : "尚未支持",
                                                  item.implemented});
            }
            const auto picked = RunSelectMenu("选择聊天平台", entries, 0);
            if (!picked.has_value()) {
                std::printf("已取消。\n");
                return 0;
            }
            platform = channel::ChannelSetupPlatforms()[*picked].id;
        }
        cli::ChannelSetupCommandArgs setup_args;
        setup_args.platform = platform;
        setup_args.account = args.account;
        return cli::RunChannelSetupCommand(setup_args);
    }

    auto launch = [&](const ChannelAccountRef& target) {
        GatewayLaunchPlan plan;
        plan.profile = args.profile;
        plan.channel_account_filter = target;  // 只成启动过滤器,不改 enabled
        plan.require_automation_pump = true;   // IM 不以聊天入口为由关自动任务
        std::printf("[im] 启动 Gateway:profile=%s 工作目录=%s\n",
                    (plan.profile.empty() ? "default" : plan.profile.c_str()),
                    platform::CurrentDirUtf8().c_str());
        std::printf("[im] 渠道范围: %s/%s(其他账号本次不装配,配置不动)\n",
                    target.channel_id.c_str(), target.account_id.c_str());
        std::printf("[im] 自动任务: 当前 profile 的既有自动任务照常执行\n");
        std::printf("[im] 切换平台/账号: lubancode im --select\n");
        // 偏好在配置校验过、启动前记(§6.2);非交互不更新交互偏好(§6.4)。
        if (interactive && prefs_store.has_value()) {
            RememberSelection(*prefs_store, prefs, target);
        }
        return RunGatewayWithPlan(plan);
    };

    switch (resolution.status) {
        case ImTargetResolution::Status::ReadyUnique: {
            if (!resolution.target.has_value()) {
                std::fprintf(stderr, "im: 内部错误——ReadyUnique 没带目标\n");
                return 1;
            }
            return launch(*resolution.target);
        }
        case ImTargetResolution::Status::DisabledAccount: {
            if (!resolution.target.has_value()) {
                std::fprintf(stderr, "im: 内部错误——DisabledAccount 没带目标\n");
                return 1;
            }
            if (!interactive) {
                std::fprintf(stderr,
                             "im: %s(%s 已停用)。非交互模式不暗改开关;先运行 "
                             "`lubancode im setup %s --account %s` 启用。\n",
                             resolution.detail.c_str(), resolution.target->account_id.c_str(),
                             resolution.target->channel_id.c_str(),
                             resolution.target->account_id.c_str());
                return 2;
            }
            std::printf("%s\n", resolution.detail.c_str());
            if (!AskYesNoCooked("是否启用该渠道与账号并启动", true)) {
                std::printf("已取消。配置未改动。\n");
                return 0;
            }
            std::string enable_error;
            if (!EnableChannelAccount(*resolution.target, &enable_error)) {
                std::fprintf(stderr, "im: 启用失败(%s)。可运行 `lubancode im setup %s "
                                     "--account %s` 走完整向导。\n",
                             enable_error.c_str(), resolution.target->channel_id.c_str(),
                             resolution.target->account_id.c_str());
                return 1;
            }
            std::printf("已启用。开始启动。\n");
            return launch(*resolution.target);
        }
        case ImTargetResolution::Status::NeedSelect: {
            if (!interactive) {
                std::fprintf(stderr,
                             "im: 目标不唯一,非交互模式不弹界面。请显式指定: "
                             "lubancode im <平台> --account <账号>(候选 %zu 个)。\n",
                             resolution.candidates.size());
                return 2;
            }
            std::vector<SelectMenuEntry> entries;
            std::size_t initial_cursor = 0;
            const std::optional<config::ImRecentSelection> recent =
                prefs.last.has_value() ? prefs.last : std::nullopt;
            for (std::size_t i = 0; i < resolution.candidates.size(); ++i) {
                const ImCandidate& candidate = resolution.candidates[i];
                std::string annotation =
                    candidate.enabled ? "已配置" : "已配置,已停用";
                if (recent.has_value() && recent->channel_id == candidate.ref.channel_id &&
                    recent->account_id == candidate.ref.account_id) {
                    annotation = "上次使用," + annotation;
                    initial_cursor = i;  // 最近选择排默认光标位置
                }
                entries.push_back(SelectMenuEntry{
                    candidate.ref.channel_id + " · " + candidate.ref.account_id, annotation,
                    true});
            }
            entries.push_back(SelectMenuEntry{"添加新平台/账号", "", true});
            for (const channel::ChannelSetupPlatform& item : channel::ChannelSetupPlatforms()) {
                if (!item.implemented) {
                    entries.push_back(
                        SelectMenuEntry{item.display_name, "尚未支持", false});
                }
            }
            const auto picked = RunSelectMenu("选择聊天平台", entries, initial_cursor);
            if (!picked.has_value()) {
                std::printf("已取消。\n");
                return 0;
            }
            if (*picked == resolution.candidates.size()) {
                // 添加新账号:进 QQ 向导(feishu 不可选,列表已如实标注)。
                cli::ChannelSetupCommandArgs setup_args;
                setup_args.platform = "qqbot";
                const int setup_code = cli::RunChannelSetupCommand(setup_args);
                if (setup_code != 0) {
                    return setup_code;
                }
                // 向导存好后再裁一次目标并启动。
                const auto relaunched_config = config::LoadFromEnv();
                if (!relaunched_config.has_value()) {
                    std::fprintf(stderr, "im: 重读配置失败——%s\n",
                                 relaunched_config.error().c_str());
                    return 1;
                }
                ImTargetQuery retry;
                retry.channels = &relaunched_config->config.channels;
                retry.preferences = nullptr;
                retry.explicit_channel = "qqbot";
                const ImTargetResolution retry_resolution = ResolveImTarget(retry);
                if (retry_resolution.status == ImTargetResolution::Status::ReadyUnique ||
                    retry_resolution.status == ImTargetResolution::Status::DisabledAccount) {
                    return launch(*retry_resolution.target);
                }
                std::printf("配置已保存。可运行 `lubancode im qqbot` 启动。\n");
                return 0;
            }
            const ImCandidate& chosen = resolution.candidates[*picked];
            if (!chosen.enabled) {
                std::printf("账号 %s/%s 已停用。\n", chosen.ref.channel_id.c_str(),
                            chosen.ref.account_id.c_str());
                if (!AskYesNoCooked("是否启用该渠道与账号并启动", true)) {
                    std::printf("已取消。配置未改动(其他账号的 enabled 也不动)。\n");
                    return 0;
                }
                std::string enable_error;
                if (!EnableChannelAccount(chosen.ref, &enable_error)) {
                    std::fprintf(stderr, "im: 启用失败(%s)。\n", enable_error.c_str());
                    return 1;
                }
                std::printf("已启用。开始启动。\n");
            }
            return launch(chosen.ref);
        }
        case ImTargetResolution::Status::NeedSetup:
        case ImTargetResolution::Status::MissingAccount: {
            if (!interactive) {
                std::fprintf(stderr,
                             "im: %s。非交互模式不进向导;先运行 "
                             "`lubancode im setup qqbot [--account <账号>]`。\n",
                             resolution.detail.c_str());
                return 2;
            }
            std::printf("%s\n", resolution.detail.c_str());
            std::string platform = query.explicit_channel.value_or(std::string("qqbot"));
            cli::ChannelSetupCommandArgs setup_args;
            setup_args.platform = platform;
            if (query.explicit_account.has_value()) {
                setup_args.account = *query.explicit_account;
            }
            const int setup_code = cli::RunChannelSetupCommand(setup_args);
            if (setup_code != 0) {
                return setup_code;
            }
            const auto relaunched_config = config::LoadFromEnv();
            if (!relaunched_config.has_value()) {
                std::fprintf(stderr, "im: 重读配置失败——%s\n",
                             relaunched_config.error().c_str());
                return 1;
            }
            ImTargetQuery retry;
            retry.channels = &relaunched_config->config.channels;
            retry.explicit_channel = platform;
            retry.explicit_account = query.explicit_account;
            const ImTargetResolution retry_resolution = ResolveImTarget(retry);
            if ((retry_resolution.status == ImTargetResolution::Status::ReadyUnique ||
                 retry_resolution.status == ImTargetResolution::Status::DisabledAccount) &&
                retry_resolution.target.has_value()) {
                return launch(*retry_resolution.target);
            }
            std::printf("配置已保存。可运行 `lubancode im %s` 启动。\n", platform.c_str());
            return 0;
        }
        case ImTargetResolution::Status::UnknownPlatform:
        case ImTargetResolution::Status::PlatformNotImplemented:
            std::fprintf(stderr, "im: %s\n", resolution.detail.c_str());
            return 1;
    }
    std::fprintf(stderr, "im: 未处理的裁决状态\n");
    return 1;
}

}  // namespace lubancode::app
