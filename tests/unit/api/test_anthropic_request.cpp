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
#include <memory>
#include <set>

#include "tools/registry.hpp"
#include "tools/tool_search.hpp"

using namespace lubancode::api;
namespace platform = lubancode::platform;
namespace api = lubancode::api;
using lubancode::api::anthropic::BuildRequestJson;

TEST_CASE("富工具结果降级: tool_result 的 content 走投影文本,base64 不出门(MCP 富结果单 P0.6)") {
    api::Request request;
    request.model = "m";
    request.max_tokens = 100;
    api::Message user_msg;
    user_msg.role = api::Role::User;
    api::ToolResultBlock rich;
    rich.tool_use_id = "toolu_shot";
    rich.content = "[图片 art-00112233.png image/png 640x480 2048字节 artifact=mcp-artifacts/art-00112233.png]";
    lubancode::tools::ImageContent image;
    image.mime_type = "image/png";
    image.width = 640;
    image.height = 480;
    image.bytes = 2048;
    image.artifact.filename = "art-00112233.png";
    image.artifact.path = "mcp-artifacts/art-00112233.png";
    image.artifact.stored = true;
    rich.blocks.push_back(std::move(image));
    rich.structured_content = nlohmann::json{{"sha256", "ab"}};
    user_msg.content.push_back(rich);
    request.messages.push_back(user_msg);

    const auto body = lubancode::api::anthropic::BuildRequestJson(request);
    const auto& wire_result = body.at("messages")[0].at("content")[0];
    CHECK(wire_result.at("type") == "tool_result");
    CHECK(wire_result.at("tool_use_id") == "toolu_shot");
    // 文本降级:投影短句就是 wire 上的全部,不假定原生 image 块、不带 base64。
    CHECK(wire_result.at("content") == rich.content);
    CHECK(wire_result.dump().find("base64") == std::string::npos);
}

TEST_CASE("工具结果图片回喂: 重灌过的图上原生 image 块,content 变块数组(工具结果图片回喂单)") {
    api::Request request;
    request.model = "m";
    request.max_tokens = 100;
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ToolUseBlock{"toolu_shot", "gui_screenshot", nlohmann::json::object()});
    api::Message user_msg;
    user_msg.role = api::Role::User;
    api::ToolResultBlock rich;
    rich.tool_use_id = "toolu_shot";
    rich.content = "已截图 640x480,证据文件落 gui-obs-1.png";
    lubancode::tools::ImageContent image;
    image.mime_type = "image/png";
    image.width = 640;
    image.height = 480;
    image.bytes = 9;
    image.wire_base64 = "iVBORw0KGgo=";  // 重灌产物(恳求态)
    image.artifact.filename = "art-00112233.png";
    image.artifact.path = "mcp-artifacts/art-00112233.png";
    image.artifact.stored = true;
    rich.blocks.push_back(std::move(image));
    user_msg.content.push_back(rich);
    request.messages.push_back(assistant);
    request.messages.push_back(user_msg);

    const auto body = lubancode::api::anthropic::BuildRequestJson(request);
    const auto& wire_result = body.at("messages")[1].at("content")[0];
    CHECK(wire_result.at("type") == "tool_result");
    // content 是块数组:第一块文本投影,第二块原生 image(base64 source)。
    REQUIRE(wire_result.at("content").is_array());
    REQUIRE(wire_result.at("content").size() == 2);
    CHECK(wire_result.at("content")[0].at("type") == "text");
    CHECK(wire_result.at("content")[0].at("text") == rich.content);
    const auto& wire_image = wire_result.at("content")[1];
    CHECK(wire_image.at("type") == "image");
    CHECK(wire_image.at("source").at("type") == "base64");
    CHECK(wire_image.at("source").at("media_type") == "image/png");
    CHECK(wire_image.at("source").at("data") == "iVBORw0KGgo=");
}

