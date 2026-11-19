#include "tools/web_search.hpp"

#include <string>
#include <utility>
#include <algorithm>
#include <memory>
#include <curl/curl.h>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/
#include "tools/web_fetch.hpp"

namespace lubancode::tools {

namespace {

using nlohmann::json;

constexpr int kDefaultCount = 5;
constexpr int kMaxCount = 10;

std::expected<std::string, std::string> SearchEndpoint(const WebSearchOptions& options) {
    std::string value = options.endpoint;
    if (value.empty()) {
        if (options.search.provider == "tavily") value = "https://api.tavily.com/search";
        else if (options.search.provider == "brave") value = "https://api.search.brave.com/res/v1/web/search";
        else if (options.search.provider == "serper") value = "https://google.serper.dev/search";
        else return std::unexpected("invalid_options");
    }
    if (value.size() > 8192 || !platform::IsValidUtf8(value) ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; }))
        return std::unexpected("invalid_endpoint");
    cpr::Session initialized;
    if (!initialized.GetCurlHolder() || !initialized.GetCurlHolder()->handle)
        return std::unexpected("transport_unavailable");
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, value.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK)
        return std::unexpected("invalid_endpoint");
    const auto part = [&](CURLUPart kind) {
        char* raw = nullptr;
        const auto status = curl_url_get(url.get(), kind, &raw, 0);
        std::unique_ptr<char, decltype(&curl_free)> held(raw, curl_free);
        return status == CURLUE_OK && raw ? std::string(raw) : std::string();
    };
    const auto scheme = part(CURLUPART_SCHEME);
    auto host = part(CURLUPART_HOST);
    if (host.starts_with('[') && host.ends_with(']')) host = host.substr(1, host.size() - 2);
    if ((scheme != "https" && !(scheme == "http" && net::IsLoopbackAddress(host))) ||
        !part(CURLUPART_QUERY).empty() || !part(CURLUPART_FRAGMENT).empty() ||
        value.find('?') != std::string::npos || value.find('#') != std::string::npos)
        return std::unexpected("invalid_endpoint");
    return part(CURLUPART_URL);
}

std::string EncodeQuery(const std::string& query) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : query) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') out += c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

class DefaultSearchTransport final : public WebSearchTransport {
public:
    std::expected<net::FullHttpResponse, net::FullHttpError> Send(
        const net::FullHttpRequest& request, const net::FullHttpLimits& limits,
        const std::atomic<bool>* cancel) override {
        return net::PerformFullHttpRequest(request, limits, cancel, nullptr);
    }
};

// 从一个结果项里安全取字符串字段,缺了/类型不对给空串——搜索结果偶尔
// 缺摘要,不至于整个解析报废。
std::string GetStringField(const json& item, const char* key) {
    if (auto it = item.find(key); it != item.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return std::string();
}

// 统一的编号列表拼装:标题一行,URL 一行,摘要一行(空摘要不占行)。
std::string FormatResults(const json& items, const char* title_key, const char* url_key, const char* snippet_key) {
    std::string out;
    int index = 0;
    for (const auto& item : items) {
        if (!item.is_object()) {
            continue;
        }
        ++index;
        const std::string title = GetStringField(item, title_key);
        const std::string url = GetStringField(item, url_key);
        const std::string snippet = GetStringField(item, snippet_key);
        out += std::to_string(index) + ". " + (title.empty() ? "(无标题)" : title) + "\n";
        out += "   " + (url.empty() ? "(无 URL)" : url) + "\n";
        if (!snippet.empty()) {
            out += "   " + snippet + "\n";
        }
    }
    if (index == 0) {
        return std::string();
    }
    return out;
}

std::expected<json, std::string> ParseJsonBody(const std::string& body, const char* provider) {
    try {
        return json::parse(body);
    } catch (const json::parse_error& e) {
        return std::unexpected(std::string(provider) + " 响应不是合法 JSON: " + e.what());
    }
}

}  // namespace

