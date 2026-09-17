// 企微协议纯函数册(W1,设计单 §六协议细账)。帧构造/入站解析/消息映射/
// 事件解析/错误分型/分段:全纯函数直钉,零 IO 零外联。报文形状按官方
// 长连接文档(101463/100719/101138,2026-09-17 核读)钉死;错误码表按
// 设计单初版表(真机核验归 W2)。fixture 一律 nlohmann 现建现 dump——
// 手拼原始串数花括号在 CI 上翻过车,结构交给类型系统兜。
#include <doctest/doctest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "channel/wecombot/wecom_proto.hpp"

namespace lubancode::channel::wecombot {
namespace {

nlohmann::json ParseOk(const std::string& text) {
    const auto parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    REQUIRE_FALSE(parsed.is_discarded());
    return parsed;
}

// msg_callback 的 body(single/text 最小例)。
nlohmann::json MsgCallbackBody(const char* msgid, const char* userid, const char* content) {
    return nlohmann::json{
        {"msgid", msgid},
        {"aibotid", "BOT1"},
        {"chattype", "single"},
        {"from", {{"userid", userid}}},
        {"msgtype", "text"},
        {"text", {{"content", content}}},
    };
}

// 官方形状的 msg_callback 整帧。
std::string MsgCallback(const char* msgid, const char* userid, const char* content) {
    return nlohmann::json{
        {"cmd", "aibot_msg_callback"},
        {"headers", {{"req_id", "REQ1"}}},
        {"body", MsgCallbackBody(msgid, userid, content)},
    }.dump();
}

}  // namespace

// ---------------------------------------------------------------------------
// 出站帧构造
// ---------------------------------------------------------------------------

TEST_CASE("wecom_proto: subscribe/ping/respond 帧形状(官方三段式)") {
    const std::string subscribe = BuildSubscribeFrame("BOT1", "SECRET1", "REQ1");
    const auto json = ParseOk(subscribe);
    CHECK(json.at("cmd") == "aibot_subscribe");
    CHECK(json.at("headers").at("req_id") == "REQ1");
    CHECK(json.at("body").at("bot_id") == "BOT1");
    CHECK(json.at("body").at("secret") == "SECRET1");

    const auto ping = ParseOk(BuildPingFrame("REQ2"));
    CHECK(ping.at("cmd") == "ping");
    CHECK(ping.at("headers").at("req_id") == "REQ2");
    CHECK_FALSE(ping.contains("body"));

    const auto respond = ParseOk(BuildRespondFrame("REQ3", "你好\n世界"));
    CHECK(respond.at("cmd") == "aibot_respond_msg");
    CHECK(respond.at("headers").at("req_id") == "REQ3");
    CHECK(respond.at("body").at("msgtype") == "markdown");
    CHECK(respond.at("body").at("markdown").at("content") == "你好\n世界");
}

// ---------------------------------------------------------------------------
// 入站帧解析
// ---------------------------------------------------------------------------

TEST_CASE("wecom_proto: 回执帧(errcode/req_id 透传)与两路 callback 分类") {
    std::string error;
    const auto ack = ParseWecomInboundFrame(
        ParseOk(R"({"headers":{"req_id":"REQ9"},"errcode":0,"errmsg":"ok"})"), &error);
    REQUIRE(ack.has_value());
    CHECK(ack->kind == WecomFrameKind::Ack);
    CHECK(ack->req_id == "REQ9");
    REQUIRE(ack->errcode.has_value());
    CHECK(*ack->errcode == 0);
    CHECK(ack->errmsg == "ok");

    const auto msg = ParseWecomInboundFrame(ParseOk(MsgCallback("MSG1", "U1", "hi")), &error);
    REQUIRE(msg.has_value());
    CHECK(msg->kind == WecomFrameKind::MsgCallback);
    CHECK(msg->cmd == "aibot_msg_callback");
    CHECK(msg->req_id == "REQ1");
    CHECK(msg->body.at("msgid") == "MSG1");

    const auto event = ParseWecomInboundFrame(
        ParseOk(nlohmann::json{{"cmd", "aibot_event_callback"},
                               {"headers", {{"req_id", "REQ2"}}},
                               {"body", {{"msgid", "E1"},
                                         {"event", {{"eventtype", "enter_chat"}}}}}}
                    .dump()),
        &error);
    REQUIRE(event.has_value());
    CHECK(event->kind == WecomFrameKind::EventCallback);
    CHECK(event->body.at("event").at("eventtype") == "enter_chat");

    const auto unknown = ParseWecomInboundFrame(
        ParseOk(nlohmann::json{{"cmd", "aibot_send_msg"},
                               {"headers", {{"req_id", "REQ3"}}},
                               {"body", nlohmann::json::object()}}
                    .dump()),
        &error);
    REQUIRE(unknown.has_value());
    CHECK(unknown->kind == WecomFrameKind::Unknown);
}

TEST_CASE("wecom_proto: 坏形状拒绝(非 object/errcode 非整数/无 cmd 无 errcode)") {
    std::string error;
    CHECK_FALSE(ParseWecomInboundFrame(nlohmann::json::array({1, 2}), &error).has_value());
    CHECK_FALSE(ParseWecomInboundFrame(
                    ParseOk(R"({"headers":{"req_id":"R"},"errcode":"0"})"), &error)
                    .has_value());
    CHECK_FALSE(ParseWecomInboundFrame(ParseOk(R"({"headers":{"req_id":"R"}})"), &error)
                    .has_value());
    CHECK_FALSE(ParseWecomInboundFrame(ParseOk(R"({"cmd":""})"), &error).has_value());
}

// ---------------------------------------------------------------------------
// msg_callback → ChannelInboundEvent
// ---------------------------------------------------------------------------

TEST_CASE("wecom_proto: single/text 映射——身份键/会话键/排重键") {
    std::string error;
    const auto mapping = MapMsgCallback(MsgCallbackBody("MSG1", "USER1", "在么"),
                                        "wecombot", "main", "BOT1", "DEL1", 1234, &error);
    REQUIRE(mapping.has_value());
    const ChannelInboundEvent& event = mapping->event;
    CHECK(event.channel_id == "wecombot");
    CHECK(event.account_id == "main");
    CHECK(event.delivery_id == "DEL1");
    CHECK(event.provider_event_id == "MSG1");
    CHECK(event.message_id == "MSG1");
    CHECK(event.received_at_ms == 1234);
    CHECK(event.conversation.kind == ConversationKind::Direct);
    CHECK(event.conversation.id == "USER1");
    CHECK(event.sender.id == "USER1");
    CHECK_FALSE(event.sender.is_bot);
    REQUIRE(event.parts.size() == 1);
    CHECK(event.parts[0].type == ChannelPartType::Text);
    CHECK(event.parts[0].text == "在么");
    CHECK(event.hints.mentions_bot);
}

TEST_CASE("wecom_proto: group 映射——conversation=chatid,sender=userid") {
    std::string error;
    const auto body = nlohmann::json{
        {"msgid", "MG1"},
        {"aibotid", "BOT1"},
        {"chatid", "CHAT1"},
        {"chattype", "group"},
        {"from", {{"userid", "U1"}}},
        {"msgtype", "text"},
        {"text", {{"content", "群里说"}}},
    };
    const auto mapping = MapMsgCallback(body, "wecombot", "main", "BOT1", "DEL1", 1, &error);
    REQUIRE(mapping.has_value());
    CHECK(mapping->event.conversation.kind == ConversationKind::Group);
    CHECK(mapping->event.conversation.id == "CHAT1");
    CHECK(mapping->event.sender.id == "U1");
}

TEST_CASE("wecom_proto: voice 转写文本进正文;warnings 记账") {
    std::string error;
    const auto body = nlohmann::json{
        {"msgid", "MV1"},
        {"aibotid", "BOT1"},
        {"chattype", "single"},
        {"from", {{"userid", "U1"}}},
        {"msgtype", "voice"},
        {"voice", {{"content", "转写的话"}}},
    };
    const auto mapping = MapMsgCallback(body, "wecombot", "main", "BOT1", "DEL1", 1, &error);
    REQUIRE(mapping.has_value());
    REQUIRE(mapping->event.parts.size() == 1);
    CHECK(mapping->event.parts[0].type == ChannelPartType::Text);
    CHECK(mapping->event.parts[0].text == "转写的话");
    CHECK(mapping->warnings.find("voice") != std::string::npos);
}

TEST_CASE("wecom_proto: mixed 抽 text 项,其余项落占位 part") {
    std::string error;
    const auto body = nlohmann::json{
        {"msgid", "MM1"},
        {"aibotid", "BOT1"},
        {"chattype", "single"},
        {"from", {{"userid", "U1"}}},
        {"msgtype", "mixed"},
        {"msg_item", nlohmann::json::array(
                         {nlohmann::json{{"msgtype", "text"},
                                         {"text", {{"content", "第一段"}}}},
                          nlohmann::json{{"msgtype", "image"},
                                         {"image", {{"url", "https://x"}, {"aeskey", "K"}}}},
                          nlohmann::json{{"msgtype", "text"},
                                         {"text", {{"content", "第二段"}}}}})},
    };
    const auto mapping = MapMsgCallback(body, "wecombot", "main", "BOT1", "DEL1", 1, &error);
    REQUIRE(mapping.has_value());
    const auto& parts = mapping->event.parts;
    REQUIRE(parts.size() == 3);
    CHECK(parts[0].type == ChannelPartType::Text);
    CHECK(parts[0].text == "第一段");
    CHECK(parts[1].type == ChannelPartType::Unsupported);
    CHECK(parts[1].unsupported_reason.has_value());
    CHECK(parts[2].type == ChannelPartType::Text);
    CHECK(parts[2].text == "第二段");
}

TEST_CASE("wecom_proto: image/file/video 落占位 part(媒体归 W2,不虚报)") {
    for (const char* msgtype : {"image", "file", "video"}) {
        std::string error;
        const auto body = nlohmann::json{
            {"msgid", "MI1"},
            {"aibotid", "BOT1"},
            {"chattype", "single"},
            {"from", {{"userid", "U1"}}},
            {"msgtype", msgtype},
            {msgtype, {{"url", "https://x"}, {"aeskey", "K"}}},
        };
        const auto mapping =
            MapMsgCallback(body, "wecombot", "main", "BOT1", "DEL1", 1, &error);
        REQUIRE(mapping.has_value());
        REQUIRE(mapping->event.parts.size() == 1);
        CHECK(mapping->event.parts[0].type == ChannelPartType::Unsupported);
        REQUIRE(mapping->event.parts[0].unsupported_reason.has_value());
        CHECK(mapping->event.parts[0].unsupported_reason->find(msgtype) != std::string::npos);
    }
}

TEST_CASE("wecom_proto: quote 立回复标志;合法 body 字段往返") {
    std::string error;
    const auto body = nlohmann::json{
        {"msgid", "MQ1"},
        {"aibotid", "BOT1"},
        {"chattype", "single"},
        {"from", {{"userid", "U1"}}},
        {"msgtype", "text"},
        {"text", {{"content", "引用说"}}},
        {"quote", {{"msgid", "OLD1"}}},
    };
    const auto mapping = MapMsgCallback(body, "wecombot", "main", "BOT1", "DEL1", 1, &error);
    REQUIRE(mapping.has_value());
    CHECK(mapping->event.hints.is_reply);
    // 规范化事件 JSON 往返(严格解析再过一遍——spool 落的就是这份)。
    const nlohmann::json event_json = mapping->event.ToJson();
    std::string roundtrip_error;
    const auto reparsed = ChannelInboundEvent::FromJsonStrict(event_json, &roundtrip_error);
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->message_id == "MQ1");
}

