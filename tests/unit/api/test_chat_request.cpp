#include <doctest/doctest.h>

#include "api/chat/request.hpp"

using namespace lubancode;

TEST_CASE("Chat request: system、图片、工具调用和工具结果翻成兼容消息") {
    api::Request request;
    request.model = "glm-5.2";
    request.system = "守规矩";
    request.reasoning_effort = "high";
    request.tools.push_back({"read_file", "读文件", nlohmann::json{{"type", "object"}}});

    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"看图"});
    user.content.push_back(api::ImageBlock{"image/png", "AAAA", "x.png", 1, 1});
    request.messages.push_back(user);

    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::TextBlock{"我来读"});
    assistant.content.push_back(api::ToolUseBlock{"call_1", "read_file", nlohmann::json{{"path", "a.cpp"}}});
    request.messages.push_back(assistant);

    api::Message result;
    result.role = api::Role::User;
    result.content.push_back(api::ToolResultBlock{"call_1", "正文", false});
    request.messages.push_back(result);

    const auto body = api::chat::BuildRequestJson(request, nlohmann::json{{"tool_stream", true}});
    CHECK(body["model"] == "glm-5.2");
    CHECK(body["reasoning_effort"] == "high");
    CHECK(body["tool_stream"] == true);
    REQUIRE(body["messages"].size() == 4);
    CHECK(body["messages"][0]["role"] == "system");
    CHECK(body["messages"][1]["content"][1]["type"] == "image_url");
    CHECK(body["messages"][2]["tool_calls"][0]["function"]["name"] == "read_file");
    CHECK(body["messages"][3]["role"] == "tool");
    CHECK(body["tools"][0]["function"]["parameters"]["type"] == "object");
}

TEST_CASE("Chat request: 富工具结果降级——tool 消息吃投影文本,不带 base64(MCP 富结果单 P0.6)") {
    api::Request request;
    request.model = "glm-5.2";
    api::Message result;
    result.role = api::Role::User;
    api::ToolResultBlock rich;
    rich.tool_use_id = "call_shot";
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
    result.content.push_back(rich);
    request.messages.push_back(result);

    const auto body = api::chat::BuildRequestJson(request, nlohmann::json::object());
    REQUIRE(body["messages"].size() == 1);
    const auto& tool_message = body["messages"][0];
    CHECK(tool_message["role"] == "tool");
    // Chat wire 不先假定图片支持:文本投影(artifact 短句)开头,后跟
    // 明降级附注(工具结果图片回喂单:chat 的 tool 消息无图,一行指路
    // 落盘路径)。字节本体(base64)不出门。
    const std::string content = tool_message["content"].get<std::string>();
    CHECK(content.rfind(rich.content, 0) == 0);  // 投影在前
    CHECK(content.find("[wire 降级] 该 wire 不支持工具结果图片,1 张未随行,字节已存盘: art-00112233.png") !=
          std::string::npos);
    CHECK(tool_message.dump().find("base64") == std::string::npos);
}

TEST_CASE("Chat request: extra_body 最后覆盖内置字段") {
    api::Request request;
    request.model = "m";
    request.extra_body = nlohmann::json{{"max_tokens", 77}};
    const auto body = api::chat::BuildRequestJson(
        request, nlohmann::json{{"max_tokens", 99}, {"stream_options", nullptr}});
    CHECK(body["max_tokens"] == 77);  // 模型 variant 压过 provider 默认
    CHECK(body["stream_options"].is_null());
}

// ---------------------------------------------------------------------------
// stream_options.include_usage:按 provider capability 发,默认不发;
// 用户 extra_body 显式写了 stream_options 整个压过内置值。
// ---------------------------------------------------------------------------

TEST_CASE("Chat request: 默认不带 stream_options(兼容端未必认这个字段)") {
    api::Request request;
    request.model = "m";
    const auto body = api::chat::BuildRequestJson(request);
    CHECK(!body.contains("stream_options"));
}