TEST_CASE("原始 think 兼容门只认 thinking+tool_use 紧接 tool_result") {
    api::Request request;
    request.messages = {
        api::Message{api::Role::User, {api::TextBlock{"读文件"}}},
        api::Message{api::Role::Assistant,
                     {api::ThinkingBlock{"先读", "sig"},
                      api::ToolUseBlock{"toolu_1", "read_file", nlohmann::json::object()}}},
        api::Message{api::Role::User, {api::ToolResultBlock{"toolu_1", "内容", false}}},
    };
    CHECK(api::anthropic::ShouldRecoverTaggedThinking(request));

    request.messages[1].content.erase(request.messages[1].content.begin());
    CHECK_FALSE(api::anthropic::ShouldRecoverTaggedThinking(request));

    request.messages[1].content.insert(request.messages[1].content.begin(), api::ThinkingBlock{"先读", "sig"});
    request.messages.back() = api::Message{api::Role::User, {api::TextBlock{"普通追问"}}};
    CHECK_FALSE(api::anthropic::ShouldRecoverTaggedThinking(request));
}

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


TEST_CASE("ModelImageBlock 重放:翻 text 短标记,不带 base64") {
    Request request;
    request.model = "claude-x";
    ModelImageBlock ref;
    ref.id = "ig_1";
    ref.filename = "img-abcd12.png";
    ref.path = "images/img-abcd12.png";
    ref.width = 512;
    ref.height = 512;
    Message assistant;
    assistant.role = Role::Assistant;
    assistant.content.push_back(ref);
    request.messages.push_back(assistant);

    const auto body = BuildRequestJson(request, /*native_web_search=*/false, nlohmann::json::object());
    const std::string dumped = body.dump();
    CHECK(dumped.find("[模型已生成图片: img-abcd12.png (512x512)]") != std::string::npos);
    CHECK(dumped.find("images/img-abcd12.png") == std::string::npos);
    CHECK(dumped.find("\"image\"") == std::string::npos);
}

// 动态工具 PromptCache 守恒单 P1:Anthropic 兼容端的 proxy_reference
// replay(通用代理路,不是 P3 的 defer_loading/native)。顶层只见固定的
// tool_search + tool_invoke;代理调用的 input 是原生 JSON 对象,往返
// 逐字段无损。
TEST_CASE("Anthropic proxy replay: tool_search+tool_invoke 定义恒在,代理调用 input 无损") {
    lubancode::tools::ToolRegistry dummy_registry;
    auto loaded = std::make_shared<std::set<std::string>>();
    lubancode::tools::ToolSearchTool search(dummy_registry, loaded);
    lubancode::tools::ToolInvokeTool invoke;

    api::Request request;
    request.model = "claude-sonnet";
    request.tools.push_back({"tool_search", search.description(), search.input_schema()});
    request.tools.push_back({"tool_invoke", invoke.description(), invoke.input_schema()});

    api::Message search_result;
    search_result.role = api::Role::User;
    const std::string discovery_json =
        nlohmann::json{{"catalog_revision", "sha256:abc"},
                       {"matches", nlohmann::json::array({nlohmann::json{
                                       {"tool_ref", "dt_0123456789_1"},
                                       {"name", "mcp__github__search_issues"},
                                       {"schema_digest", "sha256:def"},
                                       {"input_schema", nlohmann::json{{"type", "object"}}}}})}}
            .dump();
    search_result.content.push_back(api::ToolResultBlock{"call_search", discovery_json, false});

    api::Message proxy_call;
    proxy_call.role = api::Role::Assistant;
    proxy_call.content.push_back(api::ToolUseBlock{
        "call_invoke", "tool_invoke",
        nlohmann::json{{"tool_ref", "dt_0123456789_1"},
                       {"arguments", nlohmann::json{{"query", "repo:lubancode cache"}, {"per_page", 50}}}}});
    api::Message proxy_result;
    proxy_result.role = api::Role::User;
    proxy_result.content.push_back(api::ToolResultBlock{"call_invoke", "issue #42", false});
    request.messages.push_back(search_result);
    request.messages.push_back(proxy_call);
    request.messages.push_back(proxy_result);

    const auto body = BuildRequestJson(request, /*native_web_search=*/false, nlohmann::json::object());
    REQUIRE(body["tools"].size() == 2);
    CHECK(body["tools"][0]["name"] == "tool_search");
    CHECK(body["tools"][1]["name"] == "tool_invoke");
    CHECK(body["tools"][1]["input_schema"]["additionalProperties"] == false);

    // 发现结果 JSON 正文原样重放(Anthropic tool_result 的 content 字符串)。
    bool saw_discovery = false;
    for (const auto& message : body["messages"]) {
        for (const auto& block : message["content"]) {
            if (block.value("type", std::string()) == "tool_result" && block["tool_use_id"] == "call_search") {
                saw_discovery = block["content"].get<std::string>() == discovery_json;
            }
        }
    }
    CHECK(saw_discovery);

    // 代理调用:tool_use 的 input 是对象,逐字段无损。
    bool saw_proxy = false;
    for (const auto& message : body["messages"]) {
        for (const auto& block : message["content"]) {
            if (block.value("type", std::string()) == "tool_use" && block["name"] == "tool_invoke") {
                saw_proxy = true;
                CHECK(block["id"] == "call_invoke");
                CHECK(block["input"]["tool_ref"] == "dt_0123456789_1");
                CHECK(block["input"]["arguments"]["query"] == "repo:lubancode cache");
                CHECK(block["input"]["arguments"]["per_page"] == 50);
            }
        }
    }
    CHECK(saw_proxy);
}