TEST_CASE("wecom_proto: 映射拒绝面——缺 msgid/缺 from.userid/群缺 chatid/"
          "chattype 不认/未知 msgtype/aibotid 对不上") {
    std::string error;
    auto expect_error = [&](const nlohmann::json& body) {
        const auto mapping =
            MapMsgCallback(body, "wecombot", "main", "BOT1", "DEL1", 1, &error);
        CHECK_FALSE(mapping.has_value());
        CHECK_FALSE(error.empty());
        error.clear();
    };
    expect_error(nlohmann::json{{"chattype", "single"},
                                {"from", {{"userid", "U1"}}},
                                {"msgtype", "text"},
                                {"text", {{"content", "x"}}}});  // 缺 msgid
    expect_error(MsgCallbackBody("M1", "", "x"));                // 缺 from.userid
    expect_error(nlohmann::json{{"msgid", "M1"},
                                {"chattype", "group"},           // 群缺 chatid
                                {"from", {{"userid", "U1"}}},
                                {"msgtype", "text"},
                                {"text", {{"content", "x"}}}});
    expect_error(nlohmann::json{{"msgid", "M1"},
                                {"chattype", "channel"},         // chattype 不认
                                {"from", {{"userid", "U1"}}},
                                {"msgtype", "text"},
                                {"text", {{"content", "x"}}}});
    expect_error(nlohmann::json{{"msgid", "M1"},
                                {"chattype", "single"},
                                {"from", {{"userid", "U1"}}},
                                {"msgtype", "location"}});       // 未知 msgtype
    // aibotid 与连接 bot_id 对不上 → 拒(别家 bot 的事件不进本账号)。
    const auto mismatch = MapMsgCallback(MsgCallbackBody("M1", "U1", "x"),
                                         "wecombot", "main", "OTHER", "DEL1", 1, &error);
    CHECK_FALSE(mismatch.has_value());
    CHECK(error.find("aibotid") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 事件回调
// ---------------------------------------------------------------------------

TEST_CASE("wecom_proto: 事件回调解析与 disconnected_event 判定") {
    std::string error;
    const auto feedback = nlohmann::json{
        {"msgid", "E1"},
        {"create_time", 1},
        {"aibotid", "BOT1"},
        {"msgtype", "event"},
        {"event", {{"eventtype", kWecomEventFeedback}}},
    };
    const auto info = ParseEventCallback(feedback, &error);
    REQUIRE(info.has_value());
    CHECK(info->eventtype == kWecomEventFeedback);
    CHECK(info->msgid == "E1");

    const auto disconnected =
        nlohmann::json{{"event", {{"eventtype", kWecomEventDisconnected}}}};
    const auto kicked = ParseEventCallback(disconnected, &error);
    REQUIRE(kicked.has_value());
    CHECK(kicked->eventtype == kWecomEventDisconnected);

    CHECK_FALSE(ParseEventCallback(nlohmann::json{{"event", nlohmann::json::object()}},
                                   &error)
                    .has_value());
    CHECK_FALSE(ParseEventCallback(nlohmann::json::array(), &error).has_value());
}

// ---------------------------------------------------------------------------
// 错误分型表
// ---------------------------------------------------------------------------

TEST_CASE("wecom_proto: 订阅回执分型——凭据族即止,其余退避") {
    CHECK(ClassifySubscribeErrcode(0) == WecomSubscribeStatus::Ok);
    for (const std::int64_t code : {40013LL, 40014LL, 40029LL, 41001LL, 41004LL}) {
        CHECK(ClassifySubscribeErrcode(code) == WecomSubscribeStatus::CredentialRejected);
    }
    CHECK(ClassifySubscribeErrcode(-1) == WecomSubscribeStatus::Transient);
    CHECK(ClassifySubscribeErrcode(45009) == WecomSubscribeStatus::Transient);
    CHECK(ClassifySubscribeErrcode(50000) == WecomSubscribeStatus::Transient);
}

TEST_CASE("wecom_proto: respond 回执分型——限流可重试,其余永久拒") {
    CHECK(ClassifyRespondErrcode(0) == WecomRespondStatus::Ok);
    CHECK(ClassifyRespondErrcode(45009) == WecomRespondStatus::RateLimited);
    CHECK(ClassifyRespondErrcode(45011) == WecomRespondStatus::RateLimited);
    CHECK(ClassifyRespondErrcode(40058) == WecomRespondStatus::Rejected);
}

// ---------------------------------------------------------------------------
// markdown 分段
// ---------------------------------------------------------------------------

TEST_CASE("wecom_proto: UTF-8 分段——字节帽与码点边界") {
    // ASCII:帽 10 → 两段 10+5。
    const auto ascii = SplitUtf8Chunks("aaaabbbbcccccddddd", 10);
    REQUIRE(ascii.size() == 2);
    CHECK(ascii[0] == "aaaabbbbcc");
    CHECK(ascii[1] == "cccddddd");

    // 中文(3 字节/字):帽 10 → 3+3+1 字,不截半个字。
    const std::string chinese = "一二三四五六七八";
    const auto split = SplitUtf8Chunks(chinese, 10);
    REQUIRE(split.size() == 3);
    CHECK(split[0] == "一二三");
    CHECK(split[1] == "四五六");
    CHECK(split[2] == "七八");

    // 帽内不分段;空文本单空段。
    CHECK(SplitUtf8Chunks("短文", 20480).size() == 1);
    const auto empty = SplitUtf8Chunks("", 100);
    REQUIRE(empty.size() == 1);
    CHECK(empty[0].empty());

    // emoji(4 字节)贴帽切分。
    const std::string emoji = "\xF0\x9F\x98\x80\xF0\x9F\x98\x80";
    const auto emoji_split = SplitUtf8Chunks(emoji, 4);
    REQUIRE(emoji_split.size() == 2);
    CHECK(emoji_split[0] == "\xF0\x9F\x98\x80");
}

}  // namespace lubancode::channel::wecombot