TEST_CASE("Chat request: capability 开了就带 stream_options.include_usage") {
    api::Request request;
    request.model = "deepseek-v4-pro";
    api::chat::ChatRequestOptions options;
    options.stream_usage = true;
    const auto body = api::chat::BuildRequestJson(request, nlohmann::json::object(), options);
    CHECK(body["stream"] == true);
    CHECK(body["stream_options"]["include_usage"] == true);
}

TEST_CASE("Chat request: 用户 extra_body 里的 stream_options 压过 capability 默认") {
    api::Request request;
    request.model = "m";
    api::chat::ChatRequestOptions options;
    options.stream_usage = true;
    const auto body = api::chat::BuildRequestJson(
        request, nlohmann::json{{"stream_options", nlohmann::json{{"include_usage", false}}}}, options);
    CHECK(body["stream_options"]["include_usage"] == false);
}

// ---------------------------------------------------------------------------
// reasoning 回传(reasoning_replay=tool_episode,DeepSeek 协议):纯对话段
// 略过;工具交互段按原字节原次序回传 reasoning_content,不进 content。
// 默认策略 never 维持现行行为(一条不带)。
// ---------------------------------------------------------------------------

api::chat::ChatRequestOptions ToolEpisode() {
    api::chat::ChatRequestOptions options;
    options.reasoning_replay = api::chat::ReasoningReplayPolicy::ToolEpisode;
    return options;
}

TEST_CASE("Chat request: 纯对话思考不回传(哪怕开了 tool_episode)") {
    api::Request request;
    request.model = "deepseek-v4-pro";
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"你好"});
    request.messages.push_back(user);
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ThinkingBlock{"想了一下", ""});
    assistant.content.push_back(api::TextBlock{"你好!"});
    request.messages.push_back(assistant);
    api::Message next;
    next.role = api::Role::User;
    next.content.push_back(api::TextBlock{"再问"});
    request.messages.push_back(next);

    const auto body = api::chat::BuildRequestJson(request, nlohmann::json::object(), ToolEpisode());
    REQUIRE(body["messages"].size() == 3);  // system 为空不发,三条原样
    CHECK(body["messages"][1]["role"] == "assistant");
    CHECK(body["messages"][1]["content"] == "你好!");
    CHECK_FALSE(body["messages"][1].contains("reasoning_content"));
}

TEST_CASE("Chat request: 思考+工具调用,原样回传 reasoning_content 与 tool_calls") {
    api::Request request;
    request.model = "deepseek-v4-pro";
    request.tools.push_back({"read_file", "读文件", nlohmann::json{{"type", "object"}}});
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"读一下 a.cpp"});
    request.messages.push_back(user);
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ThinkingBlock{"先想路径", ""});
    assistant.content.push_back(api::ToolUseBlock{"call_1", "read_file", nlohmann::json{{"path", "a.cpp"}}});
    request.messages.push_back(assistant);
    api::Message result;
    result.role = api::Role::User;
    result.content.push_back(api::ToolResultBlock{"call_1", "正文", false});
    request.messages.push_back(result);

    const auto body = api::chat::BuildRequestJson(request, nlohmann::json::object(), ToolEpisode());
    const auto& replayed = body["messages"][1];
    CHECK(replayed["reasoning_content"] == "先想路径");  // 原字节,不加标签
    CHECK(replayed["tool_calls"][0]["id"] == "call_1");
    CHECK(replayed["content"].is_null());  // 没正文就是 null,思考不混进 content
}

