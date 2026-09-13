// QQ v2 协议纯函数册(QQ 机器人接入单 Q1)。官方 bot.q.qq.com 文档示例
// 固化为 fixture:
//   - payload/opcode 表(dev-prepare/event-emit/payload.html)
//   - C2C 事件三例(autogen/event/c2c_message_create.html 示例 1/2/3)
//   - 发送请求/响应(autogen/api/v2_users_user_openid_messages.post.html)
//   - 错误码表(同上页全表)
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "channel/qq/qq_proto.hpp"

namespace lubancode::channel::qq {
namespace {

nlohmann::json Parse(const std::string& text) {
    return nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
}

}  // namespace

// ---------------------------------------------------------------------------
// 网关 payload
// ---------------------------------------------------------------------------

TEST_CASE("qq_proto: opcode 表全量映射") {
    CHECK(GatewayOpFromInt(0) == GatewayOp::Dispatch);
    CHECK(GatewayOpFromInt(1) == GatewayOp::Heartbeat);
    CHECK(GatewayOpFromInt(2) == GatewayOp::Identify);
    CHECK(GatewayOpFromInt(6) == GatewayOp::Resume);
    CHECK(GatewayOpFromInt(7) == GatewayOp::Reconnect);
    CHECK(GatewayOpFromInt(9) == GatewayOp::InvalidSession);
    CHECK(GatewayOpFromInt(10) == GatewayOp::Hello);
    CHECK(GatewayOpFromInt(11) == GatewayOp::HeartbeatAck);
    CHECK_FALSE(GatewayOpFromInt(3).has_value());   // 保留
    CHECK_FALSE(GatewayOpFromInt(99).has_value());  // 陌生
}

TEST_CASE("qq_proto: 官方通用结构解析(op/d/s/t 各字段)") {
    // 官方 payload 页示例形状。
    const auto payload = Parse(
        R"({"id":"event_id","op":0,"d":{},"s":42,"t":"GATEWAY_EVENT_NAME"})");
    std::string error;
    const auto parsed = ParseGatewayPayload(payload, &error);
    REQUIRE(parsed.has_value());
    CHECK(parsed->op_raw == 0);
    CHECK(parsed->op == GatewayOp::Dispatch);
    CHECK(parsed->s == 42);
    CHECK(parsed->id == "event_id");
    CHECK(parsed->t == "GATEWAY_EVENT_NAME");
    CHECK(parsed->d.is_object());
}

TEST_CASE("qq_proto: 缺 op / op 非整数 / d 非 object 一律拒绝") {
    std::string error;
    CHECK_FALSE(ParseGatewayPayload(Parse(R"({"d":{}})"), &error).has_value());
    CHECK_FALSE(ParseGatewayPayload(Parse(R"({"op":"x"})"), &error).has_value());
    CHECK_FALSE(ParseGatewayPayload(Parse(R"({"op":0,"d":[1,2]})"), &error).has_value());
    CHECK_FALSE(ParseGatewayPayload(Parse(R"(42)"), &error).has_value());
    // d 为 null 合法(心跳的 d 可以是 null)。
    CHECK(ParseGatewayPayload(Parse(R"({"op":1,"d":null})"), &error).has_value());
}

TEST_CASE("qq_proto: Hello 心跳间隔解析;缺失/非正数拒绝") {
    CHECK(ParseHelloInterval(Parse(R"({"heartbeat_interval_ms":45000})")) == 45000);
    CHECK_FALSE(ParseHelloInterval(Parse(R"({})")).has_value());
    CHECK_FALSE(ParseHelloInterval(Parse(R"({"heartbeat_interval_ms":0})")).has_value());
    CHECK_FALSE(ParseHelloInterval(Parse(R"({"heartbeat_interval_ms":"x"})")).has_value());
}

TEST_CASE("qq_proto: Identify 载荷带 QQBot 前缀与 intents") {
    const auto identify = BuildIdentify("TOKEN123", kIntentGroupAndC2cEvent);
    CHECK(identify.at("op") == 2);
    CHECK(identify.at("d").at("token") == "QQBot TOKEN123");
    CHECK(identify.at("d").at("intents") == (1u << 25));
}

TEST_CASE("qq_proto: Heartbeat 携带最新 s;没收到过事件为 null") {
    CHECK(BuildHeartbeat(42).at("d") == 42);
    CHECK(BuildHeartbeat(std::nullopt).at("d").is_null());
}

TEST_CASE("qq_proto: Resume 载荷三件套") {
    const auto resume = BuildResume("T", "SESSION", 7);
    CHECK(resume.at("op") == 6);
    CHECK(resume.at("d").at("token") == "QQBot T");
    CHECK(resume.at("d").at("session_id") == "SESSION");
    CHECK(resume.at("d").at("seq") == 7);
}

TEST_CASE("qq_proto: READY 解析 session_id 与 user.id") {
    std::string error;
    const auto ready =
        ParseReady(Parse(R"({"session_id":"abc","user":{"id":"bot1"}})"), &error);
    REQUIRE(ready.has_value());
    CHECK(ready->session_id == "abc");
    CHECK(ready->user_id == "bot1");
    CHECK_FALSE(ParseReady(Parse(R"({"user":{"id":"bot1"}})"), &error).has_value());
}

TEST_CASE("qq_proto: Invalid Session 的 d 是 bool(可否恢复)") {
    CHECK(ParseInvalidSessionResumable(Parse(R"({"op":9,"d":true})")) == true);
    CHECK(ParseInvalidSessionResumable(Parse(R"({"op":9,"d":false})")) == false);
    CHECK_FALSE(ParseInvalidSessionResumable(Parse(R"({"op":9,"d":"x"})")).has_value());
    CHECK_FALSE(ParseInvalidSessionResumable(Parse(R"({"op":9})")).has_value());
}

// ---------------------------------------------------------------------------
// RFC3339
// ---------------------------------------------------------------------------

TEST_CASE("qq_proto: RFC3339 解析(东八区/Z/小数秒)") {
    // 官方示例 timestamp: "2026-07-21T10:00:00+08:00" = 02:00:00Z。
    const auto epoch = ParseRfc3339Ms("2026-07-21T10:00:00+08:00");
    REQUIRE(epoch.has_value());
    // 1970-01-01T00:00:00Z == 0,顺推验证:与标准库对拍太重,这里拿同日
    // UTC 表示互证(两种写法必须同一时刻)。
    const auto utc = ParseRfc3339Ms("2026-07-21T02:00:00Z");
    REQUIRE(utc.has_value());
    CHECK(*epoch == *utc);
    CHECK(*epoch % 1000 == 0);
    CHECK_FALSE(ParseRfc3339Ms("2026-07-21 10:00:00+08:00").has_value());  // 缺 T
    CHECK_FALSE(ParseRfc3339Ms("2026-07-21T10:00:00").has_value());  // 缺时区
    CHECK_FALSE(ParseRfc3339Ms("not a date").has_value());
    // 小数秒。
    CHECK(ParseRfc3339Ms("2026-07-21T02:00:00.500Z").has_value());
    // 负偏移。
    CHECK(ParseRfc3339Ms("2026-07-21T02:00:00-05:00").has_value());
}

// ---------------------------------------------------------------------------
// C2C 映射(官方三例固化为 fixture)
// ---------------------------------------------------------------------------

TEST_CASE("qq_proto: 官方示例1(纯文本)映射") {
    // 原文:autogen/event/c2c_message_create.html 示例1。
    const auto d = Parse(R"({
      "id": "ROBOT1.0_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
      "author": {
        "id": "A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4",
        "user_openid": "A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4",
        "union_openid": "",
        "username": "",
        "bot": false
      },
      "content": "你好，今天有什么推荐的活动吗？",
      "message_type": 0,
      "message_scene": {"source": "default", "ext": ["msg_idx=REFIDX_xxxxxxxxxxxxxxx=="]},
      "timestamp": "2026-07-21T10:00:00+08:00"
    })");
    std::string error;
    const auto mapped = MapC2cMessageCreate(d, "EVT1", "qqbot", "main", "qq-del-1", 1000,
                                            &error);
    REQUIRE(mapped.has_value());
    const auto& event = mapped->event;
    CHECK(event.channel_id == "qqbot");
    CHECK(event.account_id == "main");
    CHECK(event.delivery_id == "qq-del-1");
    CHECK(event.received_at_ms == 1000);
    CHECK(event.provider_at_ms > 0);
    CHECK(event.conversation.kind == ConversationKind::Direct);
    CHECK(event.conversation.id == "A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4");
    CHECK(event.sender.id == "A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4");
    CHECK(event.sender.is_bot == false);
    CHECK(event.sender.is_owner == false);  // sidecar 不自称 owner
    CHECK(event.message_id == "ROBOT1.0_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    // 去重键 = msg_id + "|" + msg_idx。
    CHECK(event.provider_event_id ==
          "ROBOT1.0_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx|"
          "REFIDX_xxxxxxxxxxxxxxx==");
    REQUIRE(event.parts.size() == 1);
    CHECK(event.parts[0].type == ChannelPartType::Text);
    CHECK(event.parts[0].text == "你好，今天有什么推荐的活动吗？");
    CHECK(event.hints.mentions_bot == true);
    CHECK(mapped->warnings.empty());
}

TEST_CASE("qq_proto: 官方示例2(ARK 卡片)按摘要文本入账并留记") {
    const auto d = Parse(R"({
      "id": "ROBOT1.0_yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy",
      "author": {"id": "B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5",
                 "user_openid": "B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5",
                 "username": "", "bot": false},
      "content": "[卡片消息] 小程序\n摘要: [每日打卡]快来完成今日学习打卡",
      "message_type": 3,
      "message_scene": {"source": "default", "ext": ["msg_idx=REFIDX_yyyyyyyyyyyyyyy=="]},
      "timestamp": "2026-07-21T10:01:00+08:00"
    })");
    std::string error;
    const auto mapped = MapC2cMessageCreate(d, "EVT2", "qqbot", "main", "qq-del-2", 2000,
                                            &error);
    REQUIRE(mapped.has_value());
    CHECK(mapped->event.parts.size() == 1);
    CHECK(mapped->event.parts[0].text != "");  // 平台摘要文本照落
    CHECK(mapped->warnings.find("ark_card_not_expanded") != std::string::npos);
}

