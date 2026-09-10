// v3 信封与语义校验测试(schema 文档 §一/§二/§三):
// 枚举名稳定、未知键/未知 kind 拒收、kind↔status 固定映射、message 行
// 按 role/purpose 的约束(system turnId=null、assistant 来源三件套与
// usage 键、摘要 turnId=null+sourceMessageRef、tool 配对)、引用格式、
// 链节点结构、usage 非负。
#include <doctest/doctest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/schema3.hpp"

using namespace lubancode::trajectory::v3;

namespace {

// 组一行合法 event JSON 的底座(信封字段齐全)。
nlohmann::json EventJson(EventKindV3 kind, nlohmann::json payload = nlohmann::json::object()) {
    nlohmann::json json = nlohmann::json::object();
    json["type"] = "event";
    json["schemaVersion"] = 3;
    json["sessionId"] = "20260910-120000-AAAAAA";
    json["runId"] = "run-000001";
    json["seq"] = 2;
    json["timestamp"] = "2026-09-10T04:59:25.314Z";
    json["eventId"] = "evt-000001";
    json["kind"] = EventKindV3Name(kind);
    json["payload"] = std::move(payload);
    json["prevHash"] = std::string(kGenesisHash);
    json["lineHash"] = std::string(kGenesisHash);
    return json;
}

nlohmann::json MessageJson(nlohmann::json message, MessagePurpose purpose,
                           MessageOrigin origin) {
    nlohmann::json json = nlohmann::json::object();
    json["type"] = "message";
    json["schemaVersion"] = 3;
    json["sessionId"] = "20260910-120000-AAAAAA";
    json["runId"] = "run-000001";
    json["seq"] = 3;
    json["timestamp"] = "2026-09-10T04:59:25.314Z";
    json["messageId"] = "msg-000001";
    json["turnId"] = "turn-000001";
    json["purpose"] = MessagePurposeName(purpose);
    json["origin"] = MessageOriginName(origin);
    json["message"] = std::move(message);
    json["prevHash"] = std::string(kGenesisHash);
    json["lineHash"] = std::string(kGenesisHash);
    return json;
}

nlohmann::json AssistantJson() {
    nlohmann::json message = nlohmann::json::object(
        {{"role", "assistant"}, {"content", "好。"}});
    nlohmann::json json = MessageJson(std::move(message), MessagePurpose::Conversation,
                                      MessageOrigin::SessionRuntime);
    json["requestId"] = "request-000001";
    json["provider"] = "moonshot";
    json["wire"] = "openai-chat-completions";
    json["model"] = "kimi-k2.6";
    json["responseModel"] = "kimi-k2.6";
    json["usage"] = nlohmann::json::object({{"inputTokens", 120}, {"outputTokens", 8}});
    return json;
}

}  // namespace

TEST_CASE("枚举名稳定往返") {
    CHECK(MessageRoleFromName(MessageRoleName(MessageRole::System)) == MessageRole::System);
    CHECK(MessageRoleFromName(MessageRoleName(MessageRole::Tool)) == MessageRole::Tool);
    CHECK(!MessagePurposeFromName("compact_v2").has_value());
    CHECK(!MessageOriginFromName("robot").has_value());
    CHECK(!OpStatusFromName("success").has_value());
    CHECK(!CompletionStatusFromName("partial").has_value());
    // kind 全表:名字唯一、可往返。
    CHECK(AllEventKindsV3().size() >= 60);
    for (EventKindV3 kind : AllEventKindsV3()) {
        auto back = EventKindV3FromName(EventKindV3Name(kind));
        REQUIRE(back.has_value());
        CHECK(*back == kind);
    }
    CHECK(!EventKindV3FromName("session.began").has_value());  // 未知 kind 拒
}