TEST_CASE("Chat request: 先工具后总结的交互段,下一轮 user 请求仍保留段内思考") {
    api::Request request;
    request.model = "deepseek-v4-pro";
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"查一下"});
    request.messages.push_back(user);
    api::Message tool_assistant;
    tool_assistant.role = api::Role::Assistant;
    tool_assistant.content.push_back(api::ThinkingBlock{"查哪呢", ""});
    tool_assistant.content.push_back(api::ToolUseBlock{"call_1", "search", nlohmann::json{{"pattern", "x"}}});
    request.messages.push_back(tool_assistant);
    api::Message result;
    result.role = api::Role::User;
    result.content.push_back(api::ToolResultBlock{"call_1", "查到了", false});
    request.messages.push_back(result);
    api::Message final_assistant;
    final_assistant.role = api::Role::Assistant;
    final_assistant.content.push_back(api::ThinkingBlock{"组织一下", ""});
    final_assistant.content.push_back(api::TextBlock{"结论是 X"});
    request.messages.push_back(final_assistant);
    api::Message next_user;
    next_user.role = api::Role::User;
    next_user.content.push_back(api::TextBlock{"那 Y 呢"});
    request.messages.push_back(next_user);

    const auto body = api::chat::BuildRequestJson(request, nlohmann::json::object(), ToolEpisode());
    // 段内两条 assistant:带工具那条与只出结论那条,思考都在原位、原字节。
    CHECK(body["messages"][1]["reasoning_content"] == "查哪呢");
    CHECK(body["messages"][3]["reasoning_content"] == "组织一下");
    CHECK(body["messages"][3]["content"] == "结论是 X");
}

TEST_CASE("Chat request: 默认策略 never——工具段也不回传(现行行为不变)") {
    api::Request request;
    request.model = "glm-5.2";
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"读 a"});
    request.messages.push_back(user);
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ThinkingBlock{"想", ""});
    assistant.content.push_back(api::ToolUseBlock{"call_1", "read_file", nlohmann::json{{"path", "a"}}});
    request.messages.push_back(assistant);

    const auto body = api::chat::BuildRequestJson(request);
    CHECK_FALSE(body["messages"][1].contains("reasoning_content"));
}

TEST_CASE("Chat request: 一条 assistant 多枚 tool call,reasoning 只写一次") {
    api::Request request;
    request.model = "deepseek-v4-pro";
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"两个都读"});
    request.messages.push_back(user);
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ThinkingBlock{"一次读俩", ""});
    assistant.content.push_back(api::ToolUseBlock{"call_1", "read_file", nlohmann::json{{"path", "a"}}});
    assistant.content.push_back(api::ToolUseBlock{"call_2", "read_file", nlohmann::json{{"path", "b"}}});
    request.messages.push_back(assistant);
    api::Message result;
    result.role = api::Role::User;
    result.content.push_back(api::ToolResultBlock{"call_1", "A", false});
    result.content.push_back(api::ToolResultBlock{"call_2", "B", false});
    request.messages.push_back(result);

    const auto body = api::chat::BuildRequestJson(request, nlohmann::json::object(), ToolEpisode());
    const auto& replayed = body["messages"][1];
    CHECK(replayed["reasoning_content"] == "一次读俩");
    REQUIRE(replayed["tool_calls"].size() == 2);
    // 两条 tool result 消息(tool role)不带 reasoning,只报结果。
    CHECK(body["messages"][2]["role"] == "tool");
    CHECK(body["messages"][3]["role"] == "tool");
    CHECK_FALSE(body["messages"][2].contains("reasoning_content"));
}

// ---------------------------------------------------------------------------
// reasoning 回传 always(Kimi 保留式思考单 P0):工作视图里每条带
// ThinkingBlock 的原始 assistant 都回传——纯对话、工具、总结一视同仁。
// 策略裁决方言优先:request.reasoning.dialect 有正式声明就用它,legacy
// ChatRequestOptions 只当手写旧 provider 的回落。
// ---------------------------------------------------------------------------

// Kimi K3 形状的方言:toggle 无、档位顶层 reasoning_effort、replay=always。
api::ReasoningConfig AlwaysDialect(const std::string& replay = "always",
                                   const std::string& replay_field = "") {
    api::ReasoningConfig reasoning;
    reasoning.supports_effort = true;
    reasoning.dialect.toggle = "none";
    reasoning.dialect.effort_path = "reasoning_effort";
    reasoning.dialect.delta = "reasoning_content";
    reasoning.dialect.replay = replay;
    reasoning.dialect.replay_field = replay_field;
    return reasoning;
}

