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
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include "cli/terminal_frame.hpp"  // frame::*(TUI 排版批 5b:/telemetry 渲染段)
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "platform/console.hpp"  // GetScreenInfo:整条 /telemetry 的框宽同一把尺
#include "telemetry/exporter.hpp"
#include "telemetry/service.hpp"  // TelemetryService 完整定义(HC-06:不再经注册表头传递)
#include "tools/path_utils.hpp"

namespace lubancode::app {
namespace {
using lubancode::cli::TermOut;

namespace frame = lubancode::cli::frame;

// ---- TUI 排版批 5b(/telemetry 全族)的公共小件(批 2 同款) ----------------
//
// 本文件文案是硬编码中文(不走 i18n 表),单子合同"不新增文案"在此读作:
// 既有句子原样进 frame,一字不添不改;SentenceField 拆句内冒号,没有冒号
// 的整句进 value。

int TelemetryFrameWidth() {
    if (const auto info = lubancode::platform::GetScreenInfo()) {
        return info->width;
    }
    return 0;
}

void EmitFrameLines(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        TermOut() << line << "\n";
    }
}

std::string TrimAscii(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

frame::Field SentenceField(const std::string& sentence,
                           frame::FieldAccent accent = frame::FieldAccent::None) {
    const std::size_t colon = sentence.find(':');
    if (colon == std::string::npos) {
        return frame::Field{"", sentence, accent};
    }
    return frame::Field{TrimAscii(sentence.substr(0, colon)), TrimAscii(sentence.substr(colon + 1)), accent};
}

void PrintTelemetryNotice(const lubancode::cli::Theme& theme, std::initializer_list<std::string> sentences,
                          frame::FieldAccent accent = frame::FieldAccent::None) {
    std::vector<frame::Field> fields;
    for (const std::string& sentence : sentences) {
        fields.push_back(SentenceField(sentence, accent));
    }
    EmitFrameLines(frame::RenderKeyValues({}, fields, theme, frame::Light(), TelemetryFrameWidth()));
}

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
std::optional<std::filesystem::path> GlobalConfigPath(const TelemetryCommandContext& ctx) {
    if (ctx.global_config_file_path != nullptr && ctx.global_config_file_path->has_value()) {
        return lubancode::tools::Utf8ToPath(**ctx.global_config_file_path);
    }
    const auto home = lubancode::config::HomeLubancodeDir();
    if (!home.has_value()) {
        return std::nullopt;
    }
    return lubancode::tools::Utf8ToPath(*home) / "config.json";
}

void PrintEnableChoices(const TelemetryCommandContext& ctx) {
    std::vector<frame::ListRow> rows;
    rows.push_back(frame::ListRow{"/telemetry enable session",
                                  "当前进程内开遥测,不落盘,下场会话回到真值", {}, frame::Bullet::User});
    rows.push_back(frame::ListRow{"/telemetry enable config",
                                  "写全局配置 features.telemetry=true(项目配置不动)", {}, frame::Bullet::User});
    EmitFrameLines(frame::RenderList("enable 只对当前进程还是写配置,须你挑一个(§24.2 不暗改文件)", rows,
                                     *ctx.theme, frame::Light(), TelemetryFrameWidth()));
}

void PrintDisableChoices(const TelemetryCommandContext& ctx) {
    std::vector<frame::ListRow> rows;
    rows.push_back(frame::ListRow{"/telemetry disable session",
                                  "当前进程停采停发,seal 并保留未 ACK 的 spool(§26.4)", {},
                                  frame::Bullet::User});
    rows.push_back(frame::ListRow{"/telemetry disable config",
                                  "写全局配置 features.telemetry=false(项目配置不动)", {},
                                  frame::Bullet::User});
    EmitFrameLines(frame::RenderList("disable 停在哪一档,须你挑一个", rows, *ctx.theme, frame::Light(),
                                      TelemetryFrameWidth()));
}

}  // namespace

// HC-06(材料收窄):分派位只吃本域窄材料——绑定单元在装配期折好递进来,
// handler 编译不再需要会话大上下文。
CommandFlow HandleSlashTelemetry(const TelemetryCommandContext& ctx,
                                 const lubancode::cli::ParsedSlashCommand& parsed) {
    const std::vector<std::string> words = SplitWords(parsed.args);
    const std::string sub = words.empty() ? std::string("status") : words[0];

    if (sub == "status" || (sub.empty() && words.empty())) {
        if (ctx.telemetry_service == nullptr) {
            // 未装配 = 激活判定非 Active(默认关闭/环境变量关/总闸/缺前置)。
            PrintTelemetryNotice(*ctx.theme,
                                 {"遥测未开启(features.telemetry 默认关;开启须与 "
                                  "遥测需要轨迹账在场;也可 /telemetry enable session 只开本场)"});
            return CommandFlow::Continue;
        }
        const lubancode::telemetry::TelemetryServiceStatus status =
            ctx.telemetry_service->Status();
        // 状态行出自遥测域的 FormatTelemetryStatusLines(句式"标签: 值"),
        // 这里只按句内冒号拆两列进键值对框,service 一字不动。
        std::vector<frame::Field> fields;
        for (const std::string& line : lubancode::telemetry::FormatTelemetryStatusLines(status)) {
            fields.push_back(SentenceField(line));
        }
        EmitFrameLines(
            frame::RenderKeyValues({}, fields, *ctx.theme, frame::Light(), TelemetryFrameWidth()));
        return CommandFlow::Continue;
    }

    if (sub == "enable") {
        if (words.size() < 2) {
            PrintEnableChoices(ctx);
            return CommandFlow::Continue;
        }
        if (words[1] == "session") {
            if (ctx.enable_telemetry_session == nullptr) {
                PrintTelemetryNotice(*ctx.theme,
                                     {"本场装配没接会话级执行体,enable session 接不上(配置文件路仍可用)"});
                return CommandFlow::Continue;
            }
            std::vector<frame::Field> session_fields;
            for (const std::string& line : ctx.enable_telemetry_session()) {
                session_fields.push_back(SentenceField(line));
            }
            EmitFrameLines(frame::RenderKeyValues({}, session_fields, *ctx.theme, frame::Light(),
                                                  TelemetryFrameWidth()));
            return CommandFlow::Continue;
        }
        if (words[1] == "config") {
            const auto path = GlobalConfigPath(ctx);
            if (!path.has_value()) {
                PrintTelemetryNotice(*ctx.theme, {"找不到用户主目录,写不了全局配置"});
                return CommandFlow::Continue;
            }
            const auto written =
                lubancode::config::SetTelemetryFeatureInConfigFile(path->string(), true);
            if (!written.has_value()) {
                PrintTelemetryNotice(*ctx.theme, {"写入失败: " + written.error()},
                                     frame::FieldAccent::Error);
                return CommandFlow::Continue;
            }
            PrintTelemetryNotice(*ctx.theme,
                                 {"已写 features.telemetry=true -> " +
                                  lubancode::tools::PathToUtf8(*path) +
                                  "(下场会话生效;本场要立即开可再敲 /telemetry enable session)"});
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
                PrintTelemetryNotice(*ctx.theme, {"遥测本来就没开"});
                return CommandFlow::Continue;
            }
            // §26.4:立即停采停发、seal spool、保留未 ACK 段。
            ctx.telemetry_service->Stop();
            PrintTelemetryNotice(
                *ctx.theme,
                {"本场遥测已停:停采停发,active 段已 seal,未 ACK 的 spool 保留在 " +
                 lubancode::tools::PathToUtf8(ctx.telemetry_service->options().telemetry_root)});
            return CommandFlow::Continue;
        }
        if (words[1] == "config") {
            const auto path = GlobalConfigPath(ctx);
            if (!path.has_value()) {
                PrintTelemetryNotice(*ctx.theme, {"找不到用户主目录,写不了全局配置"});
                return CommandFlow::Continue;
            }
            const auto written =
                lubancode::config::SetTelemetryFeatureInConfigFile(path->string(), false);
            if (!written.has_value()) {
                PrintTelemetryNotice(*ctx.theme, {"写入失败: " + written.error()},
                                     frame::FieldAccent::Error);
                return CommandFlow::Continue;
            }
            PrintTelemetryNotice(
                *ctx.theme, {"已写 features.telemetry=false -> " + lubancode::tools::PathToUtf8(*path) +
                                 "(下场会话生效;旧 spool 原样保留)"});
            return CommandFlow::Continue;
        }
        PrintDisableChoices(ctx);
        return CommandFlow::Continue;
    }