// ---------------------------------------------------------------------------
// 动态工具 P3(Claude NativeReference·§7.1):defer_loading 与服务端工具
// 搜索声明的请求映射。Eager 定义不写 defer_loading 字段(与从前逐字节
// 一致);Deferred 定义照发全文、只加 defer_loading:true;server_tool_search
// 非空时 tools 追加对应变体的 server tool 声明(regex 与 bm25 是同日发布的
// 两个算法变体,不是新旧版)。
// ---------------------------------------------------------------------------

TEST_CASE("P3 defer_loading: Eager 不写字段,Deferred 照发全文只加 defer_loading=true") {
    Request request;
    ToolDefinition eager;
    eager.name = "read_file";
    eager.description = "读文件";
    eager.input_schema = nlohmann::json{{"type", "object"}};
    ToolDefinition deferred;
    deferred.name = "mcp__github__search_issues";
    deferred.description = "搜 issue";
    deferred.input_schema = nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}};
    deferred.load_mode = ToolLoadMode::Deferred;
    request.tools.push_back(eager);
    request.tools.push_back(deferred);

    const auto body = BuildRequestJson(request);
    REQUIRE(body["tools"].size() == 2);
    CHECK_FALSE(body["tools"][0].contains("defer_loading"));
    REQUIRE(body["tools"][1].contains("defer_loading"));
    CHECK(body["tools"][1]["defer_loading"] == true);
    // 全文照发:provider 要拿它跑搜索、展开 tool_reference,不是只发名字。
    CHECK(body["tools"][1]["name"] == "mcp__github__search_issues");
    CHECK(body["tools"][1]["description"] == "搜 issue");
    CHECK(body["tools"][1].contains("input_schema"));
}

TEST_CASE("P3 server_tool_search: regex/bm25 各映射成对应变体声明,空串不声明") {
    Request request;
    ToolDefinition core;
    core.name = "read_file";
    core.description = "读文件";
    core.input_schema = nlohmann::json{{"type", "object"}};
    request.tools.push_back(core);

    // 空串(缺省):不声明,tools 只剩本地工具——与从前一字不差。
    const auto body_off = BuildRequestJson(request);
    REQUIRE(body_off["tools"].size() == 1);
    CHECK(body_off["tools"][0]["name"] == "read_file");

    // regex 变体。
    request.server_tool_search = "regex";
    const auto body_regex = BuildRequestJson(request);
    REQUIRE(body_regex["tools"].size() == 2);
    CHECK(body_regex["tools"][1]["type"] == "tool_search_tool_regex_20251119");
    CHECK(body_regex["tools"][1]["name"] == "tool_search_tool_regex");
    CHECK_FALSE(body_regex["tools"][1].contains("input_schema"));
    CHECK_FALSE(body_regex["tools"][1].contains("defer_loading"));  // 搜索工具自身绝不 defer

    // bm25 变体。
    request.server_tool_search = "bm25";
    const auto body_bm25 = BuildRequestJson(request);
    REQUIRE(body_bm25["tools"].size() == 2);
    CHECK(body_bm25["tools"][1]["type"] == "tool_search_tool_bm25_20251119");
    CHECK(body_bm25["tools"][1]["name"] == "tool_search_tool_bm25");
}