std::expected<std::string, std::string> ParseTavilyResponse(const std::string& body) {
    auto parsed = ParseJsonBody(body, "tavily");
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    if (!parsed->is_object() || !parsed->contains("results") || !(*parsed)["results"].is_array()) {
        return std::unexpected("tavily 响应里没有 results 数组");
    }
    const std::string out = FormatResults((*parsed)["results"], "title", "url", "content");
    if (out.empty()) {
        return std::string("没有搜到结果。");
    }
    return out;
}

std::expected<std::string, std::string> ParseBraveResponse(const std::string& body) {
    auto parsed = ParseJsonBody(body, "brave");
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    if (!parsed->is_object() || !parsed->contains("web") || !(*parsed)["web"].is_object() ||
        !(*parsed)["web"].contains("results") || !(*parsed)["web"]["results"].is_array()) {
        return std::unexpected("brave 响应里没有 web.results 数组");
    }
    const std::string out = FormatResults((*parsed)["web"]["results"], "title", "url", "description");
    if (out.empty()) {
        return std::string("没有搜到结果。");
    }
    return out;
}

std::expected<std::string, std::string> ParseSerperResponse(const std::string& body) {
    auto parsed = ParseJsonBody(body, "serper");
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    if (!parsed->is_object() || !parsed->contains("organic") || !(*parsed)["organic"].is_array()) {
        return std::unexpected("serper 响应里没有 organic 数组");
    }
    const std::string out = FormatResults((*parsed)["organic"], "title", "link", "snippet");
    if (out.empty()) {
        return std::string("没有搜到结果。");
    }
    return out;
}

int ClampSearchCount(int requested) {
    if (requested < 1) {
        return 1;
    }
    if (requested > kMaxCount) {
        return kMaxCount;
    }
    return requested;
}

WebSearchTool::WebSearchTool(config::SearchConfig search) : search_(std::move(search)) {}

std::expected<void, std::string> ValidateWebSearchOptions(const WebSearchOptions& options) {
    const auto& limits = options.limits;
    if ((options.search.provider != "tavily" && options.search.provider != "brave" && options.search.provider != "serper") ||
        options.search.api_key.empty() || options.search.api_key.size() > 4096 ||
        std::any_of(options.search.api_key.begin(), options.search.api_key.end(),
                    [](unsigned char c) { return c <= 0x20 || c > 0x7e; }) ||
        limits.connect_timeout_ms <= 0 || limits.connect_timeout_ms > 30000 ||
        limits.hard_timeout_ms <= 0 || limits.hard_timeout_ms > 120000 ||
        limits.connect_timeout_ms > limits.hard_timeout_ms ||
        limits.response_header_bytes <= 0 || limits.response_header_bytes > 512 * 1024 ||
        limits.response_body_bytes <= 0 || limits.response_body_bytes > 8 * 1024 * 1024 ||
        options.max_output_bytes < 256 || options.max_output_bytes > 1024 * 1024 ||
        options.max_query_bytes == 0 || options.max_query_bytes > 8192 ||
        options.max_results < 1 || options.max_results > 10)
        return std::unexpected("invalid_options");
    try {
        const auto endpoint = SearchEndpoint(options);
        if (!endpoint) return std::unexpected(endpoint.error());
    } catch (...) { return std::unexpected("invalid_endpoint"); }
    return {};
}

WebSearchTool::WebSearchTool(WebSearchOptions options, std::shared_ptr<WebSearchTransport> transport)
    : search_(options.search), options_(std::move(options)),
      transport_(transport ? std::move(transport) : std::make_shared<DefaultSearchTransport>()) {}

std::string WebSearchTool::name() const {
    return "web_search";
}

std::string WebSearchTool::description() const {
    // 文案在 src/prompts/tools/<语言>/web_search.md,兜底是迁移前的原文。
    return ToolText("web_search", "description",
                    "网络搜索,返回编号列表(标题/URL/摘要)。适合查最新资讯、找文档地址;拿到 URL 之后"
                    "用 web_fetch 抓正文。需要搜好几轮、读好几篇再总结的活,交给 agent 子代理去干。");
}