TEST_CASE("qq_proto: 官方示例3(引用消息)置 is_reply;msg_elements Q4 展开") {
    const auto d = Parse(R"({
      "id": "ROBOT1.0_zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",
      "author": {"id": "C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5F6",
                 "user_openid": "C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5F6",
                 "username": "", "bot": false},
      "content": "这个建议很有帮助，谢谢你！",
      "message_type": 103,
      "msg_elements": [{"msg_idx": "REFIDX_aaaaaaaaaaaaaaa==", "message_type": 103,
                        "content": "每天坚持阅读半小时，一个月后你会发现自己的变化"}],
      "message_scene": {"source": "default",
        "ext": ["ref_msg_idx=REFIDX_aaaaaaaaaaaaaaa==", "msg_idx=REFIDX_zzzzzzzzzzzzzzz=="]},
      "timestamp": "2026-07-21T10:02:00+08:00"
    })");
    std::string error;
    const auto mapped = MapC2cMessageCreate(d, "EVT3", "qqbot", "main", "qq-del-3", 3000,
                                            &error);
    REQUIRE(mapped.has_value());
    CHECK(mapped->event.hints.is_reply == true);
    CHECK(mapped->warnings.find("quoted_elements_deferred_to_q4") != std::string::npos);
}

TEST_CASE("qq_proto: 附件降级为 Unsupported(媒体下载 Q4,不虚报)") {
    const auto d = Parse(R"({
      "id": "M1", "author": {"user_openid": "OPEN1", "username": "", "bot": false},
      "content": "看图", "message_type": 0,
      "attachments": [{"url": "https://example.test/a.png", "filename": "a.png",
                       "size": 1024, "content_type": "image/png"}],
      "timestamp": "2026-07-21T10:00:00+08:00"
    })");
    std::string error;
    const auto mapped = MapC2cMessageCreate(d, "", "qqbot", "main", "qq-del-4", 0, &error);
    REQUIRE(mapped.has_value());
    REQUIRE(mapped->event.parts.size() == 2);
    CHECK(mapped->event.parts[1].type == ChannelPartType::Unsupported);
    CHECK(mapped->event.parts[1].file_name == "a.png");
    CHECK(mapped->event.parts[1].remote_ref == "https://example.test/a.png");
    CHECK(mapped->event.parts[1].size_bytes == 1024);
    CHECK(mapped->warnings.find("attachments_downgraded") != std::string::npos);
}

