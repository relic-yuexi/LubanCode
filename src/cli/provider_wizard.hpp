// /provider add 向导式引导:裸敲 `/provider add`(或 `/provider add 名字`)
// 走这一套分步问答,跟 setup_wizard.cpp 的初次配置向导长得一个模子——同一套
// WizardIO 注入点(打印一行 / 读一行 / 拉模型列表),同一套 ReadTrimmed /
// ReadRequired / ReadChoice / ResolveModel 机器(见 cli/setup_wizard.hpp)。
//
// 跟初次配置向导的区别:
//   - 多问一步"名字"(命令行已经给了名字就跳过,否则问、校验、查重名);
//   - api_key 这一步允许直接留空,改问 key_env(环境变量名,默认
//     ANTHROPIC_AUTH_TOKEN),不像初次向导那样必填明文 key;
//   - 多问一步"推理档位"(model_reasoning_effort),可选,留空跳过;
//   - 最后落盘的是一条 ProviderConfig(写进 providers 数组),不是整份
//     Config(初次向导覆盖的是当下这一次会话直接要用的配置)。
//
// 逻辑写成纯函数 RunProviderAddWizard,除了 WizardIO 三个注入点不碰任何
// 真实 IO——生产代码(main.cpp)接 std::cout / cli::ReadLine / api::ListModels,
// 单测接假的打印收集器 / 脚本化输入序列 / 假的模型列表结果。

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "cli/setup_wizard.hpp"
#include "config/config.hpp"

namespace lubancode::cli {

struct ProviderWizardOutcome {
    config::ProviderConfig provider;
    bool save_requested = false;  // 最后一问"确认写入?"用户是否同意
};

// name_prefill:命令行已经给的名字(`/provider add 名字`),非空且校验
// 通过(不与 existing 重名、字符集合法)就跳过"名字"这一步;为空,或校验
// 没过,都会进入交互式的名字问答(校验没过时先打一行提示,说明原样输入
// 不能用,再往下问)。
// existing:当前已配的 providers,用来查重名(config::FindProvider)。
//
// 中途读到 EOF(read_line 返回 nullopt)按用户放弃处理,返回 std::nullopt。
// 最后一问用户回答 n/N(不确认)时,返回值仍然有值,但 save_requested ==
// false——调用方按"整个添加动作被取消"处理,不写盘、不改内存里的
// providers 列表。
std::optional<ProviderWizardOutcome> RunProviderAddWizard(WizardIO& io, const std::string& name_prefill,
                                                            const std::vector<config::ProviderConfig>& existing);

}  // namespace lubancode::cli
