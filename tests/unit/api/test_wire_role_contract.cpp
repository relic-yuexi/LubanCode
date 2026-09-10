// 四角色 wire 合同测试册(session 轨迹 v3·P4,单子 §4.46)。
//
// 目的:钉死现行四家 wire(anthropic/chat/responses/gemini)对
// system/user/assistant/tool 四种内部含义的拍平形状,作为 v3 四角色
// 贯通时的对照基线。现状 api::Role 只有 User/Assistant 两枚:system 单列
// 在 Request::system,工具结果是 User 消息内的 ToolResultBlock。本册只读
// 现状、不改生产 wire 一行:每个断言都是"现行实现长什么样",不是"v3 应该
// 长什么样"。v3 四角色壳落地时,逐家对照本册与
// docs/architecture/trajectory-v3-schema.md §八的目标映射表;差距点见该节
// 差距清单。
//
// 本册不重复既有四册 request 测试的媒体/推理参数细节(那些在
// test_{anthropic,chat,responses,gemini}_request.cpp);这里钉的是
// "角色 -> wire 容器"这一层映射,一家一节,末尾两节横切对照。
//
// 末节另钉 usage 消费方要吃的活口径锚点(api::Usage 五项语义与
// TotalInputTokens),对应 schema 文档 §九的取数口盘点。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "api/anthropic/client.hpp"
#include "api/chat/request.hpp"
#include "api/gemini/request.hpp"
#include "api/responses/request.hpp"
#include "api/types.hpp"

namespace api = lubancode::api;

namespace {

// 单子 §4.46 的示例对话:S -> U -> A(call1, call2) -> T1 -> T2 -> A2。
// system 走 Request::system(现行唯一入口);assistant 带 thinking 块
// (含签名),工具调用两枚,结果分两条内部消息(v3 里是两枚独立 tool
// 角色;现行是两条只装 ToolResultBlock 的 User 消息)。四家 wire 的断言
// 全部吃同一份内部对话——对照表才对得齐。
api::Request CanonicalConversation() {
    api::Request request;
    request.model = "contract-model";
    request.system = "你是鲁班,守规矩。";

    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"读一下入口文件"});
    request.messages.push_back(user);

    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ThinkingBlock{"先想想从哪儿读", "sig-abc"});
    assistant.content.push_back(api::TextBlock{"我来读"});
    assistant.content.push_back(api::ToolUseBlock{"call_1", "read_file", nlohmann::json{{"path", "src/main.cpp"}}});
    assistant.content.push_back(api::ToolUseBlock{"call_2", "list_dir", nlohmann::json{{"dir", "src"}}});
    request.messages.push_back(assistant);

    api::Message tool1;
    tool1.role = api::Role::User;
    tool1.content.push_back(api::ToolResultBlock{"call_1", "#include <cstdio>\nint main() {}", false});
    request.messages.push_back(tool1);

    api::Message tool2;
    tool2.role = api::Role::User;
    tool2.content.push_back(api::ToolResultBlock{"call_2", "main.cpp api/ cli/", false});
    request.messages.push_back(tool2);

    api::Message closing;
    closing.role = api::Role::Assistant;
    closing.content.push_back(api::TextBlock{"读完了,入口很干净"});
    request.messages.push_back(closing);
    return request;
}

}  // namespace

// ---------------------------------------------------------------------------
// Anthropic Messages
// ---------------------------------------------------------------------------

TEST_CASE("Anthropic 基线: system 落顶层 system 字段,messages 里没有 system 角色") {
    const auto body = api::anthropic::BuildRequestJson(CanonicalConversation());
    REQUIRE(body.contains("system"));
    CHECK(body.at("system") == "你是鲁班,守规矩。");
    // system 不重复注入:messages 里一条 system 角色都不许有。
    for (const auto& message : body.at("messages")) {
        CHECK(message.at("role") != "system");
    }
}