TEST_CASE("qq_proto: 缺 id/user_openid/content/未知 message_type 拒绝") {
    std::string error;
    CHECK_FALSE(MapC2cMessageCreate(Parse(R"({"content":"x"})"), "", "c", "a", "d", 0,
                                    &error)
                    .has_value());
    CHECK_FALSE(MapC2cMessageCreate(
                    Parse(R"({"id":"m1","content":"x"})"), "", "c", "a", "d", 0, &error)
                    .has_value());
    CHECK_FALSE(MapC2cMessageCreate(Parse(R"({"id":"m1","author":{"user_openid":"o1"}})"),
                                    "", "c", "a", "d", 0, &error)
                    .has_value());
    CHECK_FALSE(MapC2cMessageCreate(
                    Parse(R"({"id":"m1","author":{"user_openid":"o1"},"content":"x",
                            "message_type":101})"),
                    "", "c", "a", "d", 0, &error)
                    .has_value());
}

// ---------------------------------------------------------------------------
// token / 发送
// ---------------------------------------------------------------------------

TEST_CASE("qq_proto: token 请求体字段名(appId/clientSecret)") {
    const auto body = BuildAccessTokenRequest("APP123", "SEC456");
    CHECK(body.at("appId") == "APP123");
    CHECK(body.at("clientSecret") == "SEC456");
}