TEST_CASE("Chat request: always——纯对话段的思考也原样回传(方言驱动)") {
    api::Request request;
    request.model = "kimi-k3";
    request.reasoning = AlwaysDialect();
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"你好"});
    request.messages.push_back(user);
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ThinkingBlock{"想了一下怎么答", ""});
    assistant.content.push_back(api::TextBlock{"你好!"});
    request.messages.push_back(assistant);
    api::Message next;
    next.role = api::Role::User;
    next.content.push_back(api::TextBlock{"再问"});
    request.messages.push_back(next);

    // 注意 options 仍是默认 Never:方言压过 legacy 回落。
    const auto body = api::chat::BuildRequestJson(request);
    CHECK(body["messages"][1]["reasoning_content"] == "想了一下怎么答");
    CHECK(body["messages"][1]["content"] == "你好!");
}

TEST_CASE("Chat request: always——工具段与最终总结的思考都在原位") {
    api::Request request;
    request.model = "kimi-k2.7-code";
    request.reasoning = AlwaysDialect();
    request.tools.push_back({"read_file", "读文件", nlohmann::json{{"type", "object"}}});
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"查一下"});
    request.messages.push_back(user);
    api::Message tool_assistant;
    tool_assistant.role = api::Role::Assistant;
    tool_assistant.content.push_back(api::ThinkingBlock{"查哪呢", ""});
    tool_assistant.content.push_back(api::ToolUseBlock{"call_1", "search", nlohmann::json{{"pattern", "x"}}});
    request.messages.push_back(tool_assistant);
    api::Message result;
    result.role = api::Role::User;
    result.content.push_back(api::ToolResultBlock{"call_1", "查到了", false});
    request.messages.push_back(result);
    api::Message final_assistant;
    final_assistant.role = api::Role::Assistant;
    final_assistant.content.push_back(api::ThinkingBlock{"组织一下"});
    final_assistant.content.push_back(api::TextBlock{"结论是 X"});
    request.messages.push_back(final_assistant);
    api::Message next_user;
    next_user.role = api::Role::User;
    next_user.content.push_back(api::TextBlock{"那 Y 呢"});
    request.messages.push_back(next_user);

    const auto body = api::chat::BuildRequestJson(request);
    // 带 tool call 的 assistant 与只出总结的 assistant,思考都原字节回传。
    CHECK(body["messages"][1]["reasoning_content"] == "查哪呢");
    CHECK(body["messages"][3]["reasoning_content"] == "组织一下");
    CHECK(body["messages"][3]["content"] == "结论是 X");
}

TEST_CASE("Chat request: always——多枚思考块按块序拼接,只落一枚字段;没思考不造空串") {
    api::Request request;
    request.model = "kimi-k3";
    request.reasoning = AlwaysDialect();
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"先算"});
    request.messages.push_back(user);
    api::Message joined;
    joined.role = api::Role::Assistant;
    joined.content.push_back(api::ThinkingBlock{"第一段", "sig-a"});
    joined.content.push_back(api::TextBlock{"插一句"});
    joined.content.push_back(api::ThinkingBlock{"第二段", "sig-b"});
    joined.content.push_back(api::TextBlock{"答完了"});
    request.messages.push_back(joined);
    api::Message plain_user;
    plain_user.role = api::Role::User;
    plain_user.content.push_back(api::TextBlock{"再问"});
    request.messages.push_back(plain_user);
    api::Message no_thinking;
    no_thinking.role = api::Role::Assistant;
    no_thinking.content.push_back(api::TextBlock{"这轮没思考"});
    request.messages.push_back(no_thinking);

    const auto body = api::chat::BuildRequestJson(request);
    const auto& replayed = body["messages"][1];
    CHECK(replayed["reasoning_content"] == "第一段第二段");  // 块序拼接,不加标签
    // 没有 ThinkingBlock 的 assistant 不写空 reasoning_content,正文照旧。
    CHECK_FALSE(body["messages"][3].contains("reasoning_content"));
    CHECK(body["messages"][3]["content"] == "这轮没思考");
}

