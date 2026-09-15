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

TEST_CASE("qq_proto: 缺 op / op 非整数 / 非 object 拒绝;d 形状随 op 不限") {
    std::string error;
    CHECK_FALSE(ParseGatewayPayload(Parse(R"({"d":{}})"), &error).has_value());
    CHECK_FALSE(ParseGatewayPayload(Parse(R"({"op":"x"})"), &error).has_value());
    CHECK_FALSE(ParseGatewayPayload(Parse(R"(42)"), &error).has_value());
    // d 形状不限(Hello/READY object、Heartbeat 数字、Invalid Session bool、
    // 心跳未收到事件时 null)——通用层不拦,各 op 解析函数自校。
    CHECK(ParseGatewayPayload(Parse(R"({"op":1,"d":null})"), &error).has_value());
    CHECK(ParseGatewayPayload(Parse(R"({"op":1,"d":42})"), &error).has_value());
    CHECK(ParseGatewayPayload(Parse(R"({"op":9,"d":false})"), &error).has_value());
    CHECK(ParseGatewayPayload(Parse(R"({"op":0,"d":[1,2]})"), &error).has_value());
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

TEST_CASE("qq_proto: 附件按官方 content_type 映射真类型(Q4,不再降级)") {
    const auto d = Parse(R"({
      "id": "M1", "author": {"user_openid": "OPEN1", "username": "", "bot": false},
      "content": "看图", "message_type": 0,
      "attachments": [{"url": "https://example.test/a.png", "filename": "a.png",
                       "size": 1024, "content_type": "image/png"},
                      {"url": "https://example.test/v.mp4", "filename": "v.mp4",
                       "size": "2048", "content_type": "video/mp4"},
                      {"url": "https://example.test/w.silk", "filename": "w",
                       "content_type": "voice"},
                      {"url": "https://example.test/d.bin", "filename": "../d.bin",
                       "content_type": "file"}],
      "timestamp": "2026-07-21T10:00:00+08:00"
    })");
    std::string error;
    const auto mapped = MapC2cMessageCreate(d, "", "qqbot", "main", "qq-del-4", 0, &error);
    REQUIRE(mapped.has_value());
    REQUIRE(mapped->event.parts.size() == 5);
    CHECK(mapped->event.parts[1].type == ChannelPartType::Image);
    CHECK(mapped->event.parts[1].file_name == "a.png");
    CHECK(mapped->event.parts[1].remote_ref == "https://example.test/a.png");
    CHECK(mapped->event.parts[1].size_bytes == 1024);
    CHECK(mapped->event.parts[1].mime_type == "image/png");
    // size 以字符串回传也解析(真机教训:平台数值字段两态)。
    CHECK(mapped->event.parts[2].type == ChannelPartType::Video);
    CHECK(mapped->event.parts[2].size_bytes == 2048);
    // 平台枚举的非 MIME 值:voice -> Audio,file -> File。
    CHECK(mapped->event.parts[3].type == ChannelPartType::Audio);
    CHECK(mapped->event.parts[3].mime_type == "voice");
    CHECK_FALSE(mapped->event.parts[3].size_bytes.has_value());
    CHECK(mapped->event.parts[4].type == ChannelPartType::File);
    CHECK(mapped->event.parts[4].mime_type == "file");
    CHECK(mapped->warnings.find("attachments_pending_download") != std::string::npos);
}