TEST_CASE("Anthropic 基线: 内部消息逐条对位,五条 wire 消息次序与内部一致") {
    const auto body = api::anthropic::BuildRequestJson(CanonicalConversation());
    const auto& messages = body.at("messages");
    REQUIRE(messages.size() == 5);
    CHECK(messages[0].at("role") == "user");
    CHECK(messages[1].at("role") == "assistant");
    CHECK(messages[2].at("role") == "user");
    CHECK(messages[3].at("role") == "user");
    CHECK(messages[4].at("role") == "assistant");
    // user 正文是 user 消息 content 里的 text 块。
    CHECK(messages[0].at("content").at(0).at("type") == "text");
    CHECK(messages[0].at("content").at(0).at("text") == "读一下入口文件");
    // 收尾 assistant 正文是 text 块。
    CHECK(messages[4].at("content").at(0).at("type") == "text");
    CHECK(messages[4].at("content").at(0).at("text") == "读完了,入口很干净");
}

TEST_CASE("Anthropic 基线: thinking 块按原序保真回传,签名带上") {
    const auto body = api::anthropic::BuildRequestJson(CanonicalConversation());
    const auto& content = body.at("messages").at(1).at("content");
    REQUIRE(content.size() == 4);
    CHECK(content.at(0).at("type") == "thinking");
    CHECK(content.at(0).at("thinking") == "先想想从哪儿读");
    CHECK(content.at(0).at("signature") == "sig-abc");
}

TEST_CASE("Anthropic 基线: 工具调用是 assistant content 的 tool_use 块,同消息保序") {
    const auto body = api::anthropic::BuildRequestJson(CanonicalConversation());
    const auto& content = body.at("messages").at(1).at("content");
    REQUIRE(content.size() == 4);
    CHECK(content.at(1).at("type") == "text");
    CHECK(content.at(2).at("type") == "tool_use");
    CHECK(content.at(2).at("id") == "call_1");
    CHECK(content.at(2).at("name") == "read_file");
    CHECK(content.at(2).at("input").at("path") == "src/main.cpp");
    CHECK(content.at(3).at("type") == "tool_use");
    CHECK(content.at(3).at("id") == "call_2");
    CHECK(content.at(3).at("name") == "list_dir");
}

TEST_CASE("Anthropic 基线: 工具结果留在 user 容器的 tool_result 块,相邻两条不合并") {
    const auto body = api::anthropic::BuildRequestJson(CanonicalConversation());
    const auto& messages = body.at("messages");
    // 现行形状:T1/T2 两条内部消息各自成一条 user 消息,一条一枚
    // tool_result 块——不是 v3 目标里"同组合并成一条 user 的两个
    // tool_result 块"。这条钉子红了的那天,就是四角色分组落地的那天。
    REQUIRE(messages.size() == 5);
    const auto& first = messages.at(2).at("content");
    REQUIRE(first.size() == 1);
    CHECK(first.at(0).at("type") == "tool_result");
    CHECK(first.at(0).at("tool_use_id") == "call_1");
    CHECK(first.at(0).at("content") == "#include <cstdio>\nint main() {}");
    const auto& second = messages.at(3).at("content");
    REQUIRE(second.size() == 1);
    CHECK(second.at(0).at("type") == "tool_result");
    CHECK(second.at(0).at("tool_use_id") == "call_2");
}

// ---------------------------------------------------------------------------
// Chat Completions
// ---------------------------------------------------------------------------

TEST_CASE("Chat 基线: system 是 messages 首条 system 消息,content 纯字符串") {
    const auto body = api::chat::BuildRequestJson(CanonicalConversation());
    const auto& messages = body.at("messages");
    REQUIRE(messages.size() == 6);
    CHECK(messages[0].at("role") == "system");
    CHECK(messages[0].at("content") == "你是鲁班,守规矩。");
    // system 只此一条,不重复注入。
    int system_count = 0;
    for (const auto& message : messages) {
        if (message.at("role") == "system") {
            ++system_count;
        }
    }
    CHECK(system_count == 1);
}