    if (sub == "pause" || sub == "resume") {
        if (ctx.telemetry_service == nullptr) {
            PrintTelemetryNotice(*ctx.theme, {"遥测未开启,没有出口可" + sub});
            return CommandFlow::Continue;
        }
        ctx.telemetry_service->SetExportPaused(sub == "pause");
        PrintTelemetryNotice(*ctx.theme,
                             {sub == "pause" ? "出口已暂停:本地投影与 spool 照常落(§24.2 pause)"
                                             : "出口已恢复"});
        return CommandFlow::Continue;
    }

    if (sub == "flush") {
        if (ctx.telemetry_service == nullptr) {
            PrintTelemetryNotice(*ctx.theme, {"遥测未开启,没东西可 flush"});
            return CommandFlow::Continue;
        }
        std::int64_t bounded_ms = 5000;  // §26.3 flush 有硬上限
        if (words.size() >= 2) {
            try {
                bounded_ms = std::stoll(words[1]);
            } catch (...) {
                PrintTelemetryNotice(*ctx.theme, {"毫秒数认不得: " + words[1]});
                return CommandFlow::Continue;
            }
            if (bounded_ms < 0 || bounded_ms > 30000) {
                bounded_ms = std::min<std::int64_t>(std::max<std::int64_t>(bounded_ms, 0), 30000);
            }
        }
        const bool drained = ctx.telemetry_service->Flush(bounded_ms);
        const auto status = ctx.telemetry_service->Status();
        PrintTelemetryNotice(*ctx.theme,
                             {std::string(drained ? "flush 完成:存量 sealed 批已出清(或出口未开/被暂停)"
                                                  : "flush 有界等待到点,仍有批未出(出口慢/在退避;spool 不丢)") +
                              "; spool 余 " + std::to_string(status.spool.segments) + " 段 " +
                              std::to_string(status.spool.sealed_batches) + " 批"});
        return CommandFlow::Continue;
    }

