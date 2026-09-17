// 飞书协议纯函数册(飞书/企微设计单 F1,§5):引导/令牌响应解析、事件体
// 映射(im.message.receive_v1 → ChannelInboundEvent)、发送体构造、错误
// 分型。零 IO;敏感值(AppSecret/token)不进任何断言文案。
#include <doctest/doctest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "channel/feishu/feishu_proto.hpp"

namespace lubancode::channel::feishu {
namespace {

nlohmann::json ParseJson(const std::string& text) {
    return nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
}

// 官方事件 v2 形状的最小例(im.message.receive_v1;§5.7)。
nlohmann::json TextReceiveEvent(const char* event_id = "ev_1",
                                const char* chat_type = "p2p",
                                const char* message_type = "text",
                                const char* app_id = "cli_app1") {
    return ParseJson(std::string(R"({
      "schema": "2.0",
      "header": {
        "event_id": ")") + event_id +
                    R"(", "event_type": "im.message.receive_v1",
        "create_time": "1726500000000", "token": "verify-token",
        "app_id": ")" + app_id +
                    R"(", "tenant_key": "tk1"
      },
      "event": {
        "message": {
          "message_id": "om_100", "chat_id": "oc_200",
          "chat_type": ")" + chat_type +
                    R"(", "message_type": ")" + message_type +
                    R"(", "content": "{\"text\":\"你好 LubanCode\"}",
          "create_time": "1726500000"
        },
        "sender": {
          "sender_id": {"open_id": "ou_user1", "union_id": "on_1", "user_id": "u1"},
          "sender_type": "user", "tenant_key": "tk1"
        }
      }
    })");
}

}  // namespace

// ---------------------------------------------------------------------------
// 引导(§5.1)
// ---------------------------------------------------------------------------

TEST_CASE("feishu_proto: 引导请求体(官方大写字段名)") {
    const nlohmann::json body = BuildBootstrapRequest("cli_app1", "SECRET");
    CHECK(body.at("AppID") == "cli_app1");
    CHECK(body.at("AppSecret") == "SECRET");
    CHECK(body.at("ClientAssertion") == "");
}

TEST_CASE("feishu_proto: 引导响应解析——成功带 ClientConfig") {
    std::string error;
    const auto endpoint = ParseBootstrapResponse(ParseJson(R"({
        "code": 0, "msg": "",
        "data": {"URL": "wss://ws.example/callback?device_id=d1&service_id=25",
                 "ClientConfig": {"ReconnectCount": -1, "ReconnectInterval": 120,
                                  "ReconnectNonce": 30, "PingInterval": 90}}
    })"), &error);
    REQUIRE(endpoint.has_value());
    CHECK(endpoint->url == "wss://ws.example/callback?device_id=d1&service_id=25");
    CHECK(endpoint->ping_interval_secs == 90);
}