TEST_CASE("未知键拒收,缺必选键拒收") {
    std::string ec, msg;
    SUBCASE("message 未知键") {
        nlohmann::json json = AssistantJson();
        json["oops"] = 1;
        auto parsed = MessageLine::FromJsonStrict(json, &ec, &msg);
        CHECK(!parsed.has_value());
        CHECK(ec == "schema3.unknown_key");
    }
    SUBCASE("event 未知键") {
        nlohmann::json json = EventJson(EventKindV3::SessionEnded);
        json["extra"] = true;
        auto parsed = EventLine::FromJsonStrict(json, &ec, &msg);
        CHECK(!parsed.has_value());
        CHECK(ec == "schema3.unknown_key");
    }
    SUBCASE("event 未知 kind") {
        nlohmann::json json = EventJson(EventKindV3::SessionEnded);
        json["kind"] = "compact.zen";
        auto parsed = EventLine::FromJsonStrict(json, &ec, &msg);
        CHECK(!parsed.has_value());
        CHECK(ec == "schema3.unknown_kind");
    }
    SUBCASE("message 缺 turnId 键") {
        nlohmann::json json = AssistantJson();
        json.erase("turnId");
        auto parsed = MessageLine::FromJsonStrict(json, &ec, &msg);
        CHECK(!parsed.has_value());
        CHECK(ec == "schema3.missing_field");
    }
}

TEST_CASE("kind 与 status 固定映射") {
    SUBCASE("terminal 事件必带匹配 status") {
        EventLine line;
        line.kind = EventKindV3::CompactApplied;
        std::optional<Schema3Error> error = ValidateEventLine(line);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.missing_field");
        line.status = OpStatus::Failed;  // 错配
        error = ValidateEventLine(line);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.status_kind_mismatch");
    }
    SUBCASE("非生命周期事件带 status 拒") {
        EventLine line;
        line.kind = EventKindV3::ModelResponseDelta;
        line.status = OpStatus::Running;
        auto error = ValidateEventLine(line);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.status_kind_mismatch");
    }
    SUBCASE("pending 必须带 payload.reason") {
        EventLine line;
        line.kind = EventKindV3::CompactPending;
        line.status = OpStatus::Pending;
        line.compact_id = "compact-000001";
        line.payload = nlohmann::json::object();  // 缺 reason
        auto error = ValidateEventLine(line);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.missing_field");
        line.payload = nlohmann::json::object({{"reason", "waiting_tool_group"}});
        CHECK(!ValidateEventLine(line).has_value());
    }
}

TEST_CASE("message 行按角色的合同") {
    std::string ec, msg;
    SUBCASE("system turnId 必为 null 且带 systemMeta") {
        nlohmann::json json = MessageJson(nlohmann::json::object({{"role", "system"},
                                                  {"content", "你是 LubanCode。"}}),
                                          MessagePurpose::Conversation,
                                          MessageOrigin::SessionRuntime);
        json["turnId"] = "turn-000001";  // 冒认回合
        auto parsed = MessageLine::FromJsonStrict(json, &ec, &msg);
        REQUIRE(parsed.has_value());
        auto error = ValidateMessageLine(*parsed);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.system_turn_not_null");
    }
    SUBCASE("assistant 缺来源/usage 键拒") {
        nlohmann::json json = AssistantJson();
        json.erase("usage");
        auto parsed = MessageLine::FromJsonStrict(json, &ec, &msg);
        REQUIRE(parsed.has_value());
        auto error = ValidateMessageLine(*parsed);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.missing_field");
        nlohmann::json json2 = AssistantJson();
        json2.erase("provider");
        auto parsed2 = MessageLine::FromJsonStrict(json2, &ec, &msg);
        REQUIRE(parsed2.has_value());
        CHECK(ValidateMessageLine(*parsed2)->code == "schema3.missing_source");
    }
    SUBCASE("assistant usage null 合法(缺实报不补零)") {
        nlohmann::json json = AssistantJson();
        json["usage"] = nullptr;
        auto parsed = MessageLine::FromJsonStrict(json, &ec, &msg);
        REQUIRE(parsed.has_value());
        CHECK(parsed->usage.has_value());
        CHECK((*parsed->usage).is_null());
        CHECK(!ValidateMessageLine(*parsed).has_value());
    }
    SUBCASE("context_summary 摘要 turnId=null 且指回候选") {
        nlohmann::json json = MessageJson(
            nlohmann::json::object({{"role", "user"}, {"content", "摘要正文"}}),
            MessagePurpose::ContextSummary, MessageOrigin::CompactRuntime);
        json["turnId"] = nullptr;
        json["compactId"] = "compact-000001";
        json["sourceMessageRef"] = "msg-000009";
        auto parsed = MessageLine::FromJsonStrict(json, &ec, &msg);
        REQUIRE(parsed.has_value());
        CHECK(!ValidateMessageLine(*parsed).has_value());
        // 冒充真人回合拒收。
        nlohmann::json json2 = json;
        json2["turnId"] = "turn-000002";
        auto parsed2 = MessageLine::FromJsonStrict(json2, &ec, &msg);
        REQUIRE(parsed2.has_value());
        auto error = ValidateMessageLine(*parsed2);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.summary_turn_not_null");
        // 缺 sourceMessageRef 拒收。
        nlohmann::json json3 = json;
        json3.erase("sourceMessageRef");
        auto parsed3 = MessageLine::FromJsonStrict(json3, &ec, &msg);
        REQUIRE(parsed3.has_value());
        CHECK(ValidateMessageLine(*parsed3)->code == "schema3.missing_field");
    }
    SUBCASE("tool 消息 tool_call_id 等于信封 actionId") {
        nlohmann::json json = MessageJson(
            nlohmann::json::object({{"role", "tool"},
                                    {"tool_call_id", "action-000001"},
                                    {"content", "exit_code: 0"}}),
            MessagePurpose::Conversation, MessageOrigin::SessionRuntime);
        json["actionId"] = "action-000001";
        auto parsed = MessageLine::FromJsonStrict(json, &ec, &msg);
        REQUIRE(parsed.has_value());
        CHECK(!ValidateMessageLine(*parsed).has_value());
        nlohmann::json json2 = json;
        json2["actionId"] = "action-000002";  // 配对断裂
        auto parsed2 = MessageLine::FromJsonStrict(json2, &ec, &msg);
        REQUIRE(parsed2.has_value());
        auto error = ValidateMessageLine(*parsed2);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.tool_call_id_mismatch");
    }
}