TEST_CASE("P3 server_tool_search: 本地工具表为空也能单独声明(与 native_web_search 同一条理)") {
    Request request;
    request.server_tool_search = "regex";
    const auto body = BuildRequestJson(request);
    REQUIRE(body.contains("tools"));
    REQUIRE(body["tools"].size() == 1);
    CHECK(body["tools"][0]["type"] == "tool_search_tool_regex_20251119");
}

TEST_CASE("P3 防御: 全表 deferred 时首枚降回 eager(官方 400 合同)") {
    Request request;
    ToolDefinition deferred;
    deferred.name = "only_deferred";
    deferred.description = "全表只有它";
    deferred.input_schema = nlohmann::json{{"type", "object"}};
    deferred.load_mode = ToolLoadMode::Deferred;
    request.tools.push_back(deferred);

    const auto body = BuildRequestJson(request);
    REQUIRE(body["tools"].size() == 1);
    CHECK_FALSE(body["tools"][0].contains("defer_loading"));
}

// ---------------------------------------------------------------------------
// 动态工具 P3(§7.2):原生块的无损回传。server_tool_use /
// tool_search_tool_result(含嵌套 tool_reference / error)原样回放,
// 绝不压成普通文本(单子红线 9);caller 给过的照带。
// ---------------------------------------------------------------------------

TEST_CASE("P3 原生块回传: server_tool_use/tool_search_tool_result 逐字段无损,caller 照带") {
    Request request;
    request.model = "claude-opus-5";

    api::Message assistant;
    assistant.role = api::Role::Assistant;
    api::ServerToolUseBlock server_use;
    server_use.id = "srvtoolu_01ABC";
    server_use.name = "tool_search_tool_regex";
    server_use.input = nlohmann::json{{"pattern", "weather"}, {"limit", 10}};
    assistant.content.push_back(server_use);
    const nlohmann::json result_content = nlohmann::json{
        {"type", "tool_search_tool_search_result"},
        {"tool_references", nlohmann::json::array({nlohmann::json{{"type", "tool_reference"},
                                                                  {"tool_name", "get_weather"}}})}};
    api::ServerToolResultBlock server_result;
    server_result.tool_use_id = "srvtoolu_01ABC";
    server_result.content = result_content;
    assistant.content.push_back(server_result);
    api::ToolUseBlock real_call;
    real_call.id = "toolu_01XYZ";
    real_call.name = "get_weather";
    real_call.input = nlohmann::json{{"location", "San Francisco"}};
    real_call.caller = "code_execution_20260120";
    assistant.content.push_back(real_call);
    request.messages.push_back(assistant);

    const auto body = BuildRequestJson(request);
    const auto& content = body.at("messages").at(0).at("content");
    REQUIRE(content.size() == 3);

    CHECK(content.at(0).at("type") == "server_tool_use");
    CHECK(content.at(0).at("id") == "srvtoolu_01ABC");
    CHECK(content.at(0).at("name") == "tool_search_tool_regex");
    CHECK(content.at(0).at("input") == server_use.input);

    CHECK(content.at(1).at("type") == "tool_search_tool_result");
    CHECK(content.at(1).at("tool_use_id") == "srvtoolu_01ABC");
    CHECK(content.at(1).at("content") == result_content);  // 嵌套 tool_reference 无损,不压文本

    CHECK(content.at(2).at("type") == "tool_use");
    CHECK(content.at(2).at("caller") == "code_execution_20260120");
}

TEST_CASE("P3 原生块回传: caller 缺省不造字段(旧档形状不变)") {
    Request request;
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    api::ToolUseBlock call;
    call.id = "toolu_1";
    call.name = "read_file";
    call.input = nlohmann::json::object();
    assistant.content.push_back(call);
    request.messages.push_back(assistant);

    const auto body = BuildRequestJson(request);
    CHECK_FALSE(body.at("messages").at(0).at("content").at(0).contains("caller"));
}
