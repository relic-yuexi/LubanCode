// 中立 Request -> Anthropic Messages JSON 的映射验证,重点是 M6.6 新增的
// think 强度 -> "thinking" 字段这条翻译(BuildRequestJson 是纯函数,声明在
// client.hpp 里专供单测调用,不碰网络)。
// 档位 -> budget_tokens 的映射(low=1024/medium=4096/high=16384,超过
// max_tokens 时退化夹到 max_tokens-256)是实现选择,已经在真实 MiniMax-M3
// anthropic 兼容端点上验证过 enabled/disabled 两种形态都能用(见任务报告),
// 这里只测纯逻辑翻译对不对。

#include <doctest/doctest.h>

#include <sstream>
#include <vector>

#include "api/anthropic/client.hpp"
#include "api/types.hpp"
#include "platform/log_sink.hpp"

using namespace lubancode::api;
namespace platform = lubancode::platform;
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
    CHECK(budget < *request.max_tokens);
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

TEST_CASE("Anthropic Messages: effort 方言写 adaptive thinking + output_config.effort") {
    Request request;
    request.reasoning_effort = "xhigh";
    request.reasoning.supports_effort = true;
    request.reasoning.wire_dialect = "effort";
    const auto body = BuildRequestJson(request);
    CHECK(body["thinking"]["type"] == "adaptive");
    CHECK(body["output_config"]["effort"] == "xhigh");
    CHECK(request.extra_body.empty());
}

