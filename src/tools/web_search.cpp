#include "tools/web_search.hpp"

#include <string>
#include <utility>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

namespace lubancode::tools {

namespace {

using nlohmann::json;

constexpr int kDefaultCount = 5;
constexpr int kMaxCount = 10;

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

}  // namespace lubancode::tools