    if (sub == "spool") {
        if (ctx.telemetry_service == nullptr) {
            PrintTelemetryNotice(*ctx.theme, {"遥测未开启,没有 spool"});
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
                PrintTelemetryNotice(*ctx.theme,
                                     {"spool clear 是删除动作,不可恢复。将要删的是:",
                                      "目录: " + lubancode::tools::PathToUtf8(spool_dir),
                                      "sealed 段: " + std::to_string(status.spool.segments) + " 段 " +
                                          std::to_string(status.spool.bytes) + " 字节 " +
                                          std::to_string(status.spool.sealed_batches) +
                                          " 批(未出口即弃,不再补送)",
                                      "active 半段: " + std::to_string(status.spool.active_batches) + " 批",
                                      "确认无误再敲: /telemetry spool clear --confirm",
                                      "(cursor 对账账会先记退场水位,不会误报孤儿)"});
                return CommandFlow::Continue;
            }
            const auto [segments, batches] = ctx.telemetry_service->ClearSpool();
            PrintTelemetryNotice(*ctx.theme, {"已清 spool: 删 " + std::to_string(segments) + " 段 " +
                                                  std::to_string(batches) +
                                                  " 批(退场水位与 tombstone 已记账)"});
            return CommandFlow::Continue;
        }
        PrintTelemetryNotice(
            *ctx.theme,
            {"spool 目录: " + lubancode::tools::PathToUtf8(spool_dir),
             "sealed: " + std::to_string(status.spool.segments) + " 段 " +
                 std::to_string(status.spool.bytes) + " 字节 " +
                 std::to_string(status.spool.sealed_batches) + " 批; 最老段龄 " +
                 (status.spool.oldest_age_ms < 0
                      ? std::string("无")
                      : std::to_string(status.spool.oldest_age_ms / 1000) + "s"),
             "active 半段: " + std::to_string(status.spool.active_batches) + " 批" +
                 (status.spool.degraded ? " [降级: 磁盘帽]" : ""),
             "清理/确认删除走 /telemetry spool clear"});
        return CommandFlow::Continue;
    }

    if (sub == "consent") {
        if (ctx.telemetry_service == nullptr) {
            PrintTelemetryNotice(*ctx.theme, {"遥测未开启,consent 无从谈起"});
            return CommandFlow::Continue;
        }
        const auto& options = ctx.telemetry_service->options();
        if (words.size() >= 2 && words[1] == "grant") {
            if (!options.exporter.configured()) {
                PrintTelemetryNotice(*ctx.theme, {"没配 telemetry.exporter.endpoint,授权无从谈起"});
                return CommandFlow::Continue;
            }
            // §8.4 披露:授权前把六项摆给用户看(凭证只报名,不出值)。
            PrintTelemetryNotice(
                *ctx.theme,
                {"本次授权将放行向以下 endpoint 发送遥测:",
                 "endpoint/协议: " +
                     lubancode::telemetry::SanitizeEndpointForDisplay(options.exporter.endpoint),
                 "数据等级: " + std::string(lubancode::telemetry::DataClassName(options.data_class)),
                 "脱敏规则版本: " + std::string(lubancode::telemetry::kRedactionPolicyVersion),
                 "本地 spool: " + lubancode::tools::PathToUtf8(options.telemetry_root) +
                     " (容量帽 " + std::to_string(options.spool.total_bytes_cap) + " 字节)",
                 "凭证来源: " +
                     (options.exporter.secret_ref.empty()
                          ? std::string("匿名(未配 secret_ref)")
                          : ("环境变量 " + options.exporter.secret_ref + "(不展示值)")),
                 "远端策略: 未启用(T4 前,云端不下发策略)",
                 "resource attributes: service.version=" + options.resource.service_version +
                     ", frontend=" + options.resource.frontend + ", os.type=" +
                     (options.resource.os_type.empty() ? "(装配层未给)" : options.resource.os_type),
                 "endpoint/数据等级/脱敏版本任一变更须重新授权。"});
            if (ctx.telemetry_service->GrantConsent()) {
                PrintTelemetryNotice(*ctx.theme, {"consent 已记录,出口放行。"});
            } else {
                PrintTelemetryNotice(*ctx.theme, {"consent 落盘失败(目录权限?)"},
                                     frame::FieldAccent::Error);
            }
            return CommandFlow::Continue;
        }
        if (words.size() >= 2 && words[1] == "revoke") {
            const bool ok = ctx.telemetry_service->RevokeConsent();
            PrintTelemetryNotice(
                *ctx.theme,
                {ok ? "consent 已撤回;非回环出口立即停发(telemetry.consent_required)"
                    : "撤回时出问题,再试一次"});
            return CommandFlow::Continue;
        }
        PrintTelemetryNotice(
            *ctx.theme,
            {"consent 状态: " + ctx.telemetry_service->ConsentState() +
                 "(回环 endpoint 免披露;公网先 /telemetry consent grant)"});
        return CommandFlow::Continue;
    }

    if (sub == "policy") {
        PrintTelemetryNotice(*ctx.theme,
                             {"远端策略(telemetry.remote_policy)属 T4:签名策略、本地裁决、"
                              "越权拒绝尚未落地。当前本地配置即全部策略,云端不下发任何东西。"});
        return CommandFlow::Continue;
    }

    PrintTelemetryNotice(*ctx.theme,
                         {"/telemetry " + sub + ": 认不得。可用: status|enable|disable|pause|resume|"
                                                "flush|spool|consent|policy"},
                         frame::FieldAccent::Error);
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