TEST_CASE("Chat 基线: user 正文成 user 消息,thinking 默认不回传") {
    const auto body = api::chat::BuildRequestJson(CanonicalConversation());
    const auto& messages = body.at("messages");
    CHECK(messages[1].at("role") == "user");
    CHECK(messages[1].at("content") == "读一下入口文件");
    // 默认回传策略 Never:思考正文一字不出门,不造空串。
    CHECK_FALSE(messages[2].contains("reasoning_content"));
    CHECK_FALSE(messages[2].contains("reasoning"));
    CHECK(messages[2].at("content") == "我来读");
    CHECK(messages[5].at("role") == "assistant");
    CHECK(messages[5].at("content") == "读完了,入口很干净");
}

TEST_CASE("Chat 基线: 工具调用落 assistant.tool_calls 数组,arguments 是 JSON 串") {
    const auto body = api::chat::BuildRequestJson(CanonicalConversation());
    const auto& tool_calls = body.at("messages").at(2).at("tool_calls");
    REQUIRE(tool_calls.size() == 2);
    CHECK(tool_calls[0].at("id") == "call_1");
    CHECK(tool_calls[0].at("type") == "function");
    CHECK(tool_calls[0].at("function").at("name") == "read_file");
    CHECK(nlohmann::json::parse(tool_calls[0].at("function").at("arguments").get<std::string>())
              .at("path") == "src/main.cpp");
    CHECK(tool_calls[1].at("id") == "call_2");
    CHECK(tool_calls[1].at("function").at("name") == "list_dir");
}

TEST_CASE("Chat 基线: 工具结果是独立 role=tool 消息,tool_call_id 逐条配对") {
    const auto body = api::chat::BuildRequestJson(CanonicalConversation());
    const auto& messages = body.at("messages");
    // 内部 T1/T2 只装 ToolResultBlock: JoinedText 为空,不产 user 消息,
    // 只落两条 tool 消息——已经是 v3 目标表里"role=tool 与 tool_call_id"
    // 的形状。
    REQUIRE(messages.size() == 6);
    CHECK(messages[3].at("role") == "tool");
    CHECK(messages[3].at("tool_call_id") == "call_1");
    CHECK(messages[3].at("content") == "#include <cstdio>\nint main() {}");
    CHECK(messages[4].at("role") == "tool");
    CHECK(messages[4].at("tool_call_id") == "call_2");
    CHECK(messages[4].at("content") == "main.cpp api/ cli/");
}

// ---------------------------------------------------------------------------
// Responses
// ---------------------------------------------------------------------------

TEST_CASE("Responses 基线: system 落顶层 instructions,input 里没有 system 角色") {
    const auto body = api::responses::BuildRequestJson(CanonicalConversation());
    REQUIRE(body.contains("instructions"));
    CHECK(body.at("instructions") == "你是鲁班,守规矩。");
    for (const auto& item : body.at("input")) {
        if (item.at("type") == "message") {
            CHECK(item.at("role") != "system");
        }
    }
}

TEST_CASE("Responses 基线: 逐块成 item,thinking 跳过,文本按角色 input/output_text") {
    const auto body = api::responses::BuildRequestJson(CanonicalConversation());
    const auto& input = body.at("input");
    REQUIRE(input.size() == 7);
    CHECK(input[0].at("type") == "message");
    CHECK(input[0].at("role") == "user");
    CHECK(input[0].at("content").at(0).at("type") == "input_text");
    CHECK(input[0].at("content").at(0).at("text") == "读一下入口文件");
    // assistant 的 thinking 块被跳过(responses 的 reasoning 一次性),
    // 正文落 output_text。
    CHECK(input[1].at("type") == "message");
    CHECK(input[1].at("role") == "assistant");
    CHECK(input[1].at("content").at(0).at("type") == "output_text");
    CHECK(input[1].at("content").at(0).at("text") == "我来读");
    CHECK(input[6].at("type") == "message");
    CHECK(input[6].at("role") == "assistant");
    CHECK(input[6].at("content").at(0).at("text") == "读完了,入口很干净");
    // 思考正文不出现在任何 item 里。
    const std::string dumped = body.dump();
    CHECK(dumped.find("先想想从哪儿读") == std::string::npos);
}

