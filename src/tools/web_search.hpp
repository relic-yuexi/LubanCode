// web_search 工具:调第三方搜索 API(tavily / brave / serper 三家之一),
// 统一输出编号列表(标题/URL/摘要)。走哪家、用什么钥匙由 config 的
// search 段决定——没配这一段,这个工具压根不注册(见 main.cpp),模型
// 看不见,不会瞎调。api_key 只进请求头/请求体,不写日志、不进返回文本。
//
// 探路结论(2026-07 实测):MiniMax 的 Anthropic 兼容层不支持服务端
// web_search_20250305 工具——请求不报错,但它把这个声明偷换成自家
// plugin_web_search 客户端工具吐 tool_use 块,没有 server_tool_use /
// web_search_tool_result,服务端不代跑搜索。所以走这条客户端 fallback 路线。

#pragma once

#include <expected>
#include <memory>
#include <optional>
#include <string>

#include "config/config.hpp"
#include "tools/tool.hpp"
#include "net/http_transport.hpp"

namespace lubancode::tools {

// ---- 三家响应的解析,拆成纯函数好单测(fixture 手造)。入参是响应体
// 原文,出参是编号列表文本;JSON 坏了/结构不对返回错误说明。 ----

// tavily: {"results": [{"title": ..., "url": ..., "content": ...}, ...]}
std::expected<std::string, std::string> ParseTavilyResponse(const std::string& body);

// brave: {"web": {"results": [{"title": ..., "url": ..., "description": ...}, ...]}}
std::expected<std::string, std::string> ParseBraveResponse(const std::string& body);

// serper: {"organic": [{"title": ..., "link": ..., "snippet": ...}, ...]}
std::expected<std::string, std::string> ParseSerperResponse(const std::string& body);

// count 参数归一:没给用默认 5,给了夹到 [1, 10]。
int ClampSearchCount(int requested);

struct WebSearchOptions {
    config::SearchConfig search;
    std::string endpoint;
    net::FullHttpLimits limits;
    std::uint64_t max_output_bytes = 100 * 1024;
    std::size_t max_query_bytes = 4096;
    int max_results = 10;
};
std::expected<void, std::string> ValidateWebSearchOptions(const WebSearchOptions&);
// One bounded authenticated request, no redirects or retained cancellation.
class WebSearchTransport {
public:
    virtual ~WebSearchTransport() = default;
    virtual std::expected<net::FullHttpResponse, net::FullHttpError> Send(
        const net::FullHttpRequest&, const net::FullHttpLimits&, const std::atomic<bool>*) = 0;
};

class WebSearchTool : public Tool {
public:

    // 逐枚追踪单:注册元数据声明。
    lubancode::tools::EffectClass effect_class() const override { return lubancode::tools::EffectClass::ReadOnlyRemote; }
    explicit WebSearchTool(config::SearchConfig search);
    explicit WebSearchTool(WebSearchOptions options, std::shared_ptr<WebSearchTransport> transport = {});

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    Result execute(const nlohmann::json& input) override;
    Result execute(const nlohmann::json& input, const ToolExecutionContext& context) override;

private:
    config::SearchConfig search_;
    std::optional<WebSearchOptions> options_;
    std::shared_ptr<WebSearchTransport> transport_;
};

}  // namespace lubancode::tools
