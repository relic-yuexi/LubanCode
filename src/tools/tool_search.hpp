// tool_search:工具延迟挂载(思路照 Claude Code 的 ToolSearch)。
//
// 背景:MCP/插件一多,请求的 tools 数组跟着膨胀——每个工具的完整
// description + input_schema 每轮都发一遍,吃掉大量上下文。对策是分流:
//   - 核心工具(内置九件套 + web_fetch/web_search,Tool::deferred()==false)
//     恒在,照旧全量直挂;
//   - 延迟工具(McpTool 经 DeferredTool 包装、PluginTool、LuaTool,
//     deferred()==true)不进 tools 数组,只在系统提示的紧凑索引段里露
//     "名字 + 一句截断的描述";模型用 tool_search 按关键词检索。
//
// 命中之后怎么走,看 deferred_tool_mode(动态工具 PromptCache 守恒单 §四):
//   - legacy_expand(默认/兼容):命中进 loaded 集合,下一轮完整 schema
//     扩写回顶层 tools+system——断前缀,cache-hostile,P0 回归册钉着现状;
//   - proxy_reference(P1):命中只把结构化 schema/tool_ref 放进 tool result
//     追加到历史尾部,顶层 tools 恒为 core+tool_search+tool_invoke,前缀
//     缓存不断;调用走 tool_invoke,由 AgentLoop 解引用后过 RunOneTool 正门;
//   - native_reference(P3,anthropic wire + 目录声明才开):本地不挂这两枚
//     工具——发现由 provider 的服务端 tool_search_tool_regex/bm25 做,延迟
//     定义照发但标 defer_loading,模型发现后直接调用真实工具,本地照走
//     RunOneTool 正门(接线在 ToolRuntime/装配层,不在本文件)。
//
// 阈值开关:注册表总工具数 ≤ 阈值(config.tool_search_threshold,默认 20,
// 0 = 永不延迟)时一切照旧,现状行为零变化;超阈值才启用。P4 起多一道
// token 预算闸(config.tool_search_token_floor,默认非 0):枚数过了、延迟
// 工具的声明 token 本金不够,也不启用——本金太小省不出固定开销,P0
// baseline 册轻 schema 形状实测启用反赔。两道闸与 loaded 集合都由装配层
// 持有(会话级,/clear 不清——工具挂载与对话历史无关),这里只提供纯逻辑:
// 判定、索引段、检索工具、延迟包装。
#pragma once

#include <cstddef>
#include <memory>
#include <set>
#include <string>

#include "tools/deferred_tool_resolver.hpp"  // DeferredToolMode/DeferredToolResolver:proxy 路的账与解析
#include "tools/registry.hpp"
#include "tools/tool.hpp"