TEST_CASE("Responses 基线: 工具调用成 function_call item,call_id 保真") {
    const auto body = api::responses::BuildRequestJson(CanonicalConversation());
    const auto& input = body.at("input");
    CHECK(input[2].at("type") == "function_call");
    CHECK(input[2].at("call_id") == "call_1");
    CHECK(input[2].at("name") == "read_file");
    CHECK(nlohmann::json::parse(input[2].at("arguments").get<std::string>()).at("path") == "src/main.cpp");
    CHECK(input[3].at("type") == "function_call");
    CHECK(input[3].at("call_id") == "call_2");
    CHECK(input[3].at("name") == "list_dir");
}

TEST_CASE("Responses 基线: 工具结果成 function_call_output item,call_id 配对") {
    const auto body = api::responses::BuildRequestJson(CanonicalConversation());
    const auto& input = body.at("input");
    CHECK(input[4].at("type") == "function_call_output");
    CHECK(input[4].at("call_id") == "call_1");
    CHECK(input[4].at("output") == "#include <cstdio>\nint main() {}");
    CHECK(input[5].at("type") == "function_call_output");
    CHECK(input[5].at("call_id") == "call_2");
    CHECK(input[5].at("output") == "main.cpp api/ cli/");
}

// ---------------------------------------------------------------------------
// Gemini Generate Content
// ---------------------------------------------------------------------------

TEST_CASE("Gemini 基线: system 落 systemInstruction,不进 contents") {
    const auto body = api::gemini::BuildRequestJson(CanonicalConversation());
    REQUIRE(body.contains("systemInstruction"));
    CHECK(body.at("systemInstruction").at("parts").at(0).at("text") == "你是鲁班,守规矩。");
    for (const auto& content : body.at("contents")) {
        CHECK(content.at("role") != "system");  // Gemini 的 contents 没有 system 这一角
    }
}

TEST_CASE("Gemini 基线: user/model 两个角色,thinking 跳过") {
    const auto body = api::gemini::BuildRequestJson(CanonicalConversation());
    const auto& contents = body.at("contents");
    REQUIRE(contents.size() == 7);
    CHECK(contents[0].at("role") == "user");
    CHECK(contents[0].at("parts").at(0).at("text") == "读一下入口文件");
    CHECK(contents[1].at("role") == "model");
    CHECK(contents[1].at("parts").at(0).at("text") == "我来读");
    CHECK(contents[6].at("role") == "model");
    CHECK(contents[6].at("parts").at(0).at("text") == "读完了,入口很干净");
    // 思考正文不回传(Gemini 的 thought 一次性)。
    const std::string dumped = body.dump();
    CHECK(dumped.find("先想想从哪儿读") == std::string::npos);
}

TEST_CASE("Gemini 基线: 工具调用成 model 的 functionCall 单条 content") {
    const auto body = api::gemini::BuildRequestJson(CanonicalConversation());
    const auto& contents = body.at("contents");
    CHECK(contents[2].at("role") == "model");
    CHECK(contents[2].at("parts").at(0).at("functionCall").at("name") == "read_file");
    CHECK(contents[2].at("parts").at(0).at("functionCall").at("args").at("path") == "src/main.cpp");
    CHECK(contents[3].at("role") == "model");
    CHECK(contents[3].at("parts").at(0).at("functionCall").at("name") == "list_dir");
}

