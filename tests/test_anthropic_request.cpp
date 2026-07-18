// 中立 Request -> Anthropic Messages JSON 的映射验证,重点是 M6.6 新增的
// think 强度 -> "thinking" 字段这条翻译(BuildRequestJson 是纯函数,声明在
// client.hpp 里专供单测调用,不碰网络)。
// 档位 -> budget_tokens 的映射(low=1024/medium=4096/high=16384,超过
// max_tokens 时退化夹到 max_tokens-256)是实现选择,已经在真实 MiniMax-M3
// anthropic 兼容端点上验证过 enabled/disabled 两种形态都能用(见任务报告),
// 这里只测纯逻辑翻译对不对。

#include <doctest/doctest.h>

#include <iostream>
#include <sstream>

#include "api/anthropic/client.hpp"
#include "api/types.hpp"

using namespace lubancode::api;
using lubancode::api::anthropic::BuildRequestJson;

TEST_CASE("reasoning_effort 为空串时不写 thinking 字段") {
    Request request;
    request.reasoning_effort = "";
    const auto body = BuildRequestJson(request);
    CHECK_FALSE(body.contains("thinking"));
}

TEST_CASE("reasoning_effort=none 映射成 thinking.type=disabled") {
    Request request;
    request.reasoning_effort = "none";
    const auto body = BuildRequestJson(request);
    REQUIRE(body.contains("thinking"));
    CHECK(body.at("thinking").at("type") == "disabled");
    CHECK_FALSE(body.at("thinking").contains("budget_tokens"));
}

TEST_CASE("reasoning_effort=low/medium/high 映射成 thinking.type=enabled,budget_tokens 按档位递增") {
    Request low_req;
    low_req.reasoning_effort = "low";
    const auto low_body = BuildRequestJson(low_req);
    CHECK(low_body.at("thinking").at("type") == "enabled");
    const int low_budget = low_body.at("thinking").at("budget_tokens").get<int>();

    Request medium_req;
    medium_req.reasoning_effort = "medium";
    const auto medium_body = BuildRequestJson(medium_req);
    const int medium_budget = medium_body.at("thinking").at("budget_tokens").get<int>();

    Request high_req;
    high_req.reasoning_effort = "high";
    high_req.max_tokens = 32768;  // 给 high 档足够大的 max_tokens,budget_tokens 不用夹
    const auto high_body = BuildRequestJson(high_req);
    const int high_budget = high_body.at("thinking").at("budget_tokens").get<int>();

    CHECK(low_budget < medium_budget);
    CHECK(medium_budget < high_budget);
}

TEST_CASE("budget_tokens 永远小于 max_tokens(Anthropic 的硬约束),max_tokens 很小时也不越界") {
    Request request;
    request.reasoning_effort = "high";
    request.max_tokens = 512;  // 默认 high 档位的 16384 远超这个 max_tokens
    const auto body = BuildRequestJson(request);
    const int budget = body.at("thinking").at("budget_tokens").get<int>();
    CHECK(budget < request.max_tokens);
    CHECK(budget >= 1);
}

TEST_CASE("system/thinking 都设置时,两个字段互不影响,各自正常出现") {
    Request request;
    request.system = "你是一个有用的助手。";
    request.reasoning_effort = "medium";
    const auto body = BuildRequestJson(request);
    CHECK(body.at("system") == "你是一个有用的助手。");
    CHECK(body.at("thinking").at("type") == "enabled");
}

// ---------------------------------------------------------------------------
// M10:档位放开成任意字符串——anthropic 这边内置一张 none/low/medium/high/
// xhigh/max 映射表,新增 xhigh/max 两档;映射不上的名字打警告、当没设。
// ---------------------------------------------------------------------------

TEST_CASE("reasoning_effort=xhigh/max 映射成 thinking.type=enabled,budget_tokens 比 high 更大") {
    Request high_req;
    high_req.reasoning_effort = "high";
    high_req.max_tokens = 65536;
    const int high_budget = BuildRequestJson(high_req).at("thinking").at("budget_tokens").get<int>();

    Request xhigh_req;
    xhigh_req.reasoning_effort = "xhigh";
    xhigh_req.max_tokens = 65536;
    const auto xhigh_body = BuildRequestJson(xhigh_req);
    CHECK(xhigh_body.at("thinking").at("type") == "enabled");
    const int xhigh_budget = xhigh_body.at("thinking").at("budget_tokens").get<int>();

    Request max_req;
    max_req.reasoning_effort = "max";
    max_req.max_tokens = 65536;
    const auto max_body = BuildRequestJson(max_req);
    CHECK(max_body.at("thinking").at("type") == "enabled");
    const int max_budget = max_body.at("thinking").at("budget_tokens").get<int>();

    CHECK(high_budget < xhigh_budget);
    CHECK(xhigh_budget < max_budget);
}

