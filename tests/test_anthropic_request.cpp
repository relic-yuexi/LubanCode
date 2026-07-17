// 中立 Request -> Anthropic Messages JSON 的映射验证,重点是 M6.6 新增的
// think 强度 -> "thinking" 字段这条翻译(BuildRequestJson 是纯函数,声明在
// client.hpp 里专供单测调用,不碰网络)。
// 档位 -> budget_tokens 的映射(low=1024/medium=4096/high=16384,超过
// max_tokens 时退化夹到 max_tokens-256)是实现选择,已经在真实 MiniMax-M3
// anthropic 兼容端点上验证过 enabled/disabled 两种形态都能用(见任务报告),
// 这里只测纯逻辑翻译对不对。

#include <doctest/doctest.h>

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
