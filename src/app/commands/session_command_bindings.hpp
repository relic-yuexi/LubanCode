// HC-06(把Slash命令材料收窄到各自领域)命令绑定单元——集中构造"捕获
// 材料的执行器"。路由表(command_registry)只接命令参数,各域 handler 只
// 吃自己的窄材料;尚未收窄的域仍经 SlashDispatchContext 过渡(后续批次
// 逐域迁走,最后删掉整束公共材料)。第一小批迁 Trace/Hook/Telemetry,
// 第二小批迁 Model/Memory/Usage/Package。
//
// 分层规矩:
//   - 绑定材料全借用(指针/回调),会话控制器构造尾一次配齐;表里的闭包
//     捕获这些借用,命令执行期间由控制器/组合根保活,与旧
//     SlashDispatchContext 同一条借用纪律;
//   - 表的案序、死案口径与旧 switch 登册一致(command_registry 的对账
//     测试钉着),这里只换"handler 从函数指针吃大上下文"为"闭包吃窄
//     材料",不改任何命令行为;
//   - 命令词汇(名字/别名/展示标记)不在这层——统一看 cli::
//     SlashCommandDescriptors()(HC-05)。
#pragma once

#include <vector>

#include "app/commands/command_flow.hpp"
#include "app/commands/command_registry.hpp"    // SlashCommandSpec/SlashDispatchContext
#include "app/commands/hook_commands.hpp"       // HookCommandContext
#include "app/commands/memory_commands.hpp"     // MemoryCommandContext
#include "app/commands/model_commands.hpp"      // ModelCommandContext
#include "app/commands/package_commands.hpp"    // PackageCommandContext
#include "app/commands/telemetry_commands.hpp"  // TelemetryCommandContext
#include "app/commands/trace_commands.hpp"      // TraceCommandContext
#include "app/commands/usage_commands.hpp"      // UsageCommandContext
#include "cli/slash_commands.hpp"

namespace lubancode::app {

// 绑定材料:会话控制器构造尾一次配齐(全借用)。
struct SessionCommandMaterials {
    // 尚未收窄域的过渡材料袋(HC-06 批3 逐域搬走后删除;绑定期
    // 不可为空,对账钉子 SlashCommandTable() 喂空对象)。
    SlashDispatchContext* dispatch = nullptr;
    // 已收窄域的窄材料(HC-06 第一小批:Trace/Hook/Telemetry;第二小批:
    // Model/Memory/Usage/Package)。Model 的三闭包(跨家切换/活清单/同步)
    // 在组合根折好随材料进来。
    TraceCommandContext trace{};
    HookCommandContext hook{};
    TelemetryCommandContext telemetry{};
    ModelCommandContext model{};
    MemoryCommandContext memory{};
    UsageCommandContext usage{};
    PackageCommandContext package{};
};

// 构造整张命令表(58 案按旧 switch 案序登册):已收窄域的行捕获窄材料,
// 过渡域的行包装 SlashDispatchContext 上的既有分派位。行为零变——
// 换的只是"材料怎么递",不是"命令做什么"。
std::vector<SlashCommandSpec> BuildSessionSlashCommandTable(const SessionCommandMaterials& materials);

}  // namespace lubancode::app
