// tool_search:工具延迟挂载(思路照 Claude Code 的 ToolSearch)。
//
// 背景:MCP/插件一多,请求的 tools 数组跟着膨胀——每个工具的完整
// description + input_schema 每轮都发一遍,吃掉大量上下文。对策是分流:
//   - 核心工具(内置九件套 + web_fetch/web_search,Tool::deferred()==false)
//     恒在,照旧全量直挂;
//   - 延迟工具(McpTool 经 DeferredTool 包装、PluginTool、LuaTool,
//     deferred()==true)不进 tools 数组,只在系统提示的紧凑索引段里露
//     "名字 + 一句截断的描述";模型用 tool_search 按关键词检索,命中即
//     挂载(进 loaded 集合),下一轮请求它们的完整 schema 就在 tools 里了。
//
// 阈值开关:注册表总工具数 ≤ 阈值(config.tool_search_threshold,默认 20,
// 0 = 永不延迟)时一切照旧,现状行为零变化;超阈值才启用。启用与否、
// loaded 集合都由 main.cpp 持有(会话级,/clear 不清——工具挂载与对话
// 历史无关),这里只提供纯逻辑:判定、索引段、检索工具、延迟包装。
#pragma once

#include <cstddef>
#include <memory>
#include <set>
#include <string>

#include "tools/registry.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

// 阈值判定:threshold=0 永不延迟;总数(不含 tool_search 自身,调用方在
// 注册 tool_search 之前数)严格大于阈值才启用。纯函数,好单测。
inline bool DeferralEnabled(std::size_t total_tools, int threshold) {
    return threshold > 0 && total_tools > static_cast<std::size_t>(threshold);
}

// 延迟包装:把任意工具标成 deferred=true,其余行为原样转发。给 McpTool 用
// ——mcp/ 目录不动(任务规矩),没法直接在 McpTool 上加 override,main.cpp
// 注册 MCP 工具时裹一层这个。PluginTool/LuaTool 在 tools/ 里,直接 override,
// 不用裹。
class DeferredTool : public Tool {
public:
    explicit DeferredTool(std::unique_ptr<Tool> inner) : inner_(std::move(inner)) {}

    std::string name() const override { return inner_->name(); }
    std::string description() const override { return inner_->description(); }
    nlohmann::json input_schema() const override { return inner_->input_schema(); }
    bool needs_confirm() const override { return inner_->needs_confirm(); }
    bool deferred() const override { return true; }
    // 逐枚追踪单:注册元数据透传内层的回答,不把来源洗成 deferred——
    // trace 里 MCP 工具就是 MCP,延迟挂载只是宿主的工具表策略,不是
    // 工具的身份。
    EffectClass effect_class() const override { return inner_->effect_class(); }
    Idempotency idempotency() const override { return inner_->idempotency(); }
    RecoveryCapability recovery_capability() const override { return inner_->recovery_capability(); }
    std::string version_or_digest() const override { return inner_->version_or_digest(); }
    Result execute(const nlohmann::json& input) override { return inner_->execute(input); }

private:
    std::unique_ptr<Tool> inner_;
};

// 系统提示里的紧凑索引段:列出"延迟且尚未加载"的工具,每个一行
// "- 名字: 描述(截断到 80 个字符,UTF-8 按码点数)"。不含 schema——
// 索引只是让模型知道"有这么个东西、该拿什么词去搜"。一个都没有(没启用
// 延迟、或全加载了)返回空串,调用方不注段,一个字符都不多。
std::string BuildDeferredToolsIndexSegment(const ToolRegistry& registry, const std::set<std::string>& loaded);

// tool_search 工具本体。持有注册表引用(检索它里面的延迟工具)和 loaded
// 集合的 shared_ptr(命中即写入——main.cpp 持同一份,主会话/子代理共享,
// 挂载一次两边可用)。registry 的生命周期由 main.cpp 的声明顺序保证
// (工具就注册在这张表里,表活着工具就活着)。
class ToolSearchTool : public Tool {
public:
    ToolSearchTool(const ToolRegistry& registry, std::shared_ptr<std::set<std::string>> loaded)
        : registry_(registry), loaded_(std::move(loaded)) {}

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    Result execute(const nlohmann::json& input) override;

private:
    const ToolRegistry& registry_;
    std::shared_ptr<std::set<std::string>> loaded_;
};

}  // namespace lubancode::tools