TEST_CASE("引用格式与链节点") {
    SUBCASE("引用:非空 string 或五键对象") {
        CHECK(IsValidRef(nlohmann::json("msg-000001")));
        CHECK(!IsValidRef(nlohmann::json("")));
        CHECK(IsValidRef(nlohmann::json::object(
            {{"sessionId", "s"}, {"runId", "r"}, {"seq", 9}, {"id", "m"},
             {"hash", std::string(64, '0')}})));
        CHECK(!IsValidRef(nlohmann::json::object({{"sessionId", "s"}})));  // 缺键
        CHECK(!IsValidRef(nlohmann::json(42)));
    }
    SUBCASE("链:根唯一、无环、邻接一致") {
        auto node = [](const char* ref, const char* prev) {
            return nlohmann::json::object(
                {{"messageRef", ref},
                 {"prevMessageRef", prev == nullptr ? nlohmann::json(nullptr)
                                                    : nlohmann::json(prev)}});
        };
        std::vector<nlohmann::json> good = {node("S", nullptr), node("Q", "S"), node("U", "Q")};
        CHECK(!ValidateContextChain("t", good).has_value());
        // 双根拒。
        std::vector<nlohmann::json> two_roots = {node("S", nullptr), node("Q", nullptr)};
        CHECK(ValidateContextChain("t", two_roots)->code == "schema3.bad_root");
        // 环。
        std::vector<nlohmann::json> loop = {node("S", nullptr), node("A", "B"), node("B", "A")};
        CHECK(ValidateContextChain("t", loop).has_value());
        // 邻接错位。
        std::vector<nlohmann::json> reordered = {node("S", nullptr), node("U", "Q"),
                                                 node("Q", "S")};
        CHECK(ValidateContextChain("t", reordered)->code == "schema3.chain_order_mismatch");
        // 悬空前驱。
        std::vector<nlohmann::json> dangling = {node("S", nullptr), node("Q", "X")};
        CHECK(ValidateContextChain("t", dangling)->code == "schema3.dangling_prev");
    }
}

TEST_CASE("usage 校验:非负整数,缺子项省键") {
    CHECK(!ValidateUsage(nlohmann::json(nullptr)).has_value());
    CHECK(!ValidateUsage(nlohmann::json::object({{"inputTokens", 10}})).has_value());
    CHECK(ValidateUsage(nlohmann::json::object({{"inputTokens", -3}})).has_value());
    CHECK(ValidateUsage(nlohmann::json::object({{"inputTokens", "10"}})).has_value());
}