TEST_CASE("Gemini 基线: 工具结果是 role=user 的 functionResponse,函数名按历史对回") {
    const auto body = api::gemini::BuildRequestJson(CanonicalConversation());
    const auto& contents = body.at("contents");
    // 协议只认函数名不认调用 id:wire 上没有 call_1/call_2,函数名是从
    // assistant 的 tool_use 块按 id 对回来的。这也是四角色贯通时
    // ToolNameByUseId 要吃独立 tool 消息 tool_call_id 的现行依据。
    CHECK(contents[4].at("role") == "user");
    CHECK(contents[4].at("parts").at(0).at("functionResponse").at("name") == "read_file");
    CHECK(contents[4].at("parts").at(0).at("functionResponse").at("response").at("result") ==
          "#include <cstdio>\nint main() {}");
    CHECK(contents[5].at("role") == "user");
    CHECK(contents[5].at("parts").at(0).at("functionResponse").at("name") == "list_dir");
    CHECK(contents[5].at("parts").at(0).at("functionResponse").at("response").at("result") == "main.cpp api/ cli/");
}

// ---------------------------------------------------------------------------
// 横切对照:同一份内部对话,四家的容器与数量映射
// ---------------------------------------------------------------------------

TEST_CASE("横切基线: 内外消息数量四家各不相等,请求快照不能假定一一相等") {
    const api::Request request = CanonicalConversation();
    // 内部 5 条消息(U/A/T1/T2/A2)+ 顶层 system。
    REQUIRE(request.messages.size() == 5);

    const auto anthropic = api::anthropic::BuildRequestJson(request);
    CHECK(anthropic.at("messages").size() == 5);  // 逐条对位,tool 结果不脱离 user 容器

    const auto chat = api::chat::BuildRequestJson(request);
    // system 消息 1 + U 1 + A 1 + tool 2 + A2 1;只装工具结果的内部 User
    // 消息不产 user 消息(空正文不造消息),一内一外数量从此分家。
    CHECK(chat.at("messages").size() == 6);

    const auto responses = api::responses::BuildRequestJson(request);
    // U 1 + A 正文 1(thinking 跳过)+ function_call 2 + function_call_output
    // 2 + A2 1;逐块成 item。
    CHECK(responses.at("input").size() == 7);

    const auto gemini = api::gemini::BuildRequestJson(request);
    // U 1 + A 正文 1 + functionCall 2 + functionResponse 2 + A2 1;工具块
    // 各自单独成条 content。
    CHECK(gemini.at("contents").size() == 7);
}

TEST_CASE("横切基线: user 正文与工具结果混装一条内部消息时,四家的分裂形状") {
    api::Request request;
    request.model = "m";
    api::Message mixed;
    mixed.role = api::Role::User;
    mixed.content.push_back(api::TextBlock{"这是正文"});
    mixed.content.push_back(api::ToolResultBlock{"call_x", "这是结果", false});
    request.messages.push_back(mixed);

    // Anthropic:不分裂——一条 user 消息,text 与 tool_result 同框。
    const auto anthropic = api::anthropic::BuildRequestJson(request);
    REQUIRE(anthropic.at("messages").size() == 1);
    REQUIRE(anthropic.at("messages").at(0).at("content").size() == 2);
    CHECK(anthropic.at("messages").at(0).at("content").at(0).at("type") == "text");
    CHECK(anthropic.at("messages").at(0).at("content").at(1).at("type") == "tool_result");

    // Chat:一分为二——user 消息(正文)在前,tool 消息(结果)在后。
    const auto chat = api::chat::BuildRequestJson(request);
    REQUIRE(chat.at("messages").size() == 2);
    CHECK(chat.at("messages").at(0).at("role") == "user");
    CHECK(chat.at("messages").at(0).at("content") == "这是正文");
    CHECK(chat.at("messages").at(1).at("role") == "tool");
    CHECK(chat.at("messages").at(1).at("tool_call_id") == "call_x");
    CHECK(chat.at("messages").at(1).at("content") == "这是结果");

    // Responses:一分为二——message item 与 function_call_output item。
    const auto responses = api::responses::BuildRequestJson(request);
    REQUIRE(responses.at("input").size() == 2);
    CHECK(responses.at("input").at(0).at("type") == "message");
    CHECK(responses.at("input").at(0).at("content").at(0).at("text") == "这是正文");
    CHECK(responses.at("input").at(1).at("type") == "function_call_output");
    CHECK(responses.at("input").at(1).at("call_id") == "call_x");

    // Gemini:一分为二——user 文本 content 与 functionResponse content。
    const auto gemini = api::gemini::BuildRequestJson(request);
    REQUIRE(gemini.at("contents").size() == 2);
    CHECK(gemini.at("contents").at(0).at("role") == "user");
    CHECK(gemini.at("contents").at(0).at("parts").at(0).at("text") == "这是正文");
    CHECK(gemini.at("contents").at(1).at("role") == "user");
    CHECK(gemini.at("contents").at(1).at("parts").at(0).at("functionResponse").at("response").at("result") ==
          "这是结果");
}

