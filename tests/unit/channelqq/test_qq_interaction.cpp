// QQ 接入单 Q6 协议册:互动回调(INTERACTION_CREATE)与审批键盘的
// 纯函数面——事件映射(官方互动事件页三例为 fixture 基准)、回应载荷、
// 发送载荷的键盘分支、intents 位。
//
// 真平台按钮触达/回调真实形状归 Q3 真机未验(todo §12.1/§14);这里钉
// 的是"官方文档形状 ↔ 代码映射"的契约,与旧 msg_type 0/7 行为零回归。
#include <doctest/doctest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "channel/qq/qq_proto.hpp"

using namespace lubancode::channel::qq;

namespace {

// 官方互动事件页示例 1(单聊按钮点击)的形状基准(字段拼写逐字对文档)。
nlohmann::json OfficialC2cInteractionD() {
    return nlohmann::json::parse(R"({
        "application_id": "1904842048",
        "chat_type": 2,
        "data": {"resolved": {"button_data": "confirm:once", "button_id": "allow-once"}, "type": 11},
        "id": "1b13d569-4610-4ab9-bc51-feecc5def6d4",
        "scene": "c2c",
        "timestamp": "2026-07-20T21:53:54+08:00",
        "type": 11,
        "user_openid": "A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4",
        "version": 1
    })");
}

}  // namespace

TEST_CASE("intents:互动位并进默认订阅,单聊位不变") {
    CHECK(kIntentInteraction == (1u << 26));
    CHECK(kIntentGroupAndC2cEvent == (1u << 25));
    // 默认订阅 = 单聊 + 互动(Q6):两个位都在,不误伤彼此。
    CHECK((kIntentDefaultBot & kIntentGroupAndC2cEvent) != 0);
    CHECK((kIntentDefaultBot & kIntentInteraction) != 0);
    CHECK(kIntentDefaultBot == (kIntentGroupAndC2cEvent | kIntentInteraction));
}

TEST_CASE("MapInteractionCreate:官方单聊按钮回调形状逐字段映射") {
    std::string error;
    const auto event = MapInteractionCreate(OfficialC2cInteractionD(), &error);
    REQUIRE(event.has_value());
    CHECK(event->interaction_id == "1b13d569-4610-4ab9-bc51-feecc5def6d4");
    CHECK(event->type == 11);
    CHECK(event->scene == "c2c");
    CHECK(event->chat_type == 2);
    CHECK(event->user_openid == "A1B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4");
    CHECK(event->button_data == "confirm:once");
    CHECK(event->button_id == "allow-once");
    CHECK(event->application_id == "1904842048");
    CHECK(event->version == 1);
    CHECK(event->group_openid.empty());
    // timestamp RFC3339 折算毫秒(非零即可——绝对值归 ParseRfc3339Ms 册)。
    CHECK(event->received_at_ms > 0);
}

TEST_CASE("MapInteractionCreate:群聊回调取群成员 OpenID 为操作者原料") {
    // 官方示例 2 的形状:群聊按钮,group_openid + group_member_openid,
    // resolved 仅 button_data(群例为 base64 串,不解读只透传)。
    const nlohmann::json d = nlohmann::json::parse(R"({
        "application_id": "1904842048",
        "chat_type": 1,
        "data": {"resolved": {"button_data": "cWJ0b2tlbg=="}, "type": 11},
        "id": "group-interaction-1",
        "scene": "group",
        "timestamp": "2026-07-20T21:54:00+08:00",
        "type": 11,
        "group_openid": "GROUP-OPENID-1",
        "group_member_openid": "MEMBER-OPENID-1",
        "version": 1
    })");
    std::string error;
    const auto event = MapInteractionCreate(d, &error);
    REQUIRE(event.has_value());
    CHECK(event->group_openid == "GROUP-OPENID-1");
    CHECK(event->group_member_openid == "MEMBER-OPENID-1");
    CHECK(event->user_openid.empty());
}

TEST_CASE("MapInteractionCreate:数值字段字符串回传也认(真机教训)") {
    nlohmann::json d = OfficialC2cInteractionD();
    d["type"] = "11";
    d["version"] = "1";
    d["chat_type"] = "2";
    std::string error;
    const auto event = MapInteractionCreate(d, &error);
    REQUIRE(event.has_value());
    CHECK(event->type == 11);
    CHECK(event->version == 1);
    CHECK(event->chat_type == 2);
}

