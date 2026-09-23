// channel_setup_command.hpp 的实现。向导的问/答/显示全在这层;落盘全在
// channel::ChannelConfigService / CredentialStore,这里不另开第二套写入。
#include "cli/channel_setup_command.hpp"

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include "channel/channel_setup.hpp"
#include "channel/credential_store.hpp"
#include "channel/credentials.hpp"
#include "cli/frame_notice.hpp"  // 批 7:向导回显块走 frame;问答行不动
#include "config/config.hpp"
#include "platform/hidden_input.hpp"
#include "platform/paths.hpp"

namespace lubancode::cli {

namespace {

std::string ReadVisibleLine() {
    std::string line;
    if (!std::getline(std::cin, line)) {
        return {};  // EOF 当空输入,向导层按取消处理
    }
    // 剥可能的 \r(Windows 管道/CRLF)。
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    return line;
}

// 可见行输入(已有值可保留):回车 = 保留旧值。
std::optional<std::string> AskLine(const std::string& prompt, const std::string& keep_hint) {
    if (!keep_hint.empty()) {
        std::printf("%s[回车保留 %s]:", prompt.c_str(), keep_hint.c_str());
    } else {
        std::printf("%s:", prompt.c_str());
    }
    std::fflush(stdout);
    const std::string line = ReadVisibleLine();
    if (line.empty()) {
        if (keep_hint.empty()) {
            return std::nullopt;  // 没有可保留的值又空手:视为未填
        }
        return std::string();
    }
    return line;
}

// 隐藏输入(§5.1):粘贴/退格由平台行编辑器处理,退出与异常恢复回显。
// 返回 nullopt = 用户空手回车;expected 错误 = 无终端/读失败/EOF 取消。
std::optional<std::string> AskSecret(const std::string& prompt, const std::string& keep_hint,
                                     std::string* error_out) {
    if (!keep_hint.empty()) {
        std::printf("%s[隐藏输入,回车保留旧密钥]:", prompt.c_str());
    } else {
        std::printf("%s[隐藏输入,输入时不显示]:", prompt.c_str());
    }
    std::fflush(stdout);
    const auto line = platform::ReadHiddenLine();
    if (!line.has_value()) {
        *error_out = line.error().reason + ": " + line.error().detail;
        return std::nullopt;
    }
    return *line;
}

bool AskYesNo(const std::string& prompt, bool default_yes) {
    std::printf("%s (%s):", prompt.c_str(), default_yes ? "Y/n" : "y/N");
    std::fflush(stdout);
    const std::string line = ReadVisibleLine();
    if (line.empty()) {
        return default_yes;
    }
    return line == "y" || line == "Y" || line == "yes" || line == "是";
}

std::optional<std::string> AskToolsPreset(bool existing) {
    std::printf("\n工具权限（不用填写工具名）:\n"
                "  1. 只读查询：读文件、查资料、读技能、查时间和已有提醒\n"
                "  2. 操作前询问（推荐）：查询和管理提醒直接用；改文件、跑命令、发文件前在 QQ 上问你\n"
                "  3. 自动执行：常用工具不再逐次询问，仍受禁止项和其他权限限制\n");
    if (existing) std::printf("  回车保留当前配置；选择模式会替换原先的工具名单，保留禁止项。\n");
    else std::printf("  回车选择 2。\n");
    for (;;) {
        std::printf("请选择 1/2/3（输入 q 取消）:");
        std::fflush(stdout);
        const auto line = ReadVisibleLine();
        if (!std::cin || line == "q") return std::nullopt;
        if (line.empty()) return existing ? std::string() : std::string("ask");
        if (line == "1") return std::string("readonly");
        if (line == "2") return std::string("ask");
        if (line == "3") return std::string("auto");
        std::printf("请输入 1、2 或 3。\n");
    }
}

}  // namespace

std::vector<std::string> RenderChannelSetupBanner(const std::string& platform_display_name,
                                                  const std::string& platform_id,
                                                  const std::string& account_id,
                                                  const std::string& config_file,
                                                  const std::string& secrets_root_utf8,
                                                  const Theme& theme, int width) {
    // 四句既有文案原样进框:横幅句做标题,三句"标签: 值"按冒号拆列。
    return frame::RenderKeyValues(
        "渠道配置向导 —— " + platform_display_name,
        {SentenceField("目标账号: " + platform_id + "/" + account_id),
         SentenceField("配置文件: " + config_file),
         SentenceField("受管密钥目录: " + secrets_root_utf8)},
        theme, frame::Light(), width);
}

int RunChannelSetupCommand(const ChannelSetupCommandArgs& args) {
    // 0) 平台守门:未知平台/未实现平台先报,不进问答。认得清单从平台表拼
    // (与注册表同源——新渠道注册后自动跟上,不手抄第二份)。
    const auto platform = channel::FindChannelSetupPlatform(args.platform);
    if (!platform.has_value()) {
        // 已认得清单从平台表取(单一真源;新平台注册即跟上)。
        std::string known;
        for (const channel::ChannelSetupPlatform& candidate :
             channel::ChannelSetupPlatforms()) {
            if (!known.empty()) {
                known += "、";
            }
            known += candidate.id + (candidate.implemented ? "(可配置)" : "(尚未支持)");
        }
        std::fprintf(stderr, "channel setup: 未知平台 \"%s\"。已认得: %s\n",
                     args.platform.c_str(), known.c_str());
        return 1;
    }
    if (!platform->implemented) {
        std::fprintf(stderr, "channel setup: %s(%s) 尚未支持,不能配置\n",
                     platform->display_name.c_str(), platform->id.c_str());
        return 1;
    }
    const std::string account_id = args.account.empty() ? std::string("main") : args.account;
    // 表单字段展示名(qq:AppID/AppSecret;飞书:App ID/App Secret)。
    std::string app_id_label = "AppID";
    std::string secret_label = "AppSecret";
    for (const channel::ChannelSetupField& field : platform->fields) {
        if (field.id == "app_id") {
            app_id_label = field.label;
        } else if (field.id == "app_secret") {
            secret_label = field.label;
        }
    }

    // 1) 终端守门:向导要问答,没交互终端就明报(自动化走 secret_file/
    //    secret_env,不用向导)。先于一切提问,不弹任何 UI。
    if (!platform::StdinIsTerminal()) {
        std::fprintf(stderr,
                     "channel setup: 当前不是交互终端,无法运行配置向导。\n"
                     "  自动化配置请直接编辑全局 config.json 的 channels 段\n"
                     "  (AppSecret 用 secret_file 或 secret_env 引用,不要写明文)。\n");
        return 2;
    }

    // 2) 定位配置与凭据根(应用根覆盖后的实际位置,§5.1)。
    auto options = channel::ChannelConfigService::DefaultOptions();
    if (!options.has_value()) {
        std::fprintf(stderr, "channel setup: %s\n", options.error().detail.c_str());
        return 1;
    }
    // 批 7:头部信息块走键值对框(RenderChannelSetupBanner 纯函数渲染,
    // 形状册直调);printf 散打收口 TermOut。
    EmitFrameLines(RenderChannelSetupBanner(platform->display_name, platform->id, account_id,
                                            options->config_file,
                                            platform::PathToUtf8(options->secrets_root),
                                            CliTheme(), CliFrameWidth()));
    TermOut() << "\n";

    // 3) 既有账号现况(旧密钥绝不回显,只报有无与权限结论)。
    const auto config_loaded = config::LoadFromEnv();
    bool channel_enabled = false;
    bool account_enabled = false;
    bool has_app_id = false;
    std::string old_secret_file;
    bool old_secret_managed = false;
    bool old_secret_secure = false;
    bool account_exists = false;
    std::string old_security_detail;
    if (config_loaded.has_value()) {
        const auto status =
            channel::InspectChannelAccount(config_loaded->config.channels, options->secrets_root,
                                           platform->id, account_id);
        if (status.exists) {
            account_exists = true;
            channel_enabled = status.channel_enabled;
            account_enabled = status.enabled;
            has_app_id = status.has_app_id;
            old_secret_file = status.secret_file;
            old_secret_managed = status.secret_file_managed;
            old_secret_secure = status.secret_file_secure;
            old_security_detail = status.security_detail;
            PrintNotice(CliTheme(), {"该账号已配置:渠道 " +
                                             std::string(channel_enabled ? "已启用" : "未启用") +
                                             ",账号 " +
                                             std::string(account_enabled ? "已启用" : "未启用") +
                                             ",AppID " + std::string(has_app_id ? "已填" : "未填") +
                                             ",密钥文件 " +
                                             std::string(old_secret_file.empty() ? "未配" : "已配")});
        } else {
            PrintNotice(CliTheme(), {"该账号尚未配置,将按 " + platform->display_name +
                                             " 模板新建(websocket、私聊配对、群聊禁用、final 回复)。"});
        }
    }

    if (args.permissions_only) {
        if (!account_exists) {
            std::fprintf(stderr, "账号尚未配置，请先运行 lubancode channel setup %s --account %s。\n",
                         platform->id.c_str(), account_id.c_str());
            return 1;
        }
        const auto& current = config_loaded->config.channels.at(platform->id).accounts.at(account_id).tools;
        // 裸配置(未写权限)在解析侧保持 nullopt;QQ 账号的默认询问档在
        // 注册侧生效——这里照实显示生效档,标"默认"以区分显式保存的档位。
        const bool default_ask = platform->id == "qqbot" && current.preset.empty() &&
                                 !current.allow && !current.approve;
        const char* label = current.preset == "ask" ? "操作前询问" :
                            current.preset == "auto" ? "自动执行" :
                            current.preset == "readonly" ? "只读查询" :
                            default_ask ? "操作前询问（默认）" : "自定义工具名单";
        PrintNotice(CliTheme(), {"当前策略: " + std::string(label)});
        const auto selected = AskToolsPreset(true);
        if (!selected || selected->empty()) {
            PrintNotice(CliTheme(), {"未修改权限。"});
            return 0;
        }
        channel::ChannelSetupCommitRequest request;
        request.channel_id = platform->id;
        request.account_id = account_id;
        request.ensure_enabled = false;
        request.tools_preset = *selected;
        request.dry_run = true;
        const auto preview = channel::ChannelConfigService::Commit(*options, request);
        if (!preview) {
            std::fprintf(stderr, "%s\n", preview.error().detail.c_str());
            return 1;
        }
        {
            std::vector<frame::ListRow> rows;
            for (const auto& change : preview->changes) {
                rows.push_back(frame::ListRow{change, {}, {}, frame::Bullet::None});
            }
            if (!rows.empty()) {
                EmitFrameLines(frame::RenderList({}, rows, CliTheme(), frame::Light(),
                                                 CliFrameWidth()));
            }
        }
        if (!AskYesNo("确认保存权限", false) || !std::cin) return 0;
        request.dry_run = false;
        const auto saved = channel::ChannelConfigService::Commit(*options, request);
        if (!saved) {
            std::fprintf(stderr, "%s\n", saved.error().detail.c_str());
            return 1;
        }
        PrintNotice(CliTheme(), {"权限已保存。重启 Gateway 后生效；在 QQ 发 /tools 查看实际可用项。"});
        return 0;
    }

    // 4) 已有密钥文件权限不合(§5.3):受管件给"收紧"选项;外部路径默认
    //    引导重存受管位置。owner 不符时不夺权,准确报错后走重存。
    if (!old_secret_file.empty() && !old_secret_secure) {
        PrintNotice(CliTheme(), {"现有密钥文件权限不合格: " + old_secret_file,
                                 "原因: " + old_security_detail});
        if (old_secret_managed) {
            if (AskYesNo("收紧这份密钥文件权限(程序代改 ACL,不需要管理员)", true)) {
                const channel::CredentialStore store(options->secrets_root);
                std::error_code canon_ec;
                const auto target = std::filesystem::weakly_canonical(
                    platform::Utf8ToPath(old_secret_file), canon_ec);
                const auto tightened = store.TightenFilePermissions(
                    canon_ec ? platform::Utf8ToPath(old_secret_file) : target);
                if (tightened.has_value()) {
                    old_secret_secure = true;
                    PrintNotice(CliTheme(), {"已收紧并复验通过。"});
                } else {
                    std::fprintf(stderr, "收紧失败: %s\n", tightened.error().detail.c_str());
                    std::fprintf(stderr,
                                 "可重新输入密钥保存到受管位置(继续向导即可),旧文件不会被改内容。\n");
                }
            }
        } else {
            PrintNotice(CliTheme(), {"这是外部自定义路径。默认做法:重新输入密钥,保存到受管位置(" +
                                             platform::PathToUtf8(options->secrets_root) + ")。",
                                     "不会改动旧外部文件与其父目录。"});
        }
    }

    // 5) 问答:App ID(可见,可保留)→ App Secret(隐藏,可保留)。
    std::printf("\n");
    std::optional<std::string> app_id;
    if (has_app_id) {
        const auto answer = AskLine(app_id_label, "旧值");
        if (answer.has_value() && !answer->empty()) {
            app_id = *answer;
        }
    } else {
        while (!app_id.has_value()) {
            const auto answer = AskLine(app_id_label, std::string());
            if (!answer.has_value() || answer->empty()) {
                std::printf("%s 不能为空,请重新输入(或 Ctrl+C 退出)。\n", app_id_label.c_str());
                continue;
            }
            app_id = *answer;
        }
    }
    std::optional<std::string> new_secret;
    {
        std::string secret_error;
        const auto answer = AskSecret(secret_label, old_secret_file.empty() ? std::string() : "旧密钥",
                                      &secret_error);
        if (!secret_error.empty()) {
            std::fprintf(stderr, "\n%s 输入失败: %s\n", secret_label.c_str(), secret_error.c_str());
            std::fprintf(stderr, "未做任何修改。\n");
            return 1;
        }
        if (answer.has_value() && !answer->empty()) {
            new_secret = *answer;
        } else if (answer.has_value() && old_secret_file.empty()) {
            // 空手回车又没有旧密钥:再问一次,连续空手视为取消。
            const auto retry = AskSecret(secret_label + "(不能为空)", std::string(), &secret_error);
            if (!secret_error.empty() || !retry.has_value() || retry->empty()) {
                std::fprintf(stderr, "\n未填写 AppSecret,取消保存。未做任何修改。\n");
                return 1;
            }
            new_secret = *retry;
        }
    }

    // 6) 停用账号显式询问启用(不暗改开关)。
    bool ensure_enabled = true;
    if (config_loaded.has_value() && channel_enabled && account_enabled) {
        ensure_enabled = true;  // 本来就开着:保持
    } else {
        ensure_enabled =
            AskYesNo("保存时启用该渠道与账号(channels." + platform->id + ".enabled 与账号 enabled)",
                     true);
    }

    const auto selected_tools = AskToolsPreset(account_exists);
    if (!selected_tools) {
        PrintNotice(CliTheme(), {"已取消保存。"});
        return 0;
    }
    // 7) 差异预览(dry_run,一页未写)→ 确认 → 提交。
    channel::ChannelSetupCommitRequest request;
    request.channel_id = platform->id;
    request.account_id = account_id;
    request.app_id = app_id;
    request.new_secret = new_secret;
    request.ensure_enabled = ensure_enabled;
    if (!selected_tools->empty()) request.tools_preset = *selected_tools;
    request.dry_run = true;
    const auto preview = channel::ChannelConfigService::Commit(options.value(), request);
    if (!preview.has_value()) {
        std::fprintf(stderr, "channel setup: %s(%s)\n", preview.error().detail.c_str(),
                     preview.error().reason.c_str());
        return 1;
    }
    // 批 7:预览标题句(尾冒号剥掉)做 frame 标题,变更清单走列表。
    {
        std::vector<frame::ListRow> rows;
        for (const std::string& change : preview->changes) {
            rows.push_back(frame::ListRow{change, {}, {}, frame::Bullet::None});
        }
        EmitFrameLines(frame::RenderList("将保存以下改动", rows, CliTheme(), frame::Light(),
                                         CliFrameWidth()));
        PrintNotice(CliTheme(), {"其他账号、模型配置与未认得的字段原样保留。"});
    }
    if (!AskYesNo("确认保存", true)) {
        PrintNotice(CliTheme(), {"已取消。未做任何修改。"});
        return 0;
    }
    request.dry_run = false;
    const auto committed = channel::ChannelConfigService::Commit(options.value(), request);
    if (!committed.has_value()) {
        std::fprintf(stderr, "channel setup: 保存失败(%s): %s\n",
                     committed.error().reason.c_str(), committed.error().detail.c_str());
        std::fprintf(stderr, "旧配置保留原样。\n");
        return 1;
    }

    // 8) 收尾:明示"已保存/尚未验证",给 gateway run 入口;提醒不热更新。
    PrintNotice(CliTheme(), {"配置已保存: " + committed->config_file,
                             "密钥文件: " + committed->secret_file + "(权限已按仅当前用户收紧)",
                             "尚未验证连接:本向导没有连 " + platform->display_name + "。",
                             "启动:在当前目录运行 `lubancode gateway run`(或 `lubancode im`)。",
                             "注意:正在运行的 Gateway 不会热加载配置,须重启后生效。"});
    TermOut().flush();
    return 0;
}

}  // namespace lubancode::cli
