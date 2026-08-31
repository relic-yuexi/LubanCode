// /telemetry 的实现。合同见 telemetry_commands.hpp 文件头。
//
// 本地面纪律(§8.5/§24.2):遥测没开就明说没开,不触网、不建目录、
// 不起任何副作用——这只命令只读 Status()。
#include "app/commands/telemetry_commands.hpp"

#include <string>

#include "cli/terminal_port.hpp"

namespace lubancode::app {
namespace {
using lubancode::cli::TermOut;
}  // namespace


CommandFlow HandleSlashTelemetry(SlashDispatchContext& ctx,
                                 const lubancode::cli::ParsedSlashCommand& parsed) {
    // 子命令拆词(与 /agent 同款:首词后全部当参数)。
    std::string sub = parsed.args;
    const std::size_t space = parsed.args.find_first_of(" \t");
    if (space != std::string::npos) {
        sub = parsed.args.substr(0, space);
    }
    if (!sub.empty() && sub != "status") {
        // §24.2 完整命令族(enable/disable/pause/resume/flush/spool/
        // consent/policy)按分期属 T2;T1 不装样子。
        TermOut() << ctx.theme->stats
                  << "/telemetry " << sub
                  << ": 本批未实现(状态面先行;enable/flush/spool 等随 T2 "
                     "exporter 一起落)"
                  << ctx.theme->reset << "\n";
        return CommandFlow::Continue;
    }
    if (ctx.telemetry_service == nullptr) {
        // 未装配 = 激活判定非 Active(默认关闭/环境变量关/总闸/缺前置)。
        TermOut() << ctx.theme->stats
                  << "遥测未开启(features.telemetry 默认关;开启须与 "
                     "features.trajectory 同开)"
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

}  // namespace lubancode::app