TEST_CASE("Chat request: always——回传字段名听方言声明,不双写") {
    api::Request request;
    request.model = "some-vllm-kimi";
    request.reasoning = AlwaysDialect("always", "reasoning");
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"你好"});
    request.messages.push_back(user);
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ThinkingBlock{"想了一下", ""});
    assistant.content.push_back(api::TextBlock{"你好!"});
    request.messages.push_back(assistant);

    const auto body = api::chat::BuildRequestJson(request);
    CHECK(body["messages"][1]["reasoning"] == "想了一下");
    CHECK_FALSE(body["messages"][1].contains("reasoning_content"));
}

TEST_CASE("Chat request: 方言声明压过 legacy 回落——两头各试一回") {
    api::Request request;
    request.model = "kimi-k2.5";
    request.reasoning = AlwaysDialect("never");
    request.tools.push_back({"read_file", "读文件", nlohmann::json{{"type", "object"}}});
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"读 a"});
    request.messages.push_back(user);
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ThinkingBlock{"想", ""});
    assistant.content.push_back(api::ToolUseBlock{"call_1", "read_file", nlohmann::json{{"path", "a"}}});
    request.messages.push_back(assistant);

    // 方言说 never,legacy 配了 tool_episode:正式方言赢,一条不回。
    const auto body = api::chat::BuildRequestJson(request, nlohmann::json::object(), ToolEpisode());
    CHECK_FALSE(body["messages"][1].contains("reasoning_content"));
}

TEST_CASE("Chat request: legacy ChatRequestOptions 也认 always(自定义 provider 回落档)") {
    api::Request request;
    request.model = "my-private-kimi";
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"你好"});
    request.messages.push_back(user);
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ThinkingBlock{"想了一下", ""});
    assistant.content.push_back(api::TextBlock{"你好!"});
    request.messages.push_back(assistant);

    api::chat::ChatRequestOptions options;
    options.reasoning_replay = api::chat::ReasoningReplayPolicy::Always;
    const auto body = api::chat::BuildRequestJson(request, nlohmann::json::object(), options);
    CHECK(body["messages"][1]["reasoning_content"] == "想了一下");
}

TEST_CASE("Chat request: 档位参数名按 provider 声明走,空档位字段整个缺席") {
    api::Request request;
    request.model = "qwen";
    request.reasoning_effort = "xhigh";
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"ping"});
    request.messages.push_back(user);

    SUBCASE("默认参数名 reasoning_effort") {
        const auto body = api::chat::BuildRequestJson(request);
        CHECK(body["reasoning_effort"] == "xhigh");
        CHECK_FALSE(body.contains("reasoning"));
    }
    SUBCASE("provider 声明 think_param 后换名字") {
        api::chat::ChatRequestOptions options;
        options.reasoning_param = "thinking_budget_level";
        const auto body = api::chat::BuildRequestJson(request, nlohmann::json::object(), options);
        CHECK(body["thinking_budget_level"] == "xhigh");
        CHECK_FALSE(body.contains("reasoning_effort"));
    }
    SUBCASE("档位为空(不填):不偷偷塞默认档,字段缺席") {
        request.reasoning_effort.clear();
        const auto body = api::chat::BuildRequestJson(request);
        CHECK_FALSE(body.contains("reasoning_effort"));
        api::chat::ChatRequestOptions options;
        options.reasoning_param = "custom";
        const auto named = api::chat::BuildRequestJson(request, nlohmann::json::object(), options);
        CHECK_FALSE(named.contains("custom"));
    }
}

TEST_CASE("Chat request: 模型声明 effort+toggle 时写正式字段,不用 extra_body") {
    api::Request request;
    request.model = "glm-5.2";
    request.reasoning_effort = "max";
    request.reasoning.supports_effort = true;
    request.reasoning.supports_toggle = true;
    const auto body = api::chat::BuildRequestJson(request);
    CHECK(body["reasoning_effort"] == "max");
    CHECK(body["thinking"]["type"] == "enabled");
    CHECK(request.extra_body.empty());

    request.reasoning_effort = "none";
    const auto off = api::chat::BuildRequestJson(request);
    CHECK(off["reasoning_effort"] == "none");
    CHECK(off["thinking"]["type"] == "disabled");
}