nlohmann::json WebSearchTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json query_prop = nlohmann::json::object();
    query_prop["type"] = "string";
    query_prop["description"] = ToolText("web_search", "param.query", "搜索关键词或问题");
    properties["query"] = query_prop;

    nlohmann::json count_prop = nlohmann::json::object();
    count_prop["type"] = "integer";
    count_prop["description"] = ToolText("web_search", "param.count", "想要几条结果,不填默认 5,上限 10");
    properties["count"] = count_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"query"});

    return schema;
}

Tool::Result WebSearchTool::execute(const nlohmann::json& input) {
    if (options_) return execute(input, ToolExecutionContext{});
    if (!input.contains("query") || !input.at("query").is_string()) {
        return {"缺少必填参数 query(字符串)", true};
    }
    const std::string query = input.at("query").get<std::string>();
    if (query.empty()) {
        return {"query 不能是空字符串", true};
    }

    int count = kDefaultCount;
    if (auto it = input.find("count"); it != input.end() && !it->is_null()) {
        count = ClampSearchCount(static_cast<int>(it->get<long long>()));
    }

    // 三家 API 的请求各拼各的。注意:api_key 只进请求头/请求体,报错文本里
    // 绝不带它。
    cpr::Response response;
    if (search_.provider == "tavily") {
        const json body = {{"query", query}, {"max_results", count}};
        response = cpr::Post(cpr::Url{"https://api.tavily.com/search"},
                              cpr::Header{{"Content-Type", "application/json"},
                                          {"Authorization", "Bearer " + search_.api_key}},
                              cpr::Body{body.dump()}, cpr::Timeout{30000});
    } else if (search_.provider == "brave") {
        response = cpr::Get(cpr::Url{"https://api.search.brave.com/res/v1/web/search"},
                             cpr::Parameters{{"q", query}, {"count", std::to_string(count)}},
                             cpr::Header{{"Accept", "application/json"},
                                         {"X-Subscription-Token", search_.api_key}},
                             cpr::Timeout{30000});
    } else if (search_.provider == "serper") {
        const json body = {{"q", query}, {"num", count}};
        response = cpr::Post(cpr::Url{"https://google.serper.dev/search"},
                              cpr::Header{{"Content-Type", "application/json"},
                                          {"X-API-KEY", search_.api_key}},
                              cpr::Body{body.dump()}, cpr::Timeout{30000});
    } else {
        // 正常流程到不了这儿(不配 search 段就不注册这个工具,配了的话
        // ParseSearchConfig 只放行三家),留个兜底防御手滑构造。
        return {"search.provider 不认识: " + search_.provider, true};
    }

    if (response.error) {
        return {"搜索请求失败: " + response.error.message, true};
    }
    const int status = static_cast<int>(response.status_code);
    if (status < 200 || status >= 300) {
        return {"搜索服务(" + search_.provider + ")返回 HTTP " + std::to_string(status) +
                    ",检查一下 search.api_key 配得对不对、额度够不够",
                true};
    }

    std::expected<std::string, std::string> formatted =
        search_.provider == "tavily"  ? ParseTavilyResponse(response.text)
        : search_.provider == "brave" ? ParseBraveResponse(response.text)
                                       : ParseSerperResponse(response.text);
    if (!formatted.has_value()) {
        return {formatted.error(), true};
    }
    return {*formatted, false};
}