TEST_CASE("qq_proto: Q7 命令前缀识别(hints.command/command_args;/起头文本)") {
    // 菜单 send_message / 面板 command 点击填入的文本(官方:填入输入框,
    // 用户发送后成为聊天指令)与手敲指令同走 hints.command 识别位。
    std::string error;
    const auto mapped = MapC2cMessageCreate(
        Parse(R"({
          "id": "M-Q7-1", "author": {"user_openid": "OPEN1", "username": "", "bot": false},
          "content": " /help 今晚的安排 ", "message_type": 0
        })"),
        "", "qqbot", "main", "qq-del-q7", 0, &error);
    REQUIRE(mapped.has_value());
    CHECK(mapped->event.hints.command == std::optional<std::string>("help"));
    CHECK(mapped->event.hints.command_args == std::optional<std::string>("今晚的安排"));

    // 中文命令名同样识别(/帮助)。
    const auto zh = MapC2cMessageCreate(
        Parse(R"({
          "id": "M-Q7-2", "author": {"user_openid": "OPEN1", "username": "", "bot": false},
          "content": "/查看任务", "message_type": 0
        })"),
        "", "qqbot", "main", "qq-del-q7b", 0, &error);
    REQUIRE(zh.has_value());
    CHECK(zh->event.hints.command == std::optional<std::string>("查看任务"));
    CHECK_FALSE(zh->event.hints.command_args.has_value());

    // 非命令文本(无斜杠/裸斜杠)不填识别位。
    const auto plain = MapC2cMessageCreate(
        Parse(R"({
          "id": "M-Q7-3", "author": {"user_openid": "OPEN1", "username": "", "bot": false},
          "content": "随便聊聊", "message_type": 0
        })"),
        "", "qqbot", "main", "qq-del-q7c", 0, &error);
    REQUIRE(plain.has_value());
    CHECK_FALSE(plain->event.hints.command.has_value());
    const auto bare = MapC2cMessageCreate(
        Parse(R"({
          "id": "M-Q7-4", "author": {"user_openid": "OPEN1", "username": "", "bot": false},
          "content": "/", "message_type": 0
        })"),
        "", "qqbot", "main", "qq-del-q7d", 0, &error);
    REQUIRE(bare.has_value());
    CHECK_FALSE(bare->event.hints.command.has_value());
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

