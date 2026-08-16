// app 层的 runtime profile 装配(规格"子代理与 MainAgent 同级"根因一):
// 把 Config(四级合并结果)+ provider 声明(镜像进 Config)+ 模型目录
// (models.json / 内置目录)折成一份 agent::AgentRuntimeProfile,main、
// 子代理、单发模式共用同一只装配函数,不各拼各的——"main 与
// general-purpose agent 构造出的 effective max_output_tokens 相同"这条
// 测试钉的就是这两个函数的输出。

#pragma once

#include <string>

#include "agent/runtime_profile.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"

namespace lubancode::app {

// main 的 profile:输出上限走三级解析(config agent 段 > provider 声明 >
// 模型目录条目),步数/上下文/窗口从 config 来,length 续跑从 agent 段来。
// catalog 传空指针 = 没有目录(空目录也行,条目查不到同样是"目录没声明")。
agent::AgentRuntimeProfile BuildMainRuntimeProfile(const config::Config& config,
                                                    const config::ModelCatalog* catalog,
                                                    const std::string& current_model);

// 子代理的默认 profile:整份继承 main 的有效值(默认同级,不暗自缩小)。
// 唯一的合法改写是用户显式写的 subagent.max_output_tokens——那是主动
// 收窄/放宽,来源标 ConfigFile 并在展示里注明"subagent 段"。步数上限
// 的 subagent 覆盖走既有管道(AgentTool 构造参数),这里不动。
agent::AgentRuntimeProfile BuildSubagentRuntimeProfile(const agent::AgentRuntimeProfile& main_profile,
                                                        const config::Config& config);

// 来源枚举 -> 展示句(/config、/context、agent 查看态共用,i18n 层在这头
// 之上再翻)。subagent_override 为真时句尾追加 subagent 段注记。
std::string OutputBudgetSourceText(agent::OutputBudgetSource source, bool subagent_override);

}  // namespace lubancode::app