TEST_CASE("Chat request: toggle-only 模型只写 thinking,不乱发 reasoning_effort") {
    api::Request request;
    request.reasoning_effort = "auto";
    request.reasoning.supports_toggle = true;
    const auto body = api::chat::BuildRequestJson(request);
    CHECK(body["thinking"]["type"] == "enabled");
    CHECK_FALSE(body.contains("reasoning_effort"));
}

// ---------------------------------------------------------------------------
// reasoning 回传字段名(reasoning_replay_field,vLLM/Qwen 单):回传历史
// 时不许想当然把所有服务都写成 reasoning_content——DeepSeek 协议要
// reasoning_content,vLLM 0.27+/Qwen 只认 reasoning。
// ---------------------------------------------------------------------------

api::chat::ChatRequestOptions ToolEpisodeWithReplayField(const std::string& field) {
    api::chat::ChatRequestOptions options;
    options.reasoning_replay = api::chat::ReasoningReplayPolicy::ToolEpisode;
    options.reasoning_replay_field = field;
    return options;
}

TEST_CASE("Chat request: 回传字段名按 provider 声明走,默认仍是 reasoning_content") {
    api::Request request;
    request.model = "qwen3.8-27b";
    request.tools.push_back({"read_file", "读文件", nlohmann::json{{"type", "object"}}});
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"读 a.cpp"});
    request.messages.push_back(user);
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ThinkingBlock{"先想路径", ""});
    assistant.content.push_back(api::ToolUseBlock{"call_1", "read_file", nlohmann::json{{"path", "a.cpp"}}});
    request.messages.push_back(assistant);
    api::Message result;
    result.role = api::Role::User;
    result.content.push_back(api::ToolResultBlock{"call_1", "正文", false});
    request.messages.push_back(result);

    SUBCASE("默认:回传写 reasoning_content(DeepSeek 协议,现行行为)") {
        const auto body = api::chat::BuildRequestJson(request, nlohmann::json::object(), ToolEpisode());
        CHECK(body["messages"][1]["reasoning_content"] == "先想路径");
        CHECK_FALSE(body["messages"][1].contains("reasoning"));
    }
    SUBCASE("声明 reasoning(vLLM/Qwen):同一个名字,不是 reasoning_content") {
        const auto body = api::chat::BuildRequestJson(request, nlohmann::json::object(),
                                                      ToolEpisodeWithReplayField("reasoning"));
        CHECK(body["messages"][1]["reasoning"] == "先想路径");
        CHECK_FALSE(body["messages"][1].contains("reasoning_content"));
    }
}

// ---------------------------------------------------------------------------
// 工具 schema 归一化(ToolSchemaForWire):不收参数的工具从前回空对象 {},
// 严格端按 type 取值取了个空,当场回 "got 'type: null'",整轮请求连带被拒。
// 四家 wire 出门前都得兑正,这里钉 chat 这一家。
// ---------------------------------------------------------------------------

TEST_CASE("Chat request: 空 schema 兑成最小合法壳,不让严格端吃 type: null") {
    api::Request request;
    request.model = "deepseek-v4-flash";
    request.tools.push_back({"list_sessions", "列会话", nlohmann::json::object()});

    const auto body = api::chat::BuildRequestJson(request);
    const auto& params = body["tools"][0]["function"]["parameters"];
    CHECK(params["type"] == "object");
    CHECK(params["properties"].is_object());
    CHECK(params["properties"].empty());
}

TEST_CASE("Chat request: 缺 type / type 不是 object / 非对象 schema 一概兑正") {
    api::Request request;
    request.model = "m";
    request.tools.push_back({"a", "缺 type", nlohmann::json{{"properties", nlohmann::json::object()}}});
    request.tools.push_back({"b", "type 不是 object", nlohmann::json{{"type", "string"}}});
    request.tools.push_back({"c", "压根儿不是对象", nlohmann::json::array()});
    request.tools.push_back({"d", "null", nlohmann::json()});

    const auto body = api::chat::BuildRequestJson(request);
    for (int i = 0; i < 4; ++i) {
        const auto& params = body["tools"][i]["function"]["parameters"];
        CHECK(params["type"] == "object");
        CHECK(params["properties"].is_object());
    }
}