Tool::Result WebSearchTool::execute(const nlohmann::json& input, const ToolExecutionContext& context) {
    if (!options_) return execute(input); // Legacy CLI request semantics remain unchanged.
    const auto fail = [](const std::string& reason) {
        Tool::Result result{"web_search." + reason, true};
        result.error_code = "web_search." + reason;
        return result;
    };
    const auto cancelled = [&] { return context.cancel && context.cancel->load(std::memory_order_acquire); };
    if (cancelled()) return fail("cancelled");
    if (!ValidateWebSearchOptions(*options_)) return fail("invalid_options");
    if (!input.is_object() || !input.contains("query") || !input["query"].is_string()) return fail("invalid_query");
    const auto query = input["query"].get<std::string>();
    if (query.empty() || query.size() > options_->max_query_bytes || !platform::IsValidUtf8(query))
        return fail("invalid_query");
    int count = (std::min)(kDefaultCount, options_->max_results);
    if (input.contains("count") && !input["count"].is_null()) {
        const auto& requested = input["count"];
        if (!requested.is_number_integer()) return fail("invalid_count");
        if (requested.is_number_unsigned()) count = static_cast<int>(std::min<std::uint64_t>(
            requested.get<std::uint64_t>(), static_cast<std::uint64_t>(options_->max_results)));
        else count = static_cast<int>(std::clamp<std::int64_t>(requested.get<std::int64_t>(), 1, options_->max_results));
        count = (std::max)(1, count);
    }
    try {
        const auto endpoint = SearchEndpoint(*options_);
        if (!endpoint) return fail("invalid_options");
        net::FullHttpRequest request;
        request.url = *endpoint;
        request.headers = {{"Accept", "application/json"}};
        if (search_.provider == "brave") {
            request.method = "GET";
            request.url += "?q=" + EncodeQuery(query) + "&count=" + std::to_string(count);
            request.headers.emplace_back("X-Subscription-Token", search_.api_key);
        } else {
            request.method = "POST";
            request.headers.emplace_back("Content-Type", "application/json");
            if (search_.provider == "tavily") {
                request.headers.emplace_back("Authorization", "Bearer " + search_.api_key);
                request.body = json{{"query", query}, {"max_results", count}}.dump();
            } else {
                request.headers.emplace_back("X-API-KEY", search_.api_key);
                request.body = json{{"q", query}, {"num", count}}.dump();
            }
        }
        if (cancelled()) return fail("cancelled");
        const auto response = transport_->Send(request, options_->limits, context.cancel);
        if (cancelled()) return fail("cancelled");
        if (!response) {
            switch (response.error().kind) {
            case net::FullHttpErrorKind::Cancelled: return fail("cancelled");
            case net::FullHttpErrorKind::Timeout: return fail("timeout");
            case net::FullHttpErrorKind::ResponseHeaderTooLarge: return fail("header_limit");
            case net::FullHttpErrorKind::ResponseBodyTooLarge: return fail("response_limit");
            default: return fail("network_failed");
            }
        }
        if (response->status >= 300 && response->status < 400) return fail("redirect_rejected");
        if (response->status < 200 || response->status >= 300) return fail("http_status");
        if (response->received_header_bytes > static_cast<std::uint64_t>(options_->limits.response_header_bytes))
            return fail("header_limit");
        if (response->body.size() > static_cast<std::uint64_t>(options_->limits.response_body_bytes))
            return fail("response_limit");
        auto parsed = json::parse(response->body, nullptr, false);
        if (!parsed.is_object()) return fail("invalid_response");
        json* items = nullptr;
        if (search_.provider == "tavily" && parsed.contains("results")) items = &parsed["results"];
        if (search_.provider == "serper" && parsed.contains("organic")) items = &parsed["organic"];
        if (search_.provider == "brave" && parsed.contains("web") && parsed["web"].is_object() && parsed["web"].contains("results"))
            items = &parsed["web"]["results"];
        if (!items || !items->is_array()) return fail("invalid_response");
        if (items->size() > static_cast<std::size_t>(count)) items->erase(items->begin() + count, items->end());
        auto formatted = search_.provider == "tavily" ? ParseTavilyResponse(parsed.dump())
            : search_.provider == "brave" ? ParseBraveResponse(parsed.dump()) : ParseSerperResponse(parsed.dump());
        if (!formatted) return fail("invalid_response");
        auto output = platform::SanitizeExternalText(*formatted);
        if (output.size() > options_->max_output_bytes) {
            output = TruncateUtf8(output, static_cast<std::size_t>(options_->max_output_bytes - 24));
            output += "\n[web_search.truncated]";
        }
        if (cancelled()) return fail("cancelled");
        return {std::move(output), false};
    } catch (...) { return fail("transport_failed"); }
}

}  // namespace lubancode::tools