TEST_CASE("qq_proto: token 响应解析;缺字段/空值/非法 expires_in 拒绝") {
    std::string error;
    const auto ok = ParseAccessTokenResponse(
        Parse(R"({"access_token":"T1","expires_in":7200})"), &error);
    REQUIRE(ok.has_value());
    CHECK(ok->access_token == "T1");
    CHECK(ok->expires_in_secs == 7200);
    CHECK_FALSE(ParseAccessTokenResponse(Parse(R"({})"), &error).has_value());
    CHECK_FALSE(
        ParseAccessTokenResponse(Parse(R"({"access_token":"","expires_in":7200})"), &error)
            .has_value());
    CHECK_FALSE(
        ParseAccessTokenResponse(Parse(R"({"access_token":"T","expires_in":0})"), &error)
            .has_value());
}

TEST_CASE("qq_proto: 发送载荷照官方请求示例(msg_type=0/content/msg_id/msg_seq)") {
    C2cSendRequest request;
    request.openid = "A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4";
    request.content = "你好，欢迎使用机器人助手！";
    request.msg_id = "ROBOT1.0_xxx";
    request.msg_seq = 1;
    const auto payload = BuildC2cSendPayload(request);
    CHECK(payload.at("msg_type") == 0);
    CHECK(payload.at("content") == request.content);
    CHECK(payload.at("msg_id") == request.msg_id);
    CHECK(payload.at("msg_seq") == 1);
    CHECK(payload.contains("markdown") == false);
    // 主动消息(msg_id 空)不带 msg_id/msg_seq。
    C2cSendRequest proactive = request;
    proactive.msg_id.clear();
    const auto proactive_payload = BuildC2cSendPayload(proactive);
    CHECK_FALSE(proactive_payload.contains("msg_id"));
    CHECK_FALSE(proactive_payload.contains("msg_seq"));
    CHECK(C2cSendPath("OPEN1") == "/v2/users/OPEN1/messages");
}

