// /telemetry 的实现。合同见 telemetry_commands.hpp 文件头。
//
// §24.2 命令族(T1 status 先行,T2 补真):
//   /telemetry [status]   只显示状态,不改配置不发请求
//   enable|disable        裸敲只给选项;session = 当前进程;config = 写
//                         全局配置文件(features.telemetry 一枚布尔,其余
//                         字段原样保留)——§24.2 "必须让用户明确选,不能暗
//                         改项目文件":项目文件永远不碰。
//   pause|resume          停/复出口,本地投影与 spool 照常
//   flush [毫秒]          seal + 有界赶发(§24.2 "不强制等公网无限久")
//   spool [clear --confirm]  列路径/字节/批次;删除动作两步确认
//   consent [grant|revoke]   §8.4 公网确认的本地记录
//   policy                远端策略属 T4,明说不装样子
#include "app/commands/telemetry_commands.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "cli/terminal_port.hpp"
#include "config/config.hpp"
#include "telemetry/exporter.hpp"
#include "tools/path_utils.hpp"

namespace lubancode::app {
namespace {
using lubancode::cli::TermOut;

// 拆 "/telemetry a b --c" 的词。args 首词是子命令,其余当参数。
std::vector<std::string> SplitWords(const std::string& args) {
    std::vector<std::string> words;
    std::string current;
    for (char ch : args) {
        if (ch == ' ' || ch == '\t') {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        words.push_back(current);
    }
    return words;
}

// 全局配置文件路径:优先 ConfigResult 记的现行路径,没有(还没建过配置)
// 就落到 <主目录>/.lubancode/config.json(SetTelemetryFeatureInConfigFile
// 支持从空文件起写)。
std::optional<std::filesystem::path> GlobalConfigPath(SlashDispatchContext& ctx) {
    if (ctx.config_result != nullptr &&
        ctx.config_result->global_config_file_path.has_value()) {
        return lubancode::tools::Utf8ToPath(*ctx.config_result->global_config_file_path);
    }
    const auto home = lubancode::config::HomeLubancodeDir();
    if (!home.has_value()) {
        return std::nullopt;
    }
    return lubancode::tools::Utf8ToPath(*home) / "config.json";
}

void PrintEnableChoices(SlashDispatchContext& ctx) {
    TermOut() << ctx.theme->stats
              << "enable 只对当前进程还是写配置,须你挑一个(§24.2 不暗改文件):\n"
              << "  /telemetry enable session  当前进程内开遥测,不落盘,下场会话回到真值\n"
              << "  /telemetry enable config   写全局配置 features.telemetry=true(项目配置不动)\n"
              << ctx.theme->reset;
}

void PrintDisableChoices(SlashDispatchContext& ctx) {
    TermOut() << ctx.theme->stats
              << "disable 停在哪一档,须你挑一个:\n"
              << "  /telemetry disable session  当前进程停采停发,seal 并保留未 ACK 的 spool(§26.4)\n"
              << "  /telemetry disable config   写全局配置 features.telemetry=false(项目配置不动)\n"
              << ctx.theme->reset;
}

}  // namespace

CommandFlow HandleSlashTelemetry(SlashDispatchContext& ctx,
                                 const lubancode::cli::ParsedSlashCommand& parsed) {
    const std::vector<std::string> words = SplitWords(parsed.args);
    const std::string sub = words.empty() ? std::string("status") : words[0];

    if (sub == "status" || (sub.empty() && words.empty())) {
        if (ctx.telemetry_service == nullptr) {
            // 未装配 = 激活判定非 Active(默认关闭/环境变量关/总闸/缺前置)。
            TermOut() << ctx.theme->stats
                      << "遥测未开启(features.telemetry 默认关;开启须与 "
                         "遥测需要轨迹账在场;也可 /telemetry enable session 只开本场)"
                      << ctx.theme->reset << "\n";
            return CommandFlow::Continue;
        }
        const lubancode::telemetry::TelemetryServiceStatus status =
            ctx.telemetry_service->Status();
        for (const std::string& line : lubancode::telemetry::FormatTelemetryStatusLines(status)) {
            TermOut() << line << "\n";
        }
        return CommandFlow::Continue;
    }

    if (sub == "enable") {
        if (words.size() < 2) {
            PrintEnableChoices(ctx);
            return CommandFlow::Continue;
        }
        if (words[1] == "session") {
            if (ctx.enable_telemetry_session == nullptr) {
                TermOut() << ctx.theme->stats
                          << "本场装配没接会话级执行体,enable session 接不上(配置文件路仍可用)"
                          << ctx.theme->reset << "\n";
                return CommandFlow::Continue;
            }
            for (const std::string& line : ctx.enable_telemetry_session()) {
                TermOut() << ctx.theme->stats << line << ctx.theme->reset << "\n";
            }
            return CommandFlow::Continue;
        }
        if (words[1] == "config") {
            const auto path = GlobalConfigPath(ctx);
            if (!path.has_value()) {
                TermOut() << ctx.theme->stats << "找不到用户主目录,写不了全局配置" << ctx.theme->reset
                          << "\n";
                return CommandFlow::Continue;
            }
            const auto written =
                lubancode::config::SetTelemetryFeatureInConfigFile(path->string(), true);
            if (!written.has_value()) {
                TermOut() << ctx.theme->error << "写入失败: " << written.error() << ctx.theme->reset
                          << "\n";
                return CommandFlow::Continue;
            }
            TermOut() << ctx.theme->stats
                      << "已写 features.telemetry=true -> " << lubancode::tools::PathToUtf8(*path)
                      << "(下场会话生效;本场要立即开可再敲 /telemetry enable session)"
                      << ctx.theme->reset << "\n";
            return CommandFlow::Continue;
        }
        PrintEnableChoices(ctx);
        return CommandFlow::Continue;
    }

    if (sub == "disable") {
        if (words.size() < 2) {
            PrintDisableChoices(ctx);
            return CommandFlow::Continue;
        }
        if (words[1] == "session") {
            if (ctx.telemetry_service == nullptr) {
                TermOut() << ctx.theme->stats << "遥测本来就没开" << ctx.theme->reset << "\n";
                return CommandFlow::Continue;
            }
            // §26.4:立即停采停发、seal spool、保留未 ACK 段。
            ctx.telemetry_service->Stop();
            TermOut() << ctx.theme->stats
                      << "本场遥测已停:停采停发,active 段已 seal,未 ACK 的 spool 保留在 "
                      << lubancode::tools::PathToUtf8(ctx.telemetry_service->options().telemetry_root)
                      << ctx.theme->reset << "\n";
            return CommandFlow::Continue;
        }
        if (words[1] == "config") {
            const auto path = GlobalConfigPath(ctx);
            if (!path.has_value()) {
                TermOut() << ctx.theme->stats << "找不到用户主目录,写不了全局配置" << ctx.theme->reset
                          << "\n";
                return CommandFlow::Continue;
            }
            const auto written =
                lubancode::config::SetTelemetryFeatureInConfigFile(path->string(), false);
            if (!written.has_value()) {
                TermOut() << ctx.theme->error << "写入失败: " << written.error() << ctx.theme->reset
                          << "\n";
                return CommandFlow::Continue;
            }
            TermOut() << ctx.theme->stats
                      << "已写 features.telemetry=false -> "
                      << lubancode::tools::PathToUtf8(*path)
                      << "(下场会话生效;旧 spool 原样保留)"
                      << ctx.theme->reset << "\n";
            return CommandFlow::Continue;
        }
        PrintDisableChoices(ctx);
        return CommandFlow::Continue;
    }

    if (sub == "pause" || sub == "resume") {
        if (ctx.telemetry_service == nullptr) {
            TermOut() << ctx.theme->stats << "遥测未开启,没有出口可" << sub << ctx.theme->reset << "\n";
            return CommandFlow::Continue;
        }
        ctx.telemetry_service->SetExportPaused(sub == "pause");
        TermOut() << ctx.theme->stats
                  << (sub == "pause" ? "出口已暂停:本地投影与 spool 照常落(§24.2 pause)"
                                     : "出口已恢复")
                  << ctx.theme->reset << "\n";
        return CommandFlow::Continue;
    }

    if (sub == "flush") {
        if (ctx.telemetry_service == nullptr) {
            TermOut() << ctx.theme->stats << "遥测未开启,没东西可 flush" << ctx.theme->reset << "\n";
            return CommandFlow::Continue;
        }
        std::int64_t bounded_ms = 5000;  // §26.3 flush 有硬上限
        if (words.size() >= 2) {
            try {
                bounded_ms = std::stoll(words[1]);
            } catch (...) {
                TermOut() << ctx.theme->stats << "毫秒数认不得: " << words[1] << ctx.theme->reset
                          << "\n";
                return CommandFlow::Continue;
            }
            if (bounded_ms < 0 || bounded_ms > 30000) {
                bounded_ms = std::min<std::int64_t>(std::max<std::int64_t>(bounded_ms, 0), 30000);
            }
        }
        const bool drained = ctx.telemetry_service->Flush(bounded_ms);
        const auto status = ctx.telemetry_service->Status();
        TermOut() << ctx.theme->stats
                  << (drained ? "flush 完成:存量 sealed 批已出清(或出口未开/被暂停)"
                              : "flush 有界等待到点,仍有批未出(出口慢/在退避;spool 不丢)")
                  << "; spool 余 " << status.spool.segments << " 段 "
                  << status.spool.sealed_batches << " 批"
                  << ctx.theme->reset << "\n";
        return CommandFlow::Continue;
    }

    if (sub == "spool") {
        if (ctx.telemetry_service == nullptr) {
            TermOut() << ctx.theme->stats << "遥测未开启,没有 spool" << ctx.theme->reset << "\n";
            return CommandFlow::Continue;
        }
        const auto status = ctx.telemetry_service->Status();
        const std::filesystem::path spool_dir =
            ctx.telemetry_service->options().telemetry_root / "spool";
        if (words.size() >= 2 && words[1] == "clear") {
            bool confirmed = false;
            for (std::size_t i = 2; i < words.size(); ++i) {
                if (words[i] == "--confirm") {
                    confirmed = true;
                }
            }
            if (!confirmed) {
                // §24.2:先列路径、字节、批次数与不可恢复性,再确认。
                TermOut() << ctx.theme->stats
                          << "spool clear 是删除动作,不可恢复。将要删的是:\n"
                          << "  目录: " << lubancode::tools::PathToUtf8(spool_dir) << "\n"
                          << "  sealed 段: " << status.spool.segments << " 段 "
                          << status.spool.bytes << " 字节 "
                          << status.spool.sealed_batches << " 批(未出口即弃,不再补送)\n"
                          << "  active 半段: " << status.spool.active_batches << " 批\n"
                          << "确认无误再敲: /telemetry spool clear --confirm\n"
                          << "(cursor 对账账会先记退场水位,不会误报孤儿)"
                          << ctx.theme->reset;
                return CommandFlow::Continue;
            }
            const auto [segments, batches] = ctx.telemetry_service->ClearSpool();
            TermOut() << ctx.theme->stats << "已清 spool: 删 " << segments << " 段 " << batches
                      << " 批(退场水位与 tombstone 已记账)" << ctx.theme->reset << "\n";
            return CommandFlow::Continue;
        }
        TermOut() << ctx.theme->stats
                  << "spool 目录: " << lubancode::tools::PathToUtf8(spool_dir) << "\n"
                  << "sealed: " << status.spool.segments << " 段 " << status.spool.bytes
                  << " 字节 " << status.spool.sealed_batches << " 批; 最老段龄 "
                  << (status.spool.oldest_age_ms < 0 ? std::string("无")
                                                     : std::to_string(status.spool.oldest_age_ms / 1000) + "s")
                  << "\n"
                  << "active 半段: " << status.spool.active_batches << " 批"
                  << (status.spool.degraded ? " [降级: 磁盘帽]" : "") << "\n"
                  << "清理/确认删除走 /telemetry spool clear" << ctx.theme->reset;
        return CommandFlow::Continue;
    }

    if (sub == "consent") {
        if (ctx.telemetry_service == nullptr) {
            TermOut() << ctx.theme->stats << "遥测未开启,consent 无从谈起" << ctx.theme->reset << "\n";
            return CommandFlow::Continue;
        }
        const auto& options = ctx.telemetry_service->options();
        if (words.size() >= 2 && words[1] == "grant") {
            if (!options.exporter.configured()) {
                TermOut() << ctx.theme->stats
                          << "没配 telemetry.exporter.endpoint,授权无从谈起" << ctx.theme->reset
                          << "\n";
                return CommandFlow::Continue;
            }
            // §8.4 披露:授权前把六项摆给用户看(凭证只报名,不出值)。
            TermOut() << ctx.theme->stats
                      << "本次授权将放行向以下 endpoint 发送遥测:\n"
                      << "  endpoint/协议: " << lubancode::telemetry::SanitizeEndpointForDisplay(
                                                   options.exporter.endpoint)
                      << "\n"
                      << "  数据等级: " << lubancode::telemetry::DataClassName(options.data_class)
                      << "\n"
                      << "  脱敏规则版本: " << lubancode::telemetry::kRedactionPolicyVersion << "\n"
                      << "  本地 spool: " << lubancode::tools::PathToUtf8(options.telemetry_root)
                      << " (容量帽 " << options.spool.total_bytes_cap << " 字节)\n"
                      << "  凭证来源: "
                      << (options.exporter.secret_ref.empty()
                              ? std::string("匿名(未配 secret_ref)")
                              : ("环境变量 " + options.exporter.secret_ref + "(不展示值)"))
                      << "\n"
                      << "  远端策略: 未启用(T4 前,云端不下发策略)\n"
                      << "  resource attributes: service.version=" << options.resource.service_version
                      << ", frontend=" << options.resource.frontend << ", os.type="
                      << (options.resource.os_type.empty() ? "(装配层未给)" : options.resource.os_type)
                      << "\n"
                      << "endpoint/数据等级/脱敏版本任一变更须重新授权。"
                      << ctx.theme->reset;
            if (ctx.telemetry_service->GrantConsent()) {
                TermOut() << ctx.theme->stats << "consent 已记录,出口放行。" << ctx.theme->reset
                          << "\n";
            } else {
                TermOut() << ctx.theme->error << "consent 落盘失败(目录权限?)" << ctx.theme->reset
                          << "\n";
            }
            return CommandFlow::Continue;
        }
        if (words.size() >= 2 && words[1] == "revoke") {
            const bool ok = ctx.telemetry_service->RevokeConsent();
            TermOut() << ctx.theme->stats
                      << (ok ? "consent 已撤回;非回环出口立即停发(telemetry.consent_required)"
                             : "撤回时出问题,再试一次")
                      << ctx.theme->reset << "\n";
            return CommandFlow::Continue;
        }
        TermOut() << ctx.theme->stats
                  << "consent 状态: " << ctx.telemetry_service->ConsentState()
                  << "(回环 endpoint 免披露;公网先 /telemetry consent grant)"
                  << ctx.theme->reset << "\n";
        return CommandFlow::Continue;
    }

    if (sub == "policy") {
        TermOut() << ctx.theme->stats
                  << "远端策略(telemetry.remote_policy)属 T4:签名策略、本地裁决、"
                     "越权拒绝尚未落地。当前本地配置即全部策略,云端不下发任何东西。"
                  << ctx.theme->reset << "\n";
        return CommandFlow::Continue;
    }

    TermOut() << ctx.theme->stats
              << "/telemetry " << sub
              << ": 认不得。可用: status|enable|disable|pause|resume|flush|spool|consent|policy"
              << ctx.theme->reset << "\n";
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