TEST_CASE("MapInteractionCreate:缺 id / 缺 data.resolved 拒绝") {
    nlohmann::json no_id = OfficialC2cInteractionD();
    no_id.erase("id");
    std::string error;
    CHECK_FALSE(MapInteractionCreate(no_id, &error).has_value());
    CHECK_FALSE(error.empty());

    nlohmann::json no_resolved = OfficialC2cInteractionD();
    no_resolved["data"].erase("resolved");
    error.clear();
    CHECK_FALSE(MapInteractionCreate(no_resolved, &error).has_value());

    CHECK_FALSE(MapInteractionCreate(nlohmann::json::array(), &error).has_value());
}

TEST_CASE("InteractionEventToJson:bridge 通知 params 带齐裁决原料") {
    std::string error;
    const auto event = MapInteractionCreate(OfficialC2cInteractionD(), &error);
    REQUIRE(event.has_value());
    const nlohmann::json params =
        InteractionEventToJson(*event, "qqbot", "main", "qq-del-1");
    CHECK(params["channelId"] == "qqbot");
    CHECK(params["accountId"] == "main");
    CHECK(params["deliveryId"] == "qq-del-1");
    CHECK(params["interactionId"] == event->interaction_id);
    CHECK(params["type"] == 11);
    CHECK(params["buttonData"] == "confirm:once");
    CHECK(params["userOpenid"] == event->user_openid);
    CHECK(params["chatType"] == 2);
    CHECK(params["scene"] == "c2c");
    CHECK(params["version"] == 1);
    CHECK(params["receivedAtMs"] == event->received_at_ms);
    // 群例:群成员身份也在(宿主裁操作者用)。
    nlohmann::json group_d = OfficialC2cInteractionD();
    group_d["group_member_openid"] = "MEMBER-1";
    const auto group_event = MapInteractionCreate(group_d, &error);
    REQUIRE(group_event.has_value());
    const nlohmann::json group_params =
        InteractionEventToJson(*group_event, "qqbot", "main", "qq-del-2");
    CHECK(group_params["groupMemberOpenid"] == "MEMBER-1");
    // 单聊事件不虚构群字段。
    CHECK_FALSE(params.contains("groupMemberOpenid"));
}

TEST_CASE("互动回应:PUT 路径与 code 载荷(官方回应接口页)") {
    CHECK(InteractionAckPath("abc-123") == "/interactions/abc-123");
    CHECK(BuildInteractionAckPayload(0) == nlohmann::json::parse(R"({"code": 0})"));
    CHECK(BuildInteractionAckPayload(4) == nlohmann::json::parse(R"({"code": 4})"));
}

TEST_CASE("BuildC2cSendPayload:键盘分支走 msg_type=2 且 keyboard 挂消息底") {
    C2cSendRequest request;
    request.openid = "openid-1";
    request.content = "**工具审批请求**";
    request.msg_id = "m-1";
    request.msg_seq = 1;
    request.keyboard = nlohmann::json::parse(R"({"content": {"rows": []}})");
    const nlohmann::json payload = BuildC2cSendPayload(request);
    CHECK(payload["msg_type"] == 2);
    CHECK(payload["content"] == "**工具审批请求**");
    CHECK(payload["keyboard"] == request.keyboard);
    CHECK(payload["msg_id"] == "m-1");
    CHECK(payload["msg_seq"] == 1);
    CHECK_FALSE(payload.contains("media"));
}

TEST_CASE("BuildC2cSendPayload:无键盘时 msg_type 0/7 行为零回归") {
    C2cSendRequest plain;
    plain.openid = "o";
    plain.content = "hi";
    CHECK(BuildC2cSendPayload(plain)["msg_type"] == 0);

    C2cSendRequest media;
    media.openid = "o";
    media.media_file_info = "file_info_x";
    const nlohmann::json payload = BuildC2cSendPayload(media);
    CHECK(payload["msg_type"] == 7);
    CHECK(payload["media"]["file_info"] == "file_info_x");
    CHECK_FALSE(payload.contains("content"));
    // 键盘与媒体同置:键盘优先(审批卡不走媒体路;装配层保证互斥,这里
    // 钉代码行为)。
    C2cSendRequest both = media;
    both.keyboard = nlohmann::json::parse(R"({"content": {"rows": []}})");
    CHECK(BuildC2cSendPayload(both)["msg_type"] == 2);
}
