// app-server 协议骨架单:schema 层(信封折装、错误码、参数表、事件
// params 拼装)。规矩:所有断言 parse 之后逐字段查,不把整条黄金报文
// 写死成字符串比对——jsonrpc:"2.0" 字段去留是 schema 冻结时的开关
// (kEmitJsonRpcField),测试不许替它定死。
#include <doctest/doctest.h>

#include <string>

#include "app_server/protocol.hpp"
#include "app_server/schema.hpp"

using namespace lubancode::app_server;

namespace {

nlohmann::json Parse(const std::string& line) {
    return nlohmann::json::parse(line); // 测试里直接 parse,解不开就是测试失败
}

}  // namespace

// ---------------------------------------------------------------------------
// 出站信封
// ---------------------------------------------------------------------------

TEST_CASE("MakeEvent:方法与参数落位,jsonrpc 字段按开关") {
    const nlohmann::json event = MakeEvent(kEventTurnStarted, nlohmann::json{{"threadId", "t1"}});
    const std::string line = SerializeMessage(event);

    const nlohmann::json parsed = Parse(line);
    CHECK(parsed.is_object());
    CHECK(parsed["method"] == "turn/started");
    CHECK(parsed["params"]["threadId"] == "t1");
    // jsonrpc 去留未定:断言只查"字段在不在与开关一致",不查值。
    if (kEmitJsonRpcField) {
        CHECK(parsed.contains("jsonrpc"));
    } else {
        CHECK_FALSE(parsed.contains("jsonrpc"));
    }
}

TEST_CASE("MakeResult:id 与 result 落位;error 不混进来") {
    const nlohmann::json parsed = Parse(SerializeMessage(MakeResult(7, nlohmann::json{{"ok", true}})));
    CHECK(parsed["id"] == 7);
    CHECK(parsed["result"]["ok"] == true);
    CHECK_FALSE(parsed.contains("error"));
}

TEST_CASE("MakeError:code/message 落位,data 可省") {
    const nlohmann::json no_data = Parse(SerializeMessage(MakeError(3, kErrMethodNotFound, "未知方法: x")));
    CHECK(no_data["id"] == 3);
    CHECK(no_data["error"]["code"] == kErrMethodNotFound);
    CHECK(no_data["error"]["message"] == "未知方法: x");
    CHECK_FALSE(no_data["error"].contains("data"));

    const nlohmann::json with_data = Parse(SerializeMessage(
        MakeError(4, kErrInvalidParams, "缺字段", nlohmann::json{{"field", "threadId"}})));
    CHECK(with_data["error"]["data"]["field"] == "threadId");
}

TEST_CASE("MakeErrorForUnparseable:id 是 null(parse 失败唯一能回的形状)") {
    const nlohmann::json parsed =
        Parse(SerializeMessage(MakeErrorForUnparseable(kErrParseError, "报文不是合法 JSON")));
    CHECK(parsed["id"].is_null());
    CHECK(parsed["error"]["code"] == kErrParseError);
}

TEST_CASE("SerializeMessage:坏 UTF-8 也不抛,行永远可重新解析") {
    nlohmann::json event = MakeEvent("item/delta", nlohmann::json::object());
    event["params"]["delta"] = std::string("好\xff\xfe字"); // 非法 UTF-8 字节
    const std::string line = SerializeMessage(event);
    const nlohmann::json parsed = Parse(line); // 解不开就 fail
    CHECK(parsed["method"] == "item/delta");
}

// ---------------------------------------------------------------------------
// 入站信封
// ---------------------------------------------------------------------------

TEST_CASE("ParseIncoming:请求(带数字 id 与 method)") {
    EnvelopeError error;
    const auto message = ParseIncoming(R"({"id":1,"method":"initialize","params":{"clientName":"x"}})", error);
    REQUIRE(message.has_value());
    CHECK(message->kind == IncomingMessage::Kind::Request);
    CHECK(message->request.id == 1);
    CHECK(message->request.method == "initialize");
    CHECK(message->request.params["clientName"] == "x");
}

TEST_CASE("ParseIncoming:通知(只带 method)") {
    EnvelopeError error;
    const auto message = ParseIncoming(R"({"method":"initialized"})", error);
    REQUIRE(message.has_value());
    CHECK(message->kind == IncomingMessage::Kind::Notification);
    CHECK(message->notification.method == "initialized");
}

TEST_CASE("ParseIncoming:响应(反向请求的答复,留位形状)") {
    EnvelopeError error;
    const auto message = ParseIncoming(R"({"id":9,"result":{"decision":"accept"}})", error);
    REQUIRE(message.has_value());
    CHECK(message->kind == IncomingMessage::Kind::Response);
    CHECK(message->response.id == 9);
    CHECK_FALSE(message->response.is_error);
    CHECK(message->response.result["decision"] == "accept");

    const auto error_response = ParseIncoming(R"({"id":10,"error":{"code":-1,"message":"不干了"}})", error);
    REQUIRE(error_response.has_value());
    CHECK(error_response->kind == IncomingMessage::Kind::Response);
    CHECK(error_response->response.is_error);
}