TEST_CASE("qq_proto: 发送响应解析(官方两例)") {
    std::string error;
    const auto plain = ParseC2cSendResponse(
        Parse(R"({"id":"ROBOT1.0_out1","timestamp":"2026-07-21T10:30:00+08:00"})"),
        &error);
    REQUIRE(plain.has_value());
    CHECK(plain->provider_message_id == "ROBOT1.0_out1");
    CHECK(plain->ref_idx.empty());
    const auto with_ref = ParseC2cSendResponse(
        Parse(R"({"id":"ROBOT1.0_out2","timestamp":"2026-07-21T10:30:00+08:00",
                  "ext_info":{"ref_idx":"REFIDX_xxxxxxxxxxxxxxxxxxxx=="}})"),
        &error);
    REQUIRE(with_ref.has_value());
    CHECK(with_ref->ref_idx == "REFIDX_xxxxxxxxxxxxxxxxxxxx==");
    CHECK_FALSE(ParseC2cSendResponse(Parse(R"({"timestamp":"x"})"), &error).has_value());
}

// ---------------------------------------------------------------------------
// 错误分型(官方错误码表全量钉)
// ---------------------------------------------------------------------------

TEST_CASE("qq_proto: 错误码分型全表") {
    struct Case {
        int status;
        std::int64_t code;
        QqApiErrorKind kind;
    };
    const Case cases[] = {
        {200, 40034100, QqApiErrorKind::RateLimited},
        {429, 0, QqApiErrorKind::RateLimited},
        {200, 304103, QqApiErrorKind::MsgIdExpired},
        {200, 40034005, QqApiErrorKind::MsgIdExpired},
        {200, 40034128, QqApiErrorKind::MsgIdExpired},
        {200, 40054005, QqApiErrorKind::Deduped},
        {200, 40054004, QqApiErrorKind::NoFriend},
        {200, 40054013, QqApiErrorKind::UserRejected},
        {200, 40034006, QqApiErrorKind::ContentRejected},
        {200, 304061, QqApiErrorKind::ContentRejected},
        {401, 0, QqApiErrorKind::Unauthorized},
        {403, 0, QqApiErrorKind::Unauthorized},
        {200, 50055002, QqApiErrorKind::ServerError},
        {503, 0, QqApiErrorKind::ServerError},
        {400, 99999, QqApiErrorKind::UnknownError},
    };
    for (const auto& item : cases) {
        const std::string body =
            item.code == 0 ? std::string("ignored")
                           : R"({"code":)" + std::to_string(item.code) +
                                 R"(,"message":"m"})";
        const auto error = ClassifyQqSendFailure(item.status, body);
        CHECK(error.kind == item.kind);
        CHECK(error.platform_code == item.code);
        CHECK(error.http_status == item.status);
    }
}

TEST_CASE("qq_proto: 错误体 message 进 detail;坏 body 不炸") {
    const auto error = ClassifyQqSendFailure(200, R"({"code":40034100,"message":"主动频控"})");
    CHECK(error.kind == QqApiErrorKind::RateLimited);
    CHECK(error.detail == "主动频控");
    const auto garbage = ClassifyQqSendFailure(500, "<html>oops</html>");
    CHECK(garbage.kind == QqApiErrorKind::ServerError);
    CHECK(garbage.platform_code == 0);
}

}  // namespace lubancode::channel::qq