TEST_CASE("横切基线: system 为空时四家都不落 system 键/消息") {
    api::Request request;
    request.model = "m";
    request.messages.push_back(api::Message{api::Role::User, {api::TextBlock{"只此一条"}}});

    CHECK_FALSE(api::anthropic::BuildRequestJson(request).contains("system"));
    CHECK_FALSE(api::responses::BuildRequestJson(request).contains("instructions"));
    CHECK_FALSE(api::gemini::BuildRequestJson(request).contains("systemInstruction"));
    const auto chat = api::chat::BuildRequestJson(request);
    REQUIRE(chat.at("messages").size() == 1);
    CHECK(chat.at("messages").at(0).at("role") == "user");
}

// ---------------------------------------------------------------------------
// usage 消费方的活口径锚点(schema 文档 §九盘点对应的测试钩子)
// ---------------------------------------------------------------------------

TEST_CASE("usage 锚点: api::Usage 五项是 v3 owner 键集的活口径对应,TotalInputTokens 求完整输入") {
    // v3 唯一 owner 的 usage 结构键(inputTokens/outputTokens/reasoningTokens/
    // cacheReadTokens/cacheWriteTokens,缺子项省键)与 api::Usage 五项一一对应:
    //   inputTokens -> input_tokens(非缓存输入)
    //   cacheReadTokens -> cache_read_tokens(缓存读命中)
    //   cacheWriteTokens -> cache_creation_tokens(缓存写)
    //   outputTokens -> output_tokens(输出,reasoning 含在内不另加)
    //   reasoningTokens -> output_reasoning_tokens(输出里 reasoning 部分,
    //                        服务端拆了账才非 0)
    // 未来消费方(/usage、token 账本、cost、calibrator)吃 v3 账时统一经
    // api::Usage 这副口径折算;完整输入一律走 TotalInputTokens,不许各家
    // 各算(49k 命中 + 1k 未命中 = 50k,不是 50k,更不是 1k)。
    api::Usage usage;
    usage.input_tokens = 1000;
    usage.cache_read_tokens = 49000;
    usage.cache_creation_tokens = 2000;
    usage.output_tokens = 300;
    usage.output_reasoning_tokens = 120;
    CHECK(api::TotalInputTokens(usage) == 52000);

    // reasoning 已含在 output 里:完整输入不因 reasoning 拆账而变。
    api::Usage no_reasoning_split = usage;
    no_reasoning_split.output_reasoning_tokens = 0;
    CHECK(api::TotalInputTokens(no_reasoning_split) == 52000);

    // 缺报不补 0:五项全零的 Usage 与"没报"靠明报位分家,数值层不冒充。
    api::Usage unreported;
    CHECK(api::TotalInputTokens(unreported) == 0);
    api::UsageReport report;
    CHECK_FALSE(report.reported_by_provider);
    CHECK_FALSE(report.reported());
}