TEST_CASE("ParseIncoming:jsonrpc 字段带不带都认(去留未定,入站不校验)") {
    EnvelopeError error;
    const auto with_field = ParseIncoming(R"({"jsonrpc":"2.0","id":1,"method":"shutdown"})", error);
    REQUIRE(with_field.has_value());
    CHECK(with_field->kind == IncomingMessage::Kind::Request);

    const auto without_field = ParseIncoming(R"({"id":1,"method":"shutdown"})", error);
    REQUIRE(without_field.has_value());
    CHECK(without_field->kind == IncomingMessage::Kind::Request);
}

TEST_CASE("ParseIncoming:缺 params 视为空对象") {
    EnvelopeError error;
    const auto message = ParseIncoming(R"({"id":2,"method":"thread/list"})", error);
    REQUIRE(message.has_value());
    CHECK(message->request.params.is_object());
    CHECK(message->request.params.empty());
}

TEST_CASE("ParseIncoming:坏 JSON 给稳定错误码 kErrParseError") {
    EnvelopeError error;
    const auto message = ParseIncoming("这不是 JSON", error);
    CHECK_FALSE(message.has_value());
    CHECK(error.code == kErrParseError);
    CHECK_FALSE(error.has_id);
}

TEST_CASE("ParseIncoming:非对象 JSON 也算 parse 错") {
    EnvelopeError error;
    CHECK_FALSE(ParseIncoming("[1,2,3]", error).has_value());
    CHECK(error.code == kErrParseError);
    CHECK_FALSE(ParseIncoming("\"就是个字符串\"", error).has_value());
    CHECK(error.code == kErrParseError);
}

TEST_CASE("ParseIncoming:id 类型不对给 kErrInvalidRequest") {
    EnvelopeError error;
    CHECK_FALSE(ParseIncoming(R"({"id":"str","method":"x"})", error).has_value());
    CHECK(error.code == kErrInvalidRequest);

    CHECK_FALSE(ParseIncoming(R"({"id":1.5,"method":"x"})", error).has_value());
    CHECK(error.code == kErrInvalidRequest);

    CHECK_FALSE(ParseIncoming(R"({"id":null,"method":"x"})", error).has_value());
    CHECK(error.code == kErrInvalidRequest);
}

TEST_CASE("ParseIncoming:method 不是字符串、或啥都没有") {
    EnvelopeError error;
    CHECK_FALSE(ParseIncoming(R"({"id":1,"method":42})", error).has_value());
    CHECK(error.code == kErrInvalidRequest);

    CHECK_FALSE(ParseIncoming(R"({"id":1})", error).has_value()); // 有 id 没 method 没 result
    CHECK(error.code == kErrInvalidRequest);

    CHECK_FALSE(ParseIncoming(R"({})", error).has_value()); // 空对象
    CHECK(error.code == kErrInvalidRequest);
}

TEST_CASE("ParseIncoming:坏请求里捞得出的数字 id 要带回去(响应对得上号)") {
    EnvelopeError error;
    CHECK_FALSE(ParseIncoming(R"({"id":42,"method":true})", error).has_value());
    CHECK(error.has_id);
    CHECK(error.id == 42);
}

// ---------------------------------------------------------------------------
// 参数表
// ---------------------------------------------------------------------------

TEST_CASE("CheckTurnStartParams:threadId/text 都要,缺一个错一个") {
    std::string thread_id;
    std::string text;
    CHECK(CheckTurnStartParams(nlohmann::json{{"threadId", "t"}, {"text", "问点啥"}}, thread_id, text).ok);
    CHECK(thread_id == "t");
    CHECK(text == "问点啥");

    const ParamsCheck missing_text = CheckTurnStartParams(nlohmann::json{{"threadId", "t"}}, thread_id, text);
    CHECK_FALSE(missing_text.ok);
    CHECK(missing_text.code == kErrInvalidParams);

    const ParamsCheck wrong_type =
        CheckTurnStartParams(nlohmann::json{{"threadId", 3}, {"text", "x"}}, thread_id, text);
    CHECK_FALSE(wrong_type.ok);
    CHECK(wrong_type.code == kErrInvalidParams);

    const ParamsCheck empty_id = CheckTurnStartParams(nlohmann::json{{"threadId", ""}, {"text", "x"}}, thread_id, text);
    CHECK_FALSE(empty_id.ok);
}

TEST_CASE("CheckThreadStopParams:threadId 必填字符串") {
    std::string thread_id;
    CHECK(CheckThreadStopParams(nlohmann::json{{"threadId", "t"}}, thread_id).ok);
    const ParamsCheck missing = CheckThreadStopParams(nlohmann::json::object(), thread_id);
    CHECK_FALSE(missing.ok);
    CHECK(missing.code == kErrInvalidParams);
}