// 真机教训(2026-09-15 用户 Q3 实测):QQ 平台数值字段会以数字字符串回传,
// expires_in "7200" 与 7200 同义;写死 is_number 的解析把令牌链卡死。
TEST_CASE("qq_proto: 宽松整数解析——数字字符串与数字同收") {
    CHECK(ParseLooseInt64(nlohmann::json(7200)) == std::int64_t{7200});
    CHECK(ParseLooseInt64(nlohmann::json("7200")) == std::int64_t{7200});
    CHECK(ParseLooseInt64(nlohmann::json(" 7200 ")) == std::int64_t{7200});
    CHECK(ParseLooseInt64(nlohmann::json("-30")) == std::int64_t{-30});
    CHECK_FALSE(ParseLooseInt64(nlohmann::json("abc")).has_value());
    CHECK_FALSE(ParseLooseInt64(nlohmann::json("7200x")).has_value());
    CHECK_FALSE(ParseLooseInt64(nlohmann::json("")).has_value());
    CHECK_FALSE(ParseLooseInt64(nlohmann::json("   ")).has_value());
    CHECK_FALSE(ParseLooseInt64(nlohmann::json(1.5)).has_value());
    CHECK_FALSE(ParseLooseInt64(nlohmann::json(true)).has_value());
    CHECK_FALSE(ParseLooseInt64(nlohmann::json(nullptr)).has_value());

    std::string error;
    const auto as_string = ParseAccessTokenResponse(
        Parse(R"({"access_token":"T2","expires_in":"7200"})"), &error);
    REQUIRE(as_string.has_value());
    CHECK(as_string->expires_in_secs == 7200);
    CHECK_FALSE(ParseAccessTokenResponse(
                    Parse(R"({"access_token":"T","expires_in":"soon"})"), &error)
                    .has_value());
    const auto hello_string = ParseHelloInterval(
        Parse(R"({"heartbeat_interval_ms":"41250"})"));
    REQUIRE(hello_string.has_value());
    CHECK(*hello_string == 41250);
    const auto payload_seq_string =
        ParseGatewayPayload(Parse(R"({"op":0,"s":"42","t":"C2C_MESSAGE_CREATE"})"), &error);
    REQUIRE(payload_seq_string.has_value());
    CHECK(payload_seq_string->s == std::int64_t{42});
    const auto classified =
        ClassifyQqSendFailure(200, R"({"code":"40034100","message":"主动频控"})");
    CHECK(classified.platform_code == std::int64_t{40034100});
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

// ---------------------------------------------------------------------------
// Q4 富媒体协议面
// ---------------------------------------------------------------------------

TEST_CASE("qq_proto: 富媒体发送载荷(msg_type=7 只带 media,不带 content)") {
    C2cSendRequest request;
    request.openid = "OPEN1";
    request.content = "正文不应出现在富媒体载荷";
    request.msg_id = "M1";
    request.msg_seq = 2;
    request.media_file_info = "FILE_INFO_OPAQUE";
    const auto payload = BuildC2cSendPayload(request);
    CHECK(payload.at("msg_type") == 7);
    CHECK(payload.at("media").at("file_info") == "FILE_INFO_OPAQUE");
    CHECK_FALSE(payload.contains("content"));  // 官方示例:文本与附件不混一条
    CHECK(payload.at("msg_id") == "M1");
    CHECK(payload.at("msg_seq") == 2);
}

TEST_CASE("qq_proto: upload_prepare 请求体(file_size 字符串/md5_10m 字段名)") {
    const auto body = BuildUploadPrepareRequest(4, 31457280, "report.txt",
                                                "d41d8c", "da39a3", "0d3f1a");
    CHECK(body.at("file_type") == 4);
    // 官方字段表:file_size 是字符串(SDK 1.0.4 发 number 属漂移,不采信)。
    CHECK(body.at("file_size") == "31457280");
    CHECK(body.at("file_name") == "report.txt");
    CHECK(body.at("md5") == "d41d8c");
    CHECK(body.at("sha1") == "da39a3");
    CHECK(body.at("md5_10m") == "0d3f1a");
}

TEST_CASE("qq_proto: upload_prepare 响应解析(index 0 起与 1 起都稳;嵌套 upload_config)") {
    std::string error;
    // 官方文档页:index"从 0 开始",block_size 为字符串,配置在 upload_config 下。
    const auto zero_based = ParseUploadPrepareResponse(Parse(R"({
      "upload_id": "UP1", "block_size": "10485760",
      "parts": [{"index": 0, "presigned_url": "https://cos.test/p0?sign=A", "block_size": "10485760"},
                {"index": 1, "presigned_url": "https://cos.test/p1?sign=B", "block_size": "5242880"}],
      "upload_config": {"concurrency": 2, "retry_timeout": 300, "retry_delay": 1}
    })"), &error);
    REQUIRE(zero_based.has_value());
    CHECK(zero_based->upload_id == "UP1");
    CHECK(zero_based->block_size == 10485760);
    REQUIRE(zero_based->parts.size() == 2);
    CHECK(zero_based->parts[0].index == 0);
    CHECK(zero_based->parts[0].block_size == 10485760);
    CHECK(zero_based->parts[1].index == 1);
    CHECK(zero_based->parts[1].block_size == 5242880);
    CHECK(zero_based->concurrency == 2);
    CHECK(zero_based->retry_timeout_secs == 300);
    CHECK(zero_based->retry_delay_secs == 1);
    // SDK 1.0.4 漂移案:index 从 1 起——解析不拒(原值保留回显,上传偏移
    // 按数组序,不按 index 值;§十 10.1 显式适配)。
    const auto one_based = ParseUploadPrepareResponse(Parse(R"({
      "upload_id": "UP2", "block_size": 10485760,
      "parts": [{"index": 1, "presigned_url": "https://cos.test/p1?sign=A", "block_size": 10485760},
                {"index": 2, "presigned_url": "https://cos.test/p2?sign=B", "block_size": 5242880}]
    })"), &error);
    REQUIRE(one_based.has_value());
    CHECK(one_based->parts[0].index == 1);
    CHECK(one_based->parts[1].index == 2);
    CHECK(one_based->concurrency == 1);  // upload_config 缺省:恒串行
    // 坏形状:缺 upload_id / 空 parts / 缺 presigned_url 拒绝。
    CHECK_FALSE(ParseUploadPrepareResponse(Parse(R"({"block_size":"1","parts":[{"index":0,
      "presigned_url":"u","block_size":"1"}]})"), &error).has_value());
    CHECK_FALSE(ParseUploadPrepareResponse(Parse(R"({"upload_id":"U","block_size":"1",
      "parts":[]})"), &error).has_value());
    CHECK_FALSE(ParseUploadPrepareResponse(Parse(R"({"upload_id":"U","block_size":"1",
      "parts":[{"index":0,"block_size":"1"}]})"), &error).has_value());
}

TEST_CASE("qq_proto: 分片完成请求与 files 合并请求(srv_send_msg 恒 false)") {
    const auto finish = BuildUploadPartFinishRequest("UP1", 0, 10485760, "abc123");
    CHECK(finish.at("upload_id") == "UP1");
    CHECK(finish.at("part_index") == 0);
    CHECK(finish.at("block_size") == "10485760");
    CHECK(finish.at("md5") == "abc123");
    const auto merge = BuildFileUploadBody(4, "report.txt", "UP1");
    CHECK(merge.at("file_type") == 4);
    CHECK(merge.at("file_name") == "report.txt");
    CHECK(merge.at("upload_id") == "UP1");
    CHECK(merge.at("srv_send_msg") == false);  // 可靠 outbox 不占主动消息频次
    CHECK(UploadPreparePath("O") == "/v2/users/O/upload_prepare");
    CHECK(UploadPartFinishPath("O") == "/v2/users/O/upload_part_finish");
    CHECK(FileUploadPath("O") == "/v2/users/O/files");
}

TEST_CASE("qq_proto: files 响应解析(file_info 透传/ttl 宽松两态)") {
    std::string error;
    const auto numeric = ParseFileUploadResponse(Parse(R"({
      "file_uuid": "F1", "file_info": "opaque-info", "ttl": 3600
    })"), &error);
    REQUIRE(numeric.has_value());
    CHECK(numeric->file_info == "opaque-info");
    CHECK(numeric->file_uuid == "F1");
    CHECK(numeric->ttl_secs == 3600);
    const auto text_ttl = ParseFileUploadResponse(Parse(R"({
      "file_uuid": "F2", "file_info": "opaque-2", "ttl": "7200"
    })"), &error);
    REQUIRE(text_ttl.has_value());
    CHECK(text_ttl->ttl_secs == 7200);
    CHECK_FALSE(ParseFileUploadResponse(Parse(R"({"file_uuid":"F"})"), &error).has_value());
}

TEST_CASE("qq_proto: 媒体错误码分型(850019/850031 永久;850026/40093001 可重试)") {
    const auto unsupported = ClassifyQqSendFailure(
        200, R"({"code":850019,"message":"不支持的文件格式"})");
    CHECK(unsupported.kind == QqApiErrorKind::ContentRejected);
    const auto too_large = ClassifyQqSendFailure(200, R"({"code":850031,"message":"超大小"})");
    CHECK(too_large.kind == QqApiErrorKind::ContentRejected);
    const auto fetch_failed = ClassifyQqSendFailure(200, R"({"code":850026,"message":"下载失败"})");
    CHECK(fetch_failed.kind == QqApiErrorKind::ServerError);
    const auto bdh = ClassifyQqSendFailure(200, R"({"code":40093001,"message":"通道异常"})");
    CHECK(bdh.kind == QqApiErrorKind::ServerError);
}

}  // namespace lubancode::channel::qq