TEST_CASE("feishu_proto: 引导响应解析——ClientConfig 缺省 PingInterval=120") {
    std::string error;
    const auto endpoint =
        ParseBootstrapResponse(ParseJson(R"({"code":0,"data":{"URL":"wss://x"}}")"),
                               &error);
    REQUIRE(endpoint.has_value());
    CHECK(endpoint->ping_interval_secs == 120);
}

TEST_CASE("feishu_proto: 引导响应解析——业务码与形状错") {
    std::string error;
    // 设计单 §5.1 钉死的服务端错:1=SystemBusy、1000040343=InternalError。
    CHECK_FALSE(ParseBootstrapResponse(ParseJson(R"({"code":1})"), &error).has_value());
    CHECK(error.find("bootstrap server error code 1") != std::string::npos);
    CHECK_FALSE(ParseBootstrapResponse(
                    ParseJson(R"({"code":1000040343})"), &error).has_value());
    CHECK(error.find("1000040343") != std::string::npos);
    // 未知业务码:按"被平台拒绝"归类,码值进账。
    CHECK_FALSE(ParseBootstrapResponse(ParseJson(R"({"code":10021})"), &error).has_value());
    CHECK(error.find("10021") != std::string::npos);
    // 形状错:缺 data.URL / 非 object / 缺 code。
    CHECK_FALSE(ParseBootstrapResponse(ParseJson(R"({"code":0})"), &error).has_value());
    CHECK_FALSE(ParseBootstrapResponse(ParseJson(R"([])"), &error).has_value());
    CHECK_FALSE(ParseBootstrapResponse(ParseJson(R"({})"), &error).has_value());
    // PingInterval 非法(0/非正数)。
    CHECK_FALSE(ParseBootstrapResponse(
                    ParseJson(R"({"code":0,"data":{"URL":"wss://x","ClientConfig":{"PingInterval":0}}})"),
                    &error)
                    .has_value());
}

TEST_CASE("feishu_proto: 引导失败分型表(§5.1 业务码 + HTTP 状态)") {
    CHECK(ClassifyBootstrapFailure(200, ParseJson(R"({"code":1})")) ==
          FeishuBootstrapErrorKind::ServerError);
    CHECK(ClassifyBootstrapFailure(200, ParseJson(R"({"code":1000040343})")) ==
          FeishuBootstrapErrorKind::ServerError);
    CHECK(ClassifyBootstrapFailure(503, ParseJson("{}")) ==
          FeishuBootstrapErrorKind::ServerError);
    CHECK(ClassifyBootstrapFailure(429, ParseJson("{}")) ==
          FeishuBootstrapErrorKind::RateLimited);
    CHECK(ClassifyBootstrapFailure(401, ParseJson("{}")) ==
          FeishuBootstrapErrorKind::InvalidCredentials);
    CHECK(ClassifyBootstrapFailure(403, ParseJson("{}")) ==
          FeishuBootstrapErrorKind::InvalidCredentials);
    // 2xx + 未知业务码:配置侧被拒(不重试);2xx 形状错:BadResponse。
    CHECK(ClassifyBootstrapFailure(200, ParseJson(R"({"code":10021})")) ==
          FeishuBootstrapErrorKind::InvalidCredentials);
    CHECK(ClassifyBootstrapFailure(200, ParseJson("{}")) ==
          FeishuBootstrapErrorKind::BadResponse);
}

// ---------------------------------------------------------------------------
// tenant_access_token(§5.8)
// ---------------------------------------------------------------------------

TEST_CASE("feishu_proto: 令牌请求体与响应解析") {
    const nlohmann::json request = BuildTenantTokenRequest("cli_app1", "SECRET");
    CHECK(request.at("app_id") == "cli_app1");
    CHECK(request.at("app_secret") == "SECRET");

    std::string error;
    const auto token = ParseTenantTokenResponse(
        ParseJson(R"({"code":0,"expire":7200,"msg":"ok","tenant_access_token":"t-g10"})"),
        &error);
    REQUIRE(token.has_value());
    CHECK(token->tenant_access_token == "t-g10");
    CHECK(token->expire_secs == 7200);
    // expires_in 兜底形态。
    const auto fallback = ParseTenantTokenResponse(
        ParseJson(R"({"code":0,"expires_in":3600,"tenant_access_token":"t2"})"), &error);
    REQUIRE(fallback.has_value());
    CHECK(fallback->expire_secs == 3600);
    // 两字段并存且不等:冲突不猜。
    CHECK_FALSE(ParseTenantTokenResponse(
                    ParseJson(R"({"code":0,"expire":7200,"expires_in":3600,"tenant_access_token":"t3"})"),
                    &error)
                    .has_value());
    CHECK(error.find("conflict") != std::string::npos);
    // 拒绝形态:code!=0、缺 token、空 token、缺 expire。
    CHECK_FALSE(ParseTenantTokenResponse(
        ParseJson(R"({"code":99991663,"msg":"bad app secret"})"), &error).has_value());
    CHECK_FALSE(ParseTenantTokenResponse(
        ParseJson(R"({"code":0,"expire":7200})"), &error).has_value());
    CHECK_FALSE(ParseTenantTokenResponse(
        ParseJson(R"({"code":0,"expire":7200,"tenant_access_token":""})"), &error)
        .has_value());
    CHECK_FALSE(ParseTenantTokenResponse(
        ParseJson(R"({"code":0,"tenant_access_token":"t4"})"), &error).has_value());
}

// ---------------------------------------------------------------------------
// im.message.receive_v1 → ChannelInboundEvent(§5.7)
// ---------------------------------------------------------------------------

TEST_CASE("feishu_proto: p2p 文本事件映射") {
    const auto mapping = MapFeishuEventPayload(TextReceiveEvent(), "feishu", "work",
                                               "cli_app1", "feishu-del-1", 12345);
    REQUIRE(mapping.kind == FeishuEventMapping::Kind::Mapped);
    const ChannelInboundEvent& event = mapping.event;
    CHECK(event.channel_id == "feishu");
    CHECK(event.account_id == "work");
    CHECK(event.delivery_id == "feishu-del-1");
    CHECK(event.provider_event_id == "ev_1");
    CHECK(event.received_at_ms == 12345);
    CHECK(event.provider_at_ms == 1726500000000);  // header.create_time(毫秒串)
    CHECK(event.conversation.kind == ConversationKind::Direct);
    CHECK(event.conversation.id == "oc_200");
    CHECK(event.sender.id == "ou_user1");
    CHECK(event.sender.is_bot == false);
    CHECK(event.sender.is_owner == false);  // 宿主 allowlist 推导,适配器不得自称
    CHECK(event.message_id == "om_100");
    CHECK(event.hints.mentions_bot == true);  // p2p 天然发给 bot
    REQUIRE(event.parts.size() == 1);
    CHECK(event.parts[0].type == ChannelPartType::Text);
    CHECK(event.parts[0].text == std::optional<std::string>("你好 LubanCode"));
}

TEST_CASE("feishu_proto: 群聊映射与 mention 口径") {
    const auto mapping = MapFeishuEventPayload(TextReceiveEvent("ev_2", "group"),
                                               "feishu", "work", "cli_app1", "d1", 1);
    REQUIRE(mapping.kind == FeishuEventMapping::Kind::Mapped);
    CHECK(mapping.event.conversation.kind == ConversationKind::Group);
    CHECK(mapping.event.conversation.id == "oc_200");
    // 群 @ 解析后置(§十):hints.mentions_bot 恒 false——群聊默认 disabled,
    // 真开群须关 require_mention。
    CHECK(mapping.event.hints.mentions_bot == false);
}

TEST_CASE("feishu_proto: 非文本 message_type 映 unsupported 占位 part") {
    // 范围纪律:媒体/富文本/post 首版不做,占位不丢。
    const auto mapping = MapFeishuEventPayload(
        TextReceiveEvent("ev_3", "p2p", "image"), "feishu", "work", "cli_app1", "d1", 1);
    REQUIRE(mapping.kind == FeishuEventMapping::Kind::Mapped);
    REQUIRE(mapping.event.parts.size() == 1);
    CHECK(mapping.event.parts[0].type == ChannelPartType::Unsupported);
    REQUIRE(mapping.event.parts[0].unsupported_reason.has_value());
    CHECK(mapping.event.parts[0].unsupported_reason->find("image") != std::string::npos);
}

TEST_CASE("feishu_proto: sender_type=app 判 bot;app_id 对账") {
    nlohmann::json payload = TextReceiveEvent("ev_4");
    payload["event"]["sender"]["sender_type"] = "app";
    const auto bot_mapping = MapFeishuEventPayload(payload, "feishu", "work",
                                                   "cli_app1", "d1", 1);
    REQUIRE(bot_mapping.kind == FeishuEventMapping::Kind::Mapped);
    CHECK(bot_mapping.event.sender.is_bot == true);

    // 事件所属 app 与连接账号对不上:无效丢弃(QQ 互动事件的同款纪律)。
    const auto mismatch = MapFeishuEventPayload(TextReceiveEvent("ev_5"), "feishu",
                                                "work", "cli_OTHER", "d1", 1);
    CHECK(mismatch.kind == FeishuEventMapping::Kind::Invalid);
    CHECK(mismatch.detail.find("app_id mismatch") != std::string::npos);
    // expected_app_id 为空 = 不对账(观测型装配)。
    const auto no_check = MapFeishuEventPayload(TextReceiveEvent("ev_5"), "feishu",
                                                "work", "", "d1", 1);
    CHECK(no_check.kind == FeishuEventMapping::Kind::Mapped);
}

TEST_CASE("feishu_proto: 未知事件类型→UnsupportedEventType;事件体坏→Invalid") {
    nlohmann::json other_type = TextReceiveEvent();
    other_type["header"]["event_type"] = "contact.user.updated_v3";
    const auto unsupported = MapFeishuEventPayload(other_type, "feishu", "work",
                                                   "cli_app1", "d1", 1);
    CHECK(unsupported.kind == FeishuEventMapping::Kind::UnsupportedEventType);
    CHECK(unsupported.detail == "contact.user.updated_v3");

    // 无效:非 2.0 schema / 缺 event_id / 缺 open_id / text content 形状坏 /
    // chat_type 陌生。
    nlohmann::json bad_schema = TextReceiveEvent();
    bad_schema["schema"] = "1.0";
    CHECK(MapFeishuEventPayload(bad_schema, "feishu", "work", "", "d1", 1).kind ==
          FeishuEventMapping::Kind::Invalid);
    nlohmann::json no_event_id = TextReceiveEvent();
    no_event_id["header"].erase("event_id");
    CHECK(MapFeishuEventPayload(no_event_id, "feishu", "work", "", "d1", 1).kind ==
          FeishuEventMapping::Kind::Invalid);
    nlohmann::json no_open_id = TextReceiveEvent();
    no_open_id["event"]["sender"]["sender_id"].erase("open_id");
    CHECK(MapFeishuEventPayload(no_open_id, "feishu", "work", "", "d1", 1).kind ==
          FeishuEventMapping::Kind::Invalid);
    nlohmann::json bad_content = TextReceiveEvent();
    bad_content["event"]["message"]["content"] = "不是嵌 JSON";
    CHECK(MapFeishuEventPayload(bad_content, "feishu", "work", "", "d1", 1).kind ==
          FeishuEventMapping::Kind::Invalid);
    nlohmann::json bad_chat_type = TextReceiveEvent();
    bad_chat_type["event"]["message"]["chat_type"] = "somewhere";
    CHECK(MapFeishuEventPayload(bad_chat_type, "feishu", "work", "", "d1", 1).kind ==
          FeishuEventMapping::Kind::Invalid);
}

// ---------------------------------------------------------------------------
// 回话(§5.8)
// ---------------------------------------------------------------------------

TEST_CASE("feishu_proto: 回话载荷(content 是嵌 JSON 字符串)与路径") {
    const nlohmann::json payload = BuildReplyPayload("line1\nline2");
    CHECK(payload.at("msg_type") == "text");
    // content 必须是字符串形态的 {"text":…}(官方合同),不是嵌套对象。
    REQUIRE(payload.at("content").is_string());
    const nlohmann::json inner = ParseJson(payload.at("content").get<std::string>());
    REQUIRE(inner.is_object());
    CHECK(inner.at("text") == "line1\nline2");
    CHECK(ReplyPath("om_100") == "/open-apis/im/v1/messages/om_100/reply");
}

TEST_CASE("feishu_proto: 回话响应解析") {
    std::string error;
    const auto ok = ParseReplyResponse(
        ParseJson(R"({"code":0,"msg":"success","data":{"message_id":"om_201"}}")"),
        &error);
    REQUIRE(ok.has_value());
    CHECK(ok->provider_message_id == "om_201");
    CHECK_FALSE(ParseReplyResponse(
                    ParseJson(R"({"code":230002,"msg":"no permission"})"), &error)
                    .has_value());
    CHECK_FALSE(ParseReplyResponse(ParseJson(R"({"code":0})"), &error).has_value());
    CHECK_FALSE(ParseReplyResponse(ParseJson(R"({"code":0,"data":{}})"), &error)
                .has_value());
}

// ---------------------------------------------------------------------------
// 发送/令牌侧错误分型(HTTP 状态为主;业务码记账不猜义)
// ---------------------------------------------------------------------------

TEST_CASE("feishu_proto: 发送失败分型表") {
    CHECK(ClassifyFeishuApiFailure(429, "{}").kind == FeishuApiErrorKind::RateLimited);
    CHECK(ClassifyFeishuApiFailure(401, "{}").kind == FeishuApiErrorKind::Unauthorized);
    CHECK(ClassifyFeishuApiFailure(403, "{}").kind ==
          FeishuApiErrorKind::PermissionDenied);
    CHECK(ClassifyFeishuApiFailure(503, "{}").kind == FeishuApiErrorKind::ServerError);
    CHECK(ClassifyFeishuApiFailure(400, "{}").kind == FeishuApiErrorKind::UnknownError);
    // 2xx + 业务码非 0:永久拒绝,码值进账。
    const auto business = ClassifyFeishuApiFailure(
        200, R"({"code":230002,"msg":"no permission"})");
    CHECK(business.kind == FeishuApiErrorKind::UnknownError);
    CHECK(business.platform_code == 230002);
    CHECK(business.detail.find("code=230002") != std::string::npos);
    // 脱敏纪律:平台 message 不透传进 detail。
    CHECK(business.detail.find("no permission") == std::string::npos);
    // 非 JSON body 也分型得动(码 0、non-JSON 记账)。
    const auto non_json = ClassifyFeishuApiFailure(500, "oops");
    CHECK(non_json.kind == FeishuApiErrorKind::ServerError);
    CHECK(non_json.detail.find("non-JSON") != std::string::npos);
}

}  // namespace lubancode::channel::feishu
