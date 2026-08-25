// 跨会话传话的两件窄工具:list_sessions / send_session_message。工具是
// 薄壳,这里钉目标解析(名字/短 id)与发送结果的转述,不碰真管道。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "agent/peer_registry.hpp"
#include "tools/list_sessions_tool.hpp"
#include "tools/send_session_message_tool.hpp"

using namespace lubancode;
using namespace lubancode::agent;

namespace {

std::vector<PeerCard> SamplePeers() {
    PeerCard backend;
    backend.peer_id = "aaaabbbb";
    backend.name = "backend";
    backend.status = "idle";
    backend.cwd = "D:\\work\\backend";
    PeerCard frontend;
    frontend.peer_id = "ccccdddd";
    frontend.name = "frontend";
    frontend.status = "busy";
    frontend.cwd = "D:\\work\\frontend";
    return {backend, frontend};
}

}  // namespace

TEST_CASE("list_sessions:列出名字/短id/状态/cwd,自己不在列表里") {
    tools::ListSessionsTool tool([] { return SamplePeers(); }, "self0000");
    const auto result = tool.execute(nlohmann::json::object());
    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("backend") != std::string::npos);
    CHECK(result.content.find("aaaabbbb") != std::string::npos);
    CHECK(result.content.find("frontend") != std::string::npos);
    CHECK(result.content.find("D:\\work\\backend") != std::string::npos);
    CHECK(result.content.find("空闲") != std::string::npos);
    CHECK(result.content.find("忙") != std::string::npos);
}

TEST_CASE("list_sessions:没有别的会话时如实说明,不是错误") {
    tools::ListSessionsTool tool([] { return std::vector<PeerCard>{}; }, "self0000");
    const auto result = tool.execute(nlohmann::json::object());
    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("没有") != std::string::npos);
}

TEST_CASE("send_session_message:按名字或短 id 都能定人,送达结果如实转述") {
    PeerCard target;
    PeerDelivery captured = PeerDelivery::Delivered;
    std::string captured_text;
    tools::SendSessionMessageTool tool(
        [] { return SamplePeers(); },
        [&](const PeerCard& card, const std::string& text) {
            target = card;
            captured_text = text;
            return captured;
        });

    auto result = tool.execute(nlohmann::json{{"target", "frontend"}, {"text", "改好了"}});
    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("已送达") != std::string::npos);
    CHECK(target.peer_id == "ccccdddd");
    CHECK(captured_text == "改好了");

    result = tool.execute(nlohmann::json{{"target", "aaaabbbb"}, {"text", "按短 id"}});
    CHECK(target.peer_id == "aaaabbbb");  // 短 id 定人
    CHECK(result.content.find("已送达") != std::string::npos);

    captured = PeerDelivery::Held;
    result = tool.execute(nlohmann::json{{"target", "backend"}, {"text", "扣住看看"}});
    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("扣住") != std::string::npos);

    captured = PeerDelivery::Refused;
    result = tool.execute(nlohmann::json{{"target", "backend"}, {"text", "回绝看看"}});
    CHECK(result.is_error);

    captured = PeerDelivery::Expired;
    result = tool.execute(nlohmann::json{{"target", "backend"}, {"text", "限速看看"}});
    CHECK(result.is_error);
    CHECK(result.content.find("限速") != std::string::npos);

    captured = PeerDelivery::Unavailable;
    result = tool.execute(nlohmann::json{{"target", "backend"}, {"text", "不在看看"}});
    CHECK(result.is_error);
    CHECK(result.content.find("不在") != std::string::npos);
}

TEST_CASE("send_session_message:缺参数/空正文/找不到目标都是 is_error") {
    tools::SendSessionMessageTool tool([] { return SamplePeers(); },
                                       [](const PeerCard&, const std::string&) { return PeerDelivery::Delivered; });
    CHECK(tool.execute(nlohmann::json::object()).is_error);
    CHECK(tool.execute(nlohmann::json{{"target", "backend"}}).is_error);
    CHECK(tool.execute(nlohmann::json{{"target", "backend"}, {"text", ""}}).is_error);
    const auto result = tool.execute(nlohmann::json{{"target", "nobody"}, {"text", "hi"}});
    CHECK(result.is_error);
    CHECK(result.content.find("nobody") != std::string::npos);
    CHECK(result.content.find("list_sessions") != std::string::npos);
}

// list_sessions 不收参数,可 schema 不能是空对象 {}——严格端(OpenAI 档)
// 按 type 取值取了个空就整轮拒请求。wire 层另有 ToolSchemaForWire 兜底,
// 工具自己也得把壳给全。
TEST_CASE("list_sessions:无参也要给合法 schema 壳,不能是空对象") {
    tools::ListSessionsTool tool([] { return SamplePeers(); }, "self0000");
    const auto schema = tool.input_schema();
    REQUIRE(schema.is_object());
    CHECK(schema.contains("type"));
    CHECK(schema["type"] == "object");
    REQUIRE(schema.contains("properties"));
    CHECK(schema["properties"].is_object());
}