TEST_CASE("reasoning_effort 是映射表之外的字符串:不写 thinking 字段,LogSink 打警告") {
    // 显示系统剥离单第八步:anthropic 的档位诊断改投 platform::LogSink,
    // 不再裸写 stderr(engine 守门:engine 层零标准流)。这里挂一只录音
    // 回调断言同一条诊断仍在。
    std::vector<platform::LogRecord> logs;
    platform::LogSink::Instance().SetWriter([&logs](const platform::LogRecord& record) {
        logs.push_back(record);
    });

    Request request;
    request.reasoning_effort = "extreme";
    const auto body = BuildRequestJson(request);

    platform::LogSink::Instance().SetWriter(nullptr);

    CHECK_FALSE(body.contains("thinking"));
    REQUIRE_FALSE(logs.empty());
    bool seen = false;
    for (const auto& record : logs) {
        if (record.message.find("extreme") != std::string::npos &&
            record.message.find("映射") != std::string::npos) {
            seen = true;
        }
    }
    CHECK(seen);
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

// ---------------------------------------------------------------------------
// extra_body:"任意模型特殊参数"扩展口子。浅合并进请求体顶层,merge 点在
// thinking/native_web_search/messages/tools 都拼完之后、返回之前;键冲突时
// extra_body 整个覆盖掉内置算出来的值,不做深合并——这是明确写进文档的
// 简化规则,这里验证覆盖顺序对不对。
// ---------------------------------------------------------------------------

TEST_CASE("extra_body 缺省(默认空 object)时,请求体跟不传这个参数一模一样") {
    Request request;
    request.reasoning_effort = "medium";
    const auto body_default = BuildRequestJson(request);
    const auto body_explicit_empty = BuildRequestJson(request, /*native_web_search=*/false, nlohmann::json::object());
    CHECK(body_default == body_explicit_empty);
}

TEST_CASE("extra_body 里的新键原样加到请求体顶层") {
    Request request;
    const auto extra_body = nlohmann::json::parse(R"({"reasoning_effort":"max"})");
    const auto body = BuildRequestJson(request, /*native_web_search=*/false, extra_body);
    CHECK(body.at("reasoning_effort") == "max");
}

TEST_CASE("extra_body 跟内置字段(thinking)同名时,extra_body 的值整个覆盖内置算出来的值") {
    Request request;
    request.reasoning_effort = "high";  // 内置逻辑会算出 thinking.type=enabled + budget_tokens
    const auto extra_body = nlohmann::json::parse(R"({"thinking":{"type":"enabled"}})");
    const auto body = BuildRequestJson(request, /*native_web_search=*/false, extra_body);
    // 整段替换,不是深合并——内置算出来的 budget_tokens 应该消失了。
    CHECK(body.at("thinking") == nlohmann::json::parse(R"({"type":"enabled"})"));
    CHECK_FALSE(body.at("thinking").contains("budget_tokens"));
}

TEST_CASE("模型 variant 的 request.extra_body 最后压过 provider extra_body") {
    Request request;
    request.extra_body = nlohmann::json{{"thinking", nlohmann::json{{"type", "disabled"}}}};
    const auto body = BuildRequestJson(
        request, false, nlohmann::json{{"thinking", nlohmann::json{{"type", "enabled"}}}});
    CHECK(body["thinking"]["type"] == "disabled");
}

TEST_CASE("extra_body 也能覆盖 native_web_search 拼出来的 tools 数组") {
    Request request;
    const auto extra_body = nlohmann::json::parse(R"({"tools":[]})");
    const auto body = BuildRequestJson(request, /*native_web_search=*/true, extra_body);
    CHECK(body.at("tools").empty());  // 内置拼出来的 web_search 声明被整段覆盖掉了
}

// ---------------------------------------------------------------------------
// ApplyExtraHeaders:extra_headers 覆盖/追加进基础 HTTP 头表,同名覆盖
// (含 Authorization),不同名追加。key 精确匹配(大小写敏感——真正发送前
// cpr::Header 自己还会再做一层大小写不敏感的去重,这里只测这一层本身)。
// ---------------------------------------------------------------------------

TEST_CASE("ApplyExtraHeaders: extra_headers 为空时,基础头原样不变") {
    const std::map<std::string, std::string> base{{"Content-Type", "application/json"},
                                                    {"Authorization", "Bearer token"}};
    const auto merged = lubancode::api::anthropic::ApplyExtraHeaders(base, {});
    CHECK(merged == base);
}

TEST_CASE("ApplyExtraHeaders: 新头名字追加,不影响原有的") {
    const std::map<std::string, std::string> base{{"Content-Type", "application/json"}};
    const auto merged = lubancode::api::anthropic::ApplyExtraHeaders(base, {{"X-Api-Version", "2024-06-01"}});
    CHECK(merged.at("Content-Type") == "application/json");
    CHECK(merged.at("X-Api-Version") == "2024-06-01");
}

TEST_CASE("ApplyExtraHeaders: 同名(含 Authorization)整条覆盖,用户对自己配的头负责") {
    const std::map<std::string, std::string> base{{"Content-Type", "application/json"},
                                                    {"Authorization", "Bearer old-token"}};
    const auto merged = lubancode::api::anthropic::ApplyExtraHeaders(base, {{"Authorization", "Bearer new-token"}});
    CHECK(merged.at("Authorization") == "Bearer new-token");
    CHECK(merged.at("Content-Type") == "application/json");  // 没点名的那条不受影响
}

TEST_CASE("ApplyExtraHeaders: 空值删除基础头") {
    const std::map<std::string, std::string> base{{"Authorization", "Bearer old"}, {"X-Keep", "yes"}};
    const auto merged = lubancode::api::anthropic::ApplyExtraHeaders(base, {{"Authorization", ""}});
    CHECK_FALSE(merged.contains("Authorization"));
    CHECK(merged.at("X-Keep") == "yes");
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

// 工具 schema 归一化:空 schema 出门前兑成最小合法壳(缘起与四家共用的
// 兑法见 api/types.hpp 的 ToolSchemaForWire 注释)。Anthropic 这家字段名是
// input_schema,不是 parameters。
TEST_CASE("工具空 schema 兑成 type=object,合规的原样放行") {
    Request request;
    request.tools.push_back({"list_sessions", "列会话", nlohmann::json::object()});
    const auto good = nlohmann::json{{"type", "object"},
                                     {"properties", {{"path", {{"type", "string"}}}}},
                                     {"required", nlohmann::json::array({"path"})}};
    request.tools.push_back({"read_file", "读文件", good});

    const auto body = BuildRequestJson(request);
    CHECK(body.at("tools").at(0).at("input_schema").at("type") == "object");
    CHECK(body.at("tools").at(0).at("input_schema").at("properties").is_object());
    CHECK(body.at("tools").at(1).at("input_schema") == good);
}
