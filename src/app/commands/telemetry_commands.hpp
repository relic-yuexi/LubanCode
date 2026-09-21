// /telemetry(端云协同可观测架构与 Telemetry 插件设计单 §24.2,实施分期
// T1 status 先行、T2 补 enable/disable/pause/resume/flush/spool/consent):
//   /telemetry [status]   只显示状态,不改变配置不发请求
//   enable|disable session|config  裸敲只给选项;用户明确选才动手
//                         (session = 当前进程;config = 写全局配置一枚布尔,
//                          项目配置永不暗改)
//   pause|resume          停/复出口(投影/spool 照常)
//   flush [毫秒]          seal + 有界赶发
//   spool [clear --confirm]  列账/两步确认删除
//   consent [grant|revoke]   §8.4 公网确认(回环免)
//   policy                T4 未落地,明说
#pragma once

#include "app/commands/command_flow.hpp"  // CommandFlow
#include "cli/slash_commands.hpp"          // ParsedSlashCommand

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::telemetry {
class TelemetryService;
}
namespace lubancode::cli {
struct Theme;
}

namespace lubancode::app {

// HC-06(材料收窄):/telemetry 的窄材料——handler 不再摸会话大上下文
// (SlashDispatchContext),材料由绑定单元在装配期折好递进来。
struct TelemetryCommandContext {
    lubancode::telemetry::TelemetryService* telemetry_service = nullptr;  // 空 = 遥测未开
    // /telemetry enable session(端云协同可观测单 T2,§24.2):当前进程内装
    // 遥测服务的执行体。空 = 没接(非交互装配),命令面明说接不上。
    std::function<std::vector<std::string>()> enable_telemetry_session;
    // 全局配置文件现行路径(enable/disable config 写 features.telemetry 用;
    // 空 = 还没建过配置,回落 <主目录>/.lubancode/config.json)。
    const std::optional<std::string>* global_config_file_path = nullptr;
    const lubancode::cli::Theme* theme = nullptr;
};

CommandFlow HandleSlashTelemetry(const TelemetryCommandContext& ctx,
                                 const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
