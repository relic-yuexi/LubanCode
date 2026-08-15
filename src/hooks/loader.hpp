// 配置 -> 运行期定义。config 层解析出的 HooksConfig(合并后,user 与
// project 的条目都带着各自的 source_path)在这里升级成 HookDefinition:
// 定来源、算 definition hash、对 project 来源过信任审查(未信任的不删,
// 标记 trusted=false——dispatcher 跳过并在记录里留痕,信任后即生效)。
#pragma once

#include <string>
#include <vector>

#include "config/config.hpp"
#include "hooks/trust.hpp"
#include "hooks/types.hpp"

namespace lubancode::hooks {

struct LoadedHooks {
    std::vector<HookDefinition> definitions;
    // 有未信任的项目 hook(启动提示用:指路 /hooks)。
    bool has_untrusted_project = false;
    // 有被禁用的 hook(/hooks 里看得见,这里只作提示)。
    bool has_disabled = false;

    bool Empty() const { return definitions.empty(); }
};

// definition hash 的规范串:hash 覆盖"跑什么"(type/command/args/windows
// 变体/timeout/async/failure_policy),不覆盖"何时跑"(event/matcher)——
// 命令、参数、脚本路径、handler 类型、超时、async 任一改,hash 变,project
// 信任失效须重审;matcher 与事件只管路由,不进 hash。
std::string ComputeDefinitionHash(const config::HookHandlerConfig& handler);

// 便利入口:definition 的展示串(exec form 或 shell 串),/hooks 与运行
// 记录共用。
std::string HookCommandDisplay(const config::HookHandlerConfig& handler);

// config_result.config.hooks(已合并、条目带 source_path)+ 信任账 ->
// 定义列表。project_config_path/global_config_path 用来分级;条目
// source_path 与两者都对不上时(理论不该发生),按"在当前目录下 = 项目"
// 保守分级。cwd 用于 ${LUBANCODE_PROJECT_DIR} 占位符替换与保守分级。
// 纯函数除信任账的读写(IsTrusted/IsDisabled 查询),不起任何进程。
LoadedHooks LoadHookDefinitions(const config::HooksConfig& hooks,
                                const std::optional<std::string>& project_config_path,
                                const std::optional<std::string>& global_config_path,
                                const std::string& cwd, const HookTrustStore& trust);

}  // namespace lubancode::hooks