TEST_CASE("CheckInitializeParams / CheckThreadStartParams:可选字段类型对了才过") {
    CHECK(CheckInitializeParams(nlohmann::json::object()).ok);
    CHECK(CheckInitializeParams(nlohmann::json{{"clientName", "gui"}, {"clientVersion", "0.1"}}).ok);
    CHECK_FALSE(CheckInitializeParams(nlohmann::json{{"clientName", 1}}).ok);

    CHECK(CheckThreadStartParams(nlohmann::json::object()).ok);
    CHECK(CheckThreadStartParams(nlohmann::json{{"cwd", "/tmp"}}).ok);
    CHECK_FALSE(CheckThreadStartParams(nlohmann::json{{"cwd", 5}}).ok);
}

// ---------------------------------------------------------------------------
// 事件 params 拼装
// ---------------------------------------------------------------------------

TEST_CASE("MakeTurnCompletedParams:成功不带 error 字段,失败带") {
    const nlohmann::json ok =
        MakeTurnCompletedParams("t", "turn-1", kTurnStatusSuccess, "", nlohmann::json::object(), 2);
    CHECK(ok["status"] == "success");
    CHECK_FALSE(ok.contains("error"));
    CHECK(ok["stepsUsed"] == 2);
    CHECK(ok["threadId"] == "t");
    CHECK(ok["turnId"] == "turn-1");

    const nlohmann::json failed =
        MakeTurnCompletedParams("t", "turn-1", kTurnStatusError, "模型请求失败", nlohmann::json::object(), 1);
    CHECK(failed["status"] == "error");
    CHECK(failed["error"] == "模型请求失败");
}

TEST_CASE("MakeItemStartedParams:item 的 id/type 与 payload 并进同一对象") {
    const nlohmann::json params =
        MakeItemStartedParams("t", "turn-1", "item-0", kItemTypeTool, nlohmann::json{{"name", "read_file"}});
    CHECK(params["item"]["id"] == "item-0");
    CHECK(params["item"]["type"] == "tool");
    CHECK(params["item"]["name"] == "read_file");
    CHECK(params["threadId"] == "t");
    CHECK(params["turnId"] == "turn-1");
}

TEST_CASE("MakeItemDeltaParams:增量字段落位") {
    const nlohmann::json params = MakeItemDeltaParams("t", "turn-1", "item-0", "半句");
    CHECK(params["itemId"] == "item-0");
    CHECK(params["delta"] == "半句");
}

TEST_CASE("MakeQueueOverflowParams:dropped/coalesced 落位") {
    const nlohmann::json params = MakeQueueOverflowParams("t", "turn-1", 12, 0);
    CHECK(params["dropped"] == 12);
    CHECK(params["coalesced"] == 0);
}

// ---------------------------------------------------------------------------
// initialize / thread/list 结果
// ---------------------------------------------------------------------------

TEST_CASE("MakeInitializeResult:版本、平台、能力表(接线/留位分开报)") {
    const nlohmann::json result = MakeInitializeResult("0.99.0", "linux");
    CHECK(result["protocolVersion"].get<std::string>() == kProtocolVersion);
    CHECK(result["lubancodeVersion"] == "0.99.0");
    CHECK(result["platform"] == "linux");

    const nlohmann::json& capabilities = result["capabilities"];
    // 已接线的法子。
    bool has_initialize = false;
    bool has_turn_start = false;
    for (const auto& method : capabilities["methods"]) {
        if (method == "initialize") has_initialize = true;
        if (method == "turn/start") has_turn_start = true;
    }
    CHECK(has_initialize);
    CHECK(has_turn_start);

    // 留位的法子在 pending,不在 methods(如实报,不冒充接线)。
    bool pending_has_steer = false;
    for (const auto& method : capabilities["pending"]) {
        if (method == "turn/steer") pending_has_steer = true;
    }
    CHECK(pending_has_steer);

    // 审批/ask_user 的反向请求位。
    bool has_permission = false;
    for (const auto& method : capabilities["serverRequests"]) {
        if (method == "permission/request") has_permission = true;
    }
    CHECK(has_permission);

    // 事件账的类型与终态(审批/打断/diff 留的位)。
    bool has_question_type = false;
    for (const auto& type : capabilities["itemTypes"]) {
        if (type == "question") has_question_type = true;
    }
    CHECK(has_question_type);
    bool has_interrupted = false;
    for (const auto& status : capabilities["turnStatuses"]) {
        if (status == "interrupted") has_interrupted = true;
    }
    CHECK(has_interrupted);
}

TEST_CASE("MakeThreadListResult:空表也是合法数组") {
    CHECK(MakeThreadListResult({})["threads"].is_array());
    CHECK(MakeThreadListResult({})["threads"].empty());
}