TEST_CASE("reasoning_effort 档位名大小写不敏感") {
    Request request;
    request.reasoning_effort = "XHIGH";
    const auto body = BuildRequestJson(request);
    CHECK(body.at("thinking").at("type") == "enabled");
}

TEST_CASE("reasoning_effort 是映射表之外的字符串:不写 thinking 字段,stderr 打警告") {
    std::ostringstream captured;
    std::streambuf* old_buf = std::cerr.rdbuf(captured.rdbuf());

    Request request;
    request.reasoning_effort = "extreme";
    const auto body = BuildRequestJson(request);

    std::cerr.rdbuf(old_buf);

    CHECK_FALSE(body.contains("thinking"));
    CHECK(captured.str().find("extreme") != std::string::npos);
    CHECK(captured.str().find("警告") != std::string::npos);
}

// ---------------------------------------------------------------------------
// M12(anthropic 协议接原生 web_search):跟 Responses 那边 M12 是同一个配置
// 开关(ProviderConfig::native_web_search / Config::native_web_search),只
// 是这里翻译成 anthropic 协议自己的 server tool 声明形状——
// {"type":"web_search_20260209","name":"web_search"},没有 description/
// input_schema 这两个字段(跟本地函数工具的形状不一样)。
// ---------------------------------------------------------------------------

TEST_CASE("native_web_search=false(默认)时不写 web_search 声明,行为跟现状一致") {
    Request request;
    const auto body = BuildRequestJson(request);
    CHECK_FALSE(body.contains("tools"));

    const auto body_explicit_false = BuildRequestJson(request, /*native_web_search=*/false);
    CHECK_FALSE(body_explicit_false.contains("tools"));
}

TEST_CASE("native_web_search=true 且本地工具表为空时,tools 数组只有 web_search 一项") {
    Request request;
    const auto body = BuildRequestJson(request, /*native_web_search=*/true);
    REQUIRE(body.contains("tools"));
    REQUIRE(body.at("tools").size() == 1);
    CHECK(body.at("tools")[0].at("type") == "web_search_20260209");
    CHECK(body.at("tools")[0].at("name") == "web_search");
    CHECK_FALSE(body.at("tools")[0].contains("description"));
    CHECK_FALSE(body.at("tools")[0].contains("input_schema"));
}

TEST_CASE("native_web_search=true 且本地工具表非空时,web_search 追加在本地工具后面") {
    Request request;
    ToolDefinition def;
    def.name = "read_file";
    def.description = "读一个文件的内容";
    def.input_schema = nlohmann::json{{"type", "object"}};
    request.tools.push_back(def);

    const auto body = BuildRequestJson(request, /*native_web_search=*/true);
    REQUIRE(body.contains("tools"));
    REQUIRE(body.at("tools").size() == 2);
    CHECK(body.at("tools")[0].at("name") == "read_file");
    CHECK(body.at("tools")[0].contains("input_schema"));
    CHECK(body.at("tools")[1].at("type") == "web_search_20260209");
    CHECK(body.at("tools")[1].at("name") == "web_search");
}

TEST_CASE("用户图片映射成 Anthropic image/base64 block") {
    Request request;
    Message user;
    user.role = Role::User;
    user.content.push_back(TextBlock{"看看报错"});
    user.content.push_back(ImageBlock{"image/png", "aGVsbG8=", "error.png", 640, 480});
    request.messages.push_back(user);

    const auto body = BuildRequestJson(request);
    const auto& content = body.at("messages").at(0).at("content");
    REQUIRE(content.size() == 2);
    CHECK(content.at(1).at("type") == "image");
    CHECK(content.at(1).at("source").at("type") == "base64");
    CHECK(content.at(1).at("source").at("media_type") == "image/png");
    CHECK(content.at(1).at("source").at("data") == "aGVsbG8=");
}