namespace lubancode::tools {

// 阈值判定:threshold=0 永不延迟;总数(不含 tool_search 自身,调用方在
// 注册 tool_search 之前数)严格大于阈值才启用。纯函数,好单测。
inline bool DeferralEnabled(std::size_t total_tools, int threshold) {
    return threshold > 0 && total_tools > static_cast<std::size_t>(threshold);
}

// token 预算门(动态工具 PromptCache 守恒单 P4·§十三 P4-1):枚数门之外的
// 第二道闸。依据只引 P0 baseline 册的实测(tests/unit/tools/test_tool_search.cpp
// "P0基线"三册):轻 schema/长描述的工具形状,越阈值启用延迟后首份请求
//(tools+索引段)反而比全量常驻贵——索引段把长描述原样照抄(不超 80 字
// 不截断),省出的小 schema 抵不过 tool_search 自身定义与索引段前言的固定
// 开销;重 schema/短描述形状本金大才有真节省。枚数口径(默认 20)分不出
// 这两种形状,所以补一道"延迟工具全量常驻的声明 token 本金"下限:本金
// < floor 时启用必赔,不如全量常驻(disabled)。floor=0 关掉这道门,只看
// 枚数(P4 之前的现状)。本金数与 floor 都由调用方递——EstimateUtf8Tokens
// 在 agent 层,tools 层不反向引。
inline bool DeferralBudgetOk(std::size_t deferred_decl_tokens, int token_floor) {
    return token_floor <= 0 || deferred_decl_tokens >= static_cast<std::size_t>(token_floor);
}

// 组合判定(P4-1):枚数门与预算门都开才启用。枚数默认 20 不动——baseline
// 册没有支持改枚数的实测数(它证明的恰是枚数口径不分辨工具形状),重定
// 落在"换 token-aware 决策"这一路。
inline bool ShouldDeferTools(std::size_t total_tools, int threshold, std::size_t deferred_decl_tokens,
                             int token_floor) {
    return DeferralEnabled(total_tools, threshold) && DeferralBudgetOk(deferred_decl_tokens, token_floor);
}

// P0(动态工具 PromptCache 守恒单·§十三):/context 与 trace 的
// deferred_tool_mode 展示位。P1 起 proxy_reference 落地,标签按模式给;
// bool 版保留给 P0 的既有调用(等价于 disabled/legacy_expand 两档)。
inline std::string DeferredToolModeLabel(bool deferral_enabled) {
    return deferral_enabled ? "legacy_expand" : "disabled";
}

// P1:模式版标签。deferral 关着时无论配了什么都是 disabled(没有延迟工具
// 就没有模式可言);P3 起 native_reference 落地(发现走 provider 服务端
// 搜索,本地不挂 tool_search/tool_invoke),装配层过完两道门才递它进来。
inline std::string DeferredToolModeLabel(DeferredToolMode mode, bool deferral_enabled) {
    if (!deferral_enabled) {
        return "disabled";
    }
    return DeferredToolModeName(mode);
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
    ApprovalClass approval_class() const override { return inner_->approval_class(); }
    bool deferred() const override { return true; }
    // 逐枚追踪单:注册元数据透传内层的回答,不把来源洗成 deferred——
    // trace 里 MCP 工具就是 MCP,延迟挂载只是宿主的工具表策略,不是
    // 工具的身份。
    EffectClass effect_class() const override { return inner_->effect_class(); }
    Idempotency idempotency() const override { return inner_->idempotency(); }
    RecoveryCapability recovery_capability() const override { return inner_->recovery_capability(); }
    std::string version_or_digest() const override { return inner_->version_or_digest(); }
    Result execute(const nlohmann::json& input) override { return inner_->execute(input); }
    // 取消旗透传内层(子代理 x 停止失效单):包装不许把 context 洗掉——
    // 延迟挂载只是工具表策略,取消源仍要跟到真执行的那枚工具。
    Result execute(const nlohmann::json& input, const ToolExecutionContext& context) override {
        return inner_->execute(input, context);
    }

private:
    std::unique_ptr<Tool> inner_;
};

// 系统提示里的紧凑索引段:列出"延迟且尚未加载"的工具,每个一行
// "- 名字: 描述(截断到 80 个字符,UTF-8 按码点数)"。不含 schema——
// 索引只是让模型知道"有这么个东西、该拿什么词去搜"。一个都没有(没启用
// 延迟、或全加载了)返回空串,调用方不注段,一个字符都不多。
std::string BuildDeferredToolsIndexSegment(const ToolRegistry& registry, const std::set<std::string>& loaded);

// tool_search 工具本体。两副构造,对应单子 §四的两条活路:
//   - legacy 构造(registry + loaded):命中即写 loaded 集合,下一轮 schema
//     扩写回顶层 tools——兼容路,cache-hostile,明标不洗白。
//   - proxy 构造(registry + resolver):命中只把结构化 schema/ref 放进
//     tool result 追加到历史尾部,不碰 loaded、不碰顶层 tools(单子 §5.3)。
// 搜索本身只读 catalog,不授予权限、不执行目标工具——发现不等于授权。
// registry 的生命周期由装配层(main.cpp/ToolRuntime)的声明顺序保证
// (工具就注册在这张表里,表活着工具就活着)。
class ToolSearchTool : public Tool {
public:
    ToolSearchTool(const ToolRegistry& registry, std::shared_ptr<std::set<std::string>> loaded)
        : registry_(registry), loaded_(std::move(loaded)) {}

    ToolSearchTool(const ToolRegistry& registry, std::shared_ptr<DeferredToolResolver> resolver)
        : registry_(registry), resolver_(std::move(resolver)) {}

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    Result execute(const nlohmann::json& input) override;

private:
    Result ExecuteLegacy(const nlohmann::json& input);
    Result ExecuteProxy(const nlohmann::json& input);

    const ToolRegistry& registry_;
    std::shared_ptr<std::set<std::string>> loaded_;          // legacy 路:命中写挂载
    std::shared_ptr<DeferredToolResolver> resolver_;         // proxy 路:命中铸 ref
};

// tool_invoke 的固定 wire 定义(单子 §5.4)。顶层 schema 一个字节不变:
// {tool_ref: string, arguments: object};arguments 的细校验不靠这层宽
// schema——宿主解引用后拿目标工具当下那份真 schema 再验(单子 §5.5)。
//
// 注意:这个 execute() 不是执行口!AgentLoop 收到 tool_invoke 调用后先规范
// 化成真实目标调用、只对目标走一次 RunOneTool(单子 §6.1);直接调到这只
// 壳(PTC/未接规范化的入口)会得到稳定拒绝,绝不在壳里 target->execute()
// ——那会绕过确认、Hook、取消与 Trace,养出第二条执行暗道(单子红线 5)。
class ToolInvokeTool : public Tool {
public:
    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    Result execute(const nlohmann::json& input) override;
};

}  // namespace lubancode::tools
