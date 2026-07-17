// web_search 的纯函数部分:三家搜索 API(tavily/brave/serper)响应解析,
// fixture 全部手造;count 归一。真发请求那条路不进单测(没有真钥匙,集成
// 验证另说)。

#include <doctest/doctest.h>

#include <string>

#include "config/config.hpp"
#include "tools/web_search.hpp"

using lubancode::tools::ClampSearchCount;
using lubancode::tools::ParseBraveResponse;
using lubancode::tools::ParseSerperResponse;
using lubancode::tools::ParseTavilyResponse;
using lubancode::tools::WebSearchTool;

TEST_CASE("ParseTavilyResponse: 正常结果拼成编号列表") {
    const std::string body = R"({
        "query": "cmake",
        "results": [
            {"title": "CMake 官网", "url": "https://cmake.org", "content": "构建系统。", "score": 0.9},
            {"title": "CMake Wiki", "url": "https://en.wikipedia.org/wiki/CMake", "content": "维基条目"}
        ]
    })";
    const auto result = ParseTavilyResponse(body);
    REQUIRE(result.has_value());
    CHECK(result->find("1. CMake 官网") != std::string::npos);
    CHECK(result->find("https://cmake.org") != std::string::npos);
    CHECK(result->find("构建系统。") != std::string::npos);
    CHECK(result->find("2. CMake Wiki") != std::string::npos);
}

TEST_CASE("ParseTavilyResponse: 空 results 数组返回 没有搜到结果") {
    const auto result = ParseTavilyResponse(R"({"results": []})");
    REQUIRE(result.has_value());
    CHECK(result->find("没有搜到结果") != std::string::npos);
}

TEST_CASE("ParseTavilyResponse: 缺 results / 非法 JSON 报错") {
    CHECK_FALSE(ParseTavilyResponse(R"({"answer": "x"})").has_value());
    CHECK_FALSE(ParseTavilyResponse("not json{{").has_value());
}

TEST_CASE("ParseBraveResponse: web.results 里的 title/url/description") {
    const std::string body = R"({
        "type": "search",
        "web": {
            "results": [
                {"title": "Brave 第一条", "url": "https://a.example", "description": "摘要甲"},
                {"title": "第二条", "url": "https://b.example", "description": "摘要乙"}
            ]
        }
    })";
    const auto result = ParseBraveResponse(body);
    REQUIRE(result.has_value());
    CHECK(result->find("1. Brave 第一条") != std::string::npos);
    CHECK(result->find("https://a.example") != std::string::npos);
    CHECK(result->find("摘要甲") != std::string::npos);
    CHECK(result->find("2. 第二条") != std::string::npos);
}

TEST_CASE("ParseBraveResponse: 缺 web 段 / web.results 不是数组,报错") {
    CHECK_FALSE(ParseBraveResponse(R"({"query": {}})").has_value());
    CHECK_FALSE(ParseBraveResponse(R"({"web": {"results": "oops"}})").has_value());
}

TEST_CASE("ParseSerperResponse: organic 里的 title/link/snippet") {
    const std::string body = R"({
        "searchParameters": {"q": "test"},
        "organic": [
            {"title": "Serper 结果", "link": "https://c.example", "snippet": "摘要丙", "position": 1},
            {"title": "无摘要的一条", "link": "https://d.example"}
        ]
    })";
    const auto result = ParseSerperResponse(body);
    REQUIRE(result.has_value());
    CHECK(result->find("1. Serper 结果") != std::string::npos);
    CHECK(result->find("https://c.example") != std::string::npos);
    CHECK(result->find("摘要丙") != std::string::npos);
    CHECK(result->find("2. 无摘要的一条") != std::string::npos);
    CHECK(result->find("https://d.example") != std::string::npos);
}

TEST_CASE("ParseSerperResponse: 缺 organic 报错,空 organic 不算错") {
    CHECK_FALSE(ParseSerperResponse(R"({"answerBox": {}})").has_value());
    const auto empty = ParseSerperResponse(R"({"organic": []})");
    REQUIRE(empty.has_value());
    CHECK(empty->find("没有搜到结果") != std::string::npos);
}

TEST_CASE("结果项缺字段不报废:缺标题给占位,缺摘要不占行") {
    const auto result = ParseTavilyResponse(R"({"results": [{"url": "https://x.example"}]})");
    REQUIRE(result.has_value());
    CHECK(result->find("(无标题)") != std::string::npos);
    CHECK(result->find("https://x.example") != std::string::npos);
}

TEST_CASE("ClampSearchCount: 夹到 1..10") {
    CHECK(ClampSearchCount(5) == 5);
    CHECK(ClampSearchCount(0) == 1);
    CHECK(ClampSearchCount(-3) == 1);
    CHECK(ClampSearchCount(10) == 10);
    CHECK(ClampSearchCount(99) == 10);
}

TEST_CASE("WebSearchTool: 元信息与参数校验") {
    lubancode::config::SearchConfig search;
    search.provider = "tavily";
    search.api_key = "test-key";
    WebSearchTool tool(search);
    CHECK(tool.name() == "web_search");
    CHECK_FALSE(tool.needs_confirm());
    CHECK(tool.input_schema()["required"] == nlohmann::json::array({"query"}));
    // 缺 query / 空 query:不碰网络,直接报错,而且错误文本里不能带 api_key。
    const auto missing = tool.execute(nlohmann::json::object());
    CHECK(missing.is_error);
    CHECK(missing.content.find("test-key") == std::string::npos);
    CHECK(tool.execute({{"query", ""}}).is_error);
}