TEST_CASE("Chat request: 已合规的 schema 原样放行,不动 required 与嵌套声明") {
    api::Request request;
    request.model = "m";
    const auto schema = nlohmann::json{
        {"type", "object"},
        {"properties", {{"path", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"path"})},
        {"additionalProperties", false},
    };
    request.tools.push_back({"read_file", "读文件", schema});

    const auto body = api::chat::BuildRequestJson(request);
    CHECK(body["tools"][0]["function"]["parameters"] == schema);
}


TEST_CASE("ModelImageBlock 重放:assistant content 带短标记,不带 base64") {
    api::Request request;
    request.model = "glm-5.2";
    api::ModelImageBlock ref;
    ref.id = "ig_1";
    ref.filename = "img-abcd12.png";
    ref.path = "images/img-abcd12.png";
    ref.width = 512;
    ref.height = 512;
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::TextBlock{"图好了"});
    assistant.content.push_back(ref);
    request.messages.push_back(assistant);

    const auto body = api::chat::BuildRequestJson(request, nlohmann::json::object(), api::chat::ChatRequestOptions{});
    const std::string dumped = body.dump();
    CHECK(dumped.find("[模型已生成图片: img-abcd12.png (512x512)]") != std::string::npos);
    CHECK(dumped.find("images/img-abcd12.png") == std::string::npos);
    CHECK(dumped.find("image_url") == std::string::npos);
}

TEST_CASE("工具结果图片: chat wire 的 tool 消息明降级——附注指路,字节不出门(工具结果图片回喂单)") {
    api::Request request;
    request.model = "glm-5.2";
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ToolUseBlock{"call_img", "gui_screenshot", nlohmann::json::object()});
    api::Message result_message;
    result_message.role = api::Role::User;
    api::ToolResultBlock rich;
    rich.tool_use_id = "call_img";
    rich.content = "[图片 art-00112233.png image/png 640x480 2048字节 artifact=mcp-artifacts/art-00112233.png]";
    lubancode::tools::ImageContent image;
    image.mime_type = "image/png";
    image.width = 640;
    image.height = 480;
    image.bytes = 2048;
    image.wire_base64 = "aGVsbG8=";  // 即便重灌过,chat wire 也不发——文档没有背书
    image.artifact.filename = "art-00112233.png";
    image.artifact.path = "mcp-artifacts/art-00112233.png";
    image.artifact.stored = true;
    rich.blocks.push_back(std::move(image));
    result_message.content.push_back(rich);
    request.messages.push_back(assistant);
    request.messages.push_back(result_message);

    const auto body = api::chat::BuildRequestJson(request);
    const auto& tool_message = body["messages"][1];
    REQUIRE(tool_message["role"] == "tool");
    const std::string content = tool_message["content"].get<std::string>();
    // 投影照旧在前,明降级附注点名张数与落盘文件名。
    CHECK(content.find(rich.content) != std::string::npos);
    CHECK(content.find("[wire 降级] 该 wire 不支持工具结果图片,1 张未随行,字节已存盘: art-00112233.png") !=
          std::string::npos);
    // 字节不出门:base64 不进请求体。
    CHECK(body.dump().find("aGVsbG8=") == std::string::npos);

    // 没有图片块的结果一个字节不加:纯文本结果的 content 原样。
    api::Request plain;
    plain.model = "glm-5.2";
    api::Message plain_result;
    plain_result.role = api::Role::User;
    plain_result.content.push_back(api::ToolResultBlock{"c9", "正文", false});
    plain.messages.push_back(plain_result);
    const auto plain_body = api::chat::BuildRequestJson(plain);
    CHECK(plain_body["messages"][0]["content"] == "正文");
}
