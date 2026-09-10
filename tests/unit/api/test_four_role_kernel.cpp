// 四角色内核(轨迹 v3·消息主轴,单子 §1.1/§4.46)。第一棒 Anthropic,
// 第二棒 chat/responses/gemini 三家,第三棒差距清单尾巴 6/7/8 三条。
//
// 换骨不换皮:内部消息模型升四角色(system/user/assistant/tool 一等
// 公民),wire 出口形状一字不变。本册钉:
//
//   1. 内核件——api::Role 四枚枚举与 RoleToString 共用件的拍平语义;
//   2. Anthropic 换骨(第一棒)——System 消息顶置顶层 system、Tool 角色
//      折 user 容器 tool_result 块、相邻不合并、ShouldRecoverTaggedThinking
//      认 tool 角色(旧式 User+ToolResultBlock 共存同认);
//   3. chat/responses/gemini 换骨(第二棒,差距清单 §8.2 第 2/4/5 条)
//      ——chat 的 System 落 system role 消息、Tool 直落 role=tool
//      (tool_call_id 配对)、IsUserTurnStart 段判据按内部角色;responses
//      的 System 顶置 instructions、Tool 折 function_call_output item
//      (文本部件 input_text/output_text 只认 assistant 一角);gemini 的
//      System 顶置 systemInstruction、Tool 折 role=user 的 functionResponse
//      (ToolNameByUseId 对回表不扫角色,新旧同表同对);
//   4. 总钉子——同一份对话用"两角色旧路"(Request::system + User 容器
//      ToolResultBlock)与"四角色新路"(System 消息 + Tool 角色)分别
//      组装,四家出口 JSON 逐字节相等。这就是"内里换骨、外观不变"的
//      直接证据;红了说明换骨漏了或拍平变了。
//   5. 第三棒(差距清单 §8.2 第 6/7/8 条)——thinking/redacted_thinking
//      原生块的进出核对(§4.42 不透明块无损回传、空签名不虚构);内部
//      消息序 -> wire 元素序的拍平对照(v3 账 prepared 的 inputMessageRefs
//      对账用);容量取数口读 extra_body 覆盖后的有效输出上限。
//
// 四家合同基线在 test_wire_role_contract.cpp(18 案,断言不动);本册只
// 钉四角色新增面,不重复四家横切对照。
//
// 断言纪律:json 缺键一律 contains() 判,禁止 const json 上 operator[]
// 查缺键(nlohmann UB)。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "api/anthropic/client.hpp"
#include "api/anthropic/events.hpp"
#include "api/assembler.hpp"
#include "api/backend.hpp"
#include "api/chat/client.hpp"
#include "api/chat/request.hpp"
#include "api/gemini/client.hpp"
#include "api/gemini/request.hpp"
#include "api/responses/client.hpp"
#include "api/responses/request.hpp"
#include "api/sse_framing.hpp"
#include "api/types.hpp"

namespace api = lubancode::api;

namespace {

// 与合同册同款的示例对话(system 走 Request::system,工具结果寄 User
// 容器)——现行两角色路径的标准输入。
api::Request TwoRoleConversation() {
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

// 同一份对话的四角色版:system 是上下文根消息(不再走 Request::system),
// 两条工具结果是独立的 Tool 角色消息(不再伪装 user)。消息的"业务内容"
// 与两角色版逐块相同——出口才有得比。
api::Request FourRoleConversation() {
    api::Request request;
    request.model = "contract-model";

    api::Message system;
    system.role = api::Role::System;
    system.content.push_back(api::TextBlock{"你是鲁班,守规矩。"});
    request.messages.push_back(system);

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
    tool1.role = api::Role::Tool;
    tool1.content.push_back(api::ToolResultBlock{"call_1", "#include <cstdio>\nint main() {}", false});
    request.messages.push_back(tool1);

    api::Message tool2;
    tool2.role = api::Role::Tool;
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
// 内核件:四枚枚举与共用件 RoleToString
// ---------------------------------------------------------------------------

TEST_CASE("内核: Role 有 System/Tool 两枚一等公民,枚举按名取值不动旧两枚") {
    // 四枚都能构造、都能比较——这是一等公民的编译面;旧两枚的取值一枚
    // 不动(现行持久化按名不按数值,这里钉住相对次序防手滑重排)。
    CHECK(api::Role::User != api::Role::System);
    CHECK(api::Role::User != api::Role::Tool);
    CHECK(api::Role::Assistant != api::Role::System);
    CHECK(api::Role::Assistant != api::Role::Tool);
    CHECK(api::Role::System != api::Role::Tool);
    CHECK(api::Role::User != api::Role::Assistant);
}

TEST_CASE("内核: RoleToString 把 Tool 折 user 一侧,System 防御折 user 不造第三角") {
    // 共用件(差距清单 §8.2 第 1 条核对点):wire 容器没有 system/tool 角。
    // Tool 折 user 与四家 wire 的工具结果容器一致;System 本该由 adapter
    // 顶置到顶层字段、不该走到这层,防御性到达也折 user,不折另一角
    //(assistant/model)少造一重假象。现行 User/Assistant 的输出不变。
    CHECK(api::RoleToString(api::Role::User, "assistant") == "user");
    CHECK(api::RoleToString(api::Role::Assistant, "assistant") == "assistant");
    CHECK(api::RoleToString(api::Role::Assistant, "model") == "model");
    CHECK(api::RoleToString(api::Role::Tool, "assistant") == "user");
    CHECK(api::RoleToString(api::Role::Tool, "model") == "user");
    CHECK(api::RoleToString(api::Role::System, "assistant") == "user");
}

// ---------------------------------------------------------------------------
// Anthropic 换骨:System 顶置、Tool 折 user 容器
// ---------------------------------------------------------------------------

TEST_CASE("Anthropic 换骨: System 消息顶置顶层 system,对话流一条不落") {
    const auto body = api::anthropic::BuildRequestJson(FourRoleConversation());
    REQUIRE(body.contains("system"));
    CHECK(body.at("system") == "你是鲁班,守规矩。");
    // 上下文根不重复注入:messages 里没有 system 角色,一条都不许有。
    for (const auto& message : body.at("messages")) {
        CHECK(message.at("role") != "system");
    }
}

TEST_CASE("Anthropic 换骨: Request.system 先行,多条 System 消息按序接后") {
    // 共存期拼接规矩:Request::system(现行两角色路径的唯一 system 入口)
    // 在前,System 消息按消息序接在其后,"\n" 连接、空段不造。
    api::Request request;
    request.model = "m";
    request.system = "根系统提示";

    api::Message first;
    first.role = api::Role::System;
    first.content.push_back(api::TextBlock{"第一段追加"});
    request.messages.push_back(first);

    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"问一句"});
    request.messages.push_back(user);

    api::Message second;
    second.role = api::Role::System;
    second.content.push_back(api::TextBlock{"换 soul 后的新版"});
    second.content.push_back(api::TextBlock{"第二块同段"});
    request.messages.push_back(second);

    const auto body = api::anthropic::BuildRequestJson(request);
    REQUIRE(body.contains("system"));
    CHECK(body.at("system") == "根系统提示\n第一段追加\n换 soul 后的新版\n第二块同段");
    // System 消息不占对话位:user 一条,末尾没有多出来的空消息。
    REQUIRE(body.at("messages").size() == 1);
    CHECK(body.at("messages").at(0).at("role") == "user");
    CHECK(body.at("messages").at(0).at("content").at(0).at("text") == "问一句");
}

TEST_CASE("Anthropic 换骨: system 与 System 消息都空时不落 system 键") {
    api::Request request;
    request.model = "m";
    api::Message empty_system;
    empty_system.role = api::Role::System;  // 空正文:不造键
    request.messages.push_back(empty_system);
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"只此一条"});
    request.messages.push_back(user);

    const auto body = api::anthropic::BuildRequestJson(request);
    CHECK_FALSE(body.contains("system"));
    REQUIRE(body.at("messages").size() == 1);
}

TEST_CASE("Anthropic 换骨: System 消息只取 TextBlock,富块不进顶层 system") {
    // v3 §1.2 的 system 是 soul/规则文本;图片等富块 system 后续棒次需要
    // 再扩——现在的行为面是"不悄悄丢进去也不硬造块数组",钉住防漂移。
    api::Request request;
    request.model = "m";
    api::Message system;
    system.role = api::Role::System;
    system.content.push_back(api::TextBlock{"规则文本"});
    system.content.push_back(api::ImageBlock{"image/png", "AAAA", "a.png", 0, 0});
    request.messages.push_back(system);
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"问"});
    request.messages.push_back(user);

    const auto body = api::anthropic::BuildRequestJson(request);
    REQUIRE(body.contains("system"));
    CHECK(body.at("system") == "规则文本");
    CHECK_FALSE(body.at("system").is_array());
    const std::string dumped = body.dump();
    CHECK(dumped.find("AAAA") == std::string::npos);  // 富块不出门
}

TEST_CASE("Anthropic 换骨: Tool 角色折 user 容器 tool_result 块,相邻不合并") {
    const auto body = api::anthropic::BuildRequestJson(FourRoleConversation());
    const auto& messages = body.at("messages");
    // 与合同册现行基线同形:S/U/A(calls)/T1/T2/A2 出 5 条 wire 消息,
    // T1/T2 各自成条 user、一条一枚 tool_result 块。相邻同组合并是 v3
    // 目标形状,归后续棒次(合并会红合同钉子,本棒不并)。
    REQUIRE(messages.size() == 5);
    CHECK(messages.at(0).at("role") == "user");
    CHECK(messages.at(1).at("role") == "assistant");
    CHECK(messages.at(2).at("role") == "user");
    CHECK(messages.at(3).at("role") == "user");
    CHECK(messages.at(4).at("role") == "assistant");
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

TEST_CASE("Anthropic 换骨: Tool 消息正文与结果混装时同框不分裂,块序保真") {
    // 对照合同册横切节的 anthropic 行状:user 容器内 text 与 tool_result
    // 同框。Tool 角色沿用同一拍平——不因换了角色就改分裂形状。
    api::Request request;
    request.model = "m";
    api::Message mixed;
    mixed.role = api::Role::Tool;
    mixed.content.push_back(api::TextBlock{"附言"});
    mixed.content.push_back(api::ToolResultBlock{"call_x", "这是结果", false});
    request.messages.push_back(mixed);

    const auto body = api::anthropic::BuildRequestJson(request);
    REQUIRE(body.at("messages").size() == 1);
    CHECK(body.at("messages").at(0).at("role") == "user");
    REQUIRE(body.at("messages").at(0).at("content").size() == 2);
    CHECK(body.at("messages").at(0).at("content").at(0).at("type") == "text");
    CHECK(body.at("messages").at(0).at("content").at(1).at("type") == "tool_result");
    CHECK(body.at("messages").at(0).at("content").at(1).at("tool_use_id") == "call_x");
}

// ---------------------------------------------------------------------------
// 总钉子:换骨不换皮
// ---------------------------------------------------------------------------

TEST_CASE("总钉子: 同一对话两角色旧路与四角色新路出口逐字节相等") {
    const auto legacy = api::anthropic::BuildRequestJson(TwoRoleConversation());
    const auto four_role = api::anthropic::BuildRequestJson(FourRoleConversation());
    // 内部模型换了骨(system 消息化、tool 角色独立),wire 出口一字不变。
    // dump 含键序(nlohmann 按键名字典序)+ 数组序 + 全部值,逐字节相等
    // 即形状、次序、内容全同。
    CHECK(four_role.dump() == legacy.dump());
}

TEST_CASE("总钉子: 两角色旧路的出口与合同基线自证(5 条 messages 防串扰)") {
    // 换骨改动不许把旧路的形状带偏:两角色版出口与合同册断言的关键节
    // 再钉一遍(system 顶层、5 条消息、thinking 签名、tool_use 保序)。
    const auto legacy = api::anthropic::BuildRequestJson(TwoRoleConversation());
    REQUIRE(legacy.at("messages").size() == 5);
    CHECK(legacy.at("messages").at(1).at("content").at(0).at("type") == "thinking");
    CHECK(legacy.at("messages").at(1).at("content").at(0).at("signature") == "sig-abc");
    CHECK(legacy.at("messages").at(1).at("content").at(2).at("type") == "tool_use");
    CHECK(legacy.at("messages").at(1).at("content").at(2).at("id") == "call_1");
}

// ---------------------------------------------------------------------------
// ShouldRecoverTaggedThinking:工具续轮判定认 tool 角色,旧式共存
// ---------------------------------------------------------------------------

namespace {

// "assistant(thinking + tool_use) -> 末条工具结果"的续轮形状骨架。
// tail_role 给 Tool 是四角色新路;给 User 是旧两角色路径(ToolResultBlock
// 寄 User 容器)。
api::Request TaggedThinkingSkeleton(api::Role tail_role) {
    api::Request request;
    request.model = "m";
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ThinkingBlock{"想想", "sig"});
    assistant.content.push_back(api::ToolUseBlock{"c1", "read_file", nlohmann::json::object()});
    request.messages.push_back(assistant);

    api::Message tail;
    tail.role = tail_role;
    tail.content.push_back(api::ToolResultBlock{"c1", "结果", false});
    request.messages.push_back(tail);
    return request;
}

}  // namespace

TEST_CASE("换骨: ShouldRecoverTaggedThinking 末条 tool 角色触发,旧式 User 容器同认") {
    CHECK(api::anthropic::ShouldRecoverTaggedThinking(TaggedThinkingSkeleton(api::Role::Tool)));
    CHECK(api::anthropic::ShouldRecoverTaggedThinking(TaggedThinkingSkeleton(api::Role::User)));

    // 末条是真 user 文本(无工具结果):不触发——兼容门不许放宽到普通轮。
    api::Request plain = TaggedThinkingSkeleton(api::Role::User);
    plain.messages.back().content.clear();
    plain.messages.back().content.push_back(api::TextBlock{"就聊聊"});
    CHECK_FALSE(api::anthropic::ShouldRecoverTaggedThinking(plain));

    // 末条 System(上下文根消息顶到末尾的怪形状):不触发,tool/system
    // 各归各位,system 不该踩工具续轮的判定。
    api::Request system_tail = TaggedThinkingSkeleton(api::Role::System);
    CHECK_FALSE(api::anthropic::ShouldRecoverTaggedThinking(system_tail));
}

// ---------------------------------------------------------------------------
// Chat 换骨(第二棒,差距清单 §8.2 第 2 条):System 落 system 消息、
// Tool 直落 role=tool
// ---------------------------------------------------------------------------

TEST_CASE("Chat 换骨: System 消息落 system role 消息,Request.system 先行按序接后") {
    // Chat 协议没有顶层 system 参数:system 是消息流首条 system role 消息。
    // 共存期拼接规矩与 Anthropic 第一棒同款——Request::system 先行,
    // System 消息按消息序接后,"\n" 连接、空段不造,多源拼一条。
    api::Request request;
    request.model = "m";
    request.system = "根系统提示";

    api::Message first;
    first.role = api::Role::System;
    first.content.push_back(api::TextBlock{"第一段追加"});
    request.messages.push_back(first);

    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"问一句"});
    request.messages.push_back(user);

    api::Message second;
    second.role = api::Role::System;
    second.content.push_back(api::TextBlock{"换 soul 后的新版"});
    second.content.push_back(api::TextBlock{"第二块同段"});
    request.messages.push_back(second);

    const auto body = api::chat::BuildRequestJson(request);
    const auto& messages = body.at("messages");
    REQUIRE(messages.size() == 2);  // system 一条 + user 一条,System 不占对话位
    CHECK(messages.at(0).at("role") == "system");
    CHECK(messages.at(0).at("content") == "根系统提示\n第一段追加\n换 soul 后的新版\n第二块同段");
    CHECK(messages.at(1).at("role") == "user");
    CHECK(messages.at(1).at("content") == "问一句");
    // system 只此一条,不重复注入。
    int system_count = 0;
    for (const auto& message : messages) {
        if (message.at("role") == "system") {
            ++system_count;
        }
    }
    CHECK(system_count == 1);
}

TEST_CASE("Chat 换骨: system 与 System 消息都空时不产 system 消息") {
    api::Request request;
    request.model = "m";
    api::Message empty_system;
    empty_system.role = api::Role::System;  // 空正文:不造消息
    request.messages.push_back(empty_system);
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"只此一条"});
    request.messages.push_back(user);

    const auto body = api::chat::BuildRequestJson(request);
    REQUIRE(body.at("messages").size() == 1);
    CHECK(body.at("messages").at(0).at("role") == "user");
}

TEST_CASE("Chat 换骨: Tool 角色直落 role=tool 消息,tool_call_id 逐条配对") {
    const auto body = api::chat::BuildRequestJson(FourRoleConversation());
    const auto& messages = body.at("messages");
    // 现行拍平形状(合同册 Chat 基线):S/U/A(calls)/T1/T2/A2 出 6 条——
    // system 1 + user 1 + assistant 1 + tool 2 + assistant 1。Tool 角色
    // 只装 ToolResultBlock 时 JoinedText 为空,不产 user 消息(空正文不造)。
    REQUIRE(messages.size() == 6);
    CHECK(messages.at(0).at("role") == "system");
    CHECK(messages.at(1).at("role") == "user");
    CHECK(messages.at(2).at("role") == "assistant");
    CHECK(messages.at(3).at("role") == "tool");
    CHECK(messages.at(3).at("tool_call_id") == "call_1");
    CHECK(messages.at(3).at("content") == "#include <cstdio>\nint main() {}");
    CHECK(messages.at(4).at("role") == "tool");
    CHECK(messages.at(4).at("tool_call_id") == "call_2");
    CHECK(messages.at(4).at("content") == "main.cpp api/ cli/");
    CHECK(messages.at(5).at("role") == "assistant");
}

TEST_CASE("Chat 换骨: Tool 角色不启新交互段,tool_episode 段标记照旧生效") {
    // IsUserTurnStart/SegmentToolUseFlags 的判据核对(差距清单 §8.2 第 2
    // 条):按内部消息角色判定,不扫 wire role 倒推——Tool 角色的消息不是
    // "真 user 输入",不把交互段切断。带 thinking 的工具段在 tool_episode
    // 策略下,旧路(工具结果寄 User 容器)与新路(独立 Tool 角色)出口
    // 逐字节相等;且两条路都要真回传了 reasoning_content——dump 相等若都
    // 缺回传,判据就白钉了。
    api::chat::ChatRequestOptions options;
    options.reasoning_replay = api::chat::ReasoningReplayPolicy::ToolEpisode;

    const auto legacy = api::chat::BuildRequestJson(TwoRoleConversation(), nlohmann::json::object(), options);
    const auto four_role =
        api::chat::BuildRequestJson(FourRoleConversation(), nlohmann::json::object(), options);
    CHECK(four_role.dump() == legacy.dump());

    REQUIRE(legacy.at("messages").at(2).contains("reasoning_content"));
    CHECK(legacy.at("messages").at(2).at("reasoning_content") == "先想想从哪儿读");
    REQUIRE(four_role.at("messages").at(2).contains("reasoning_content"));
    CHECK(four_role.at("messages").at(2).at("reasoning_content") == "先想想从哪儿读");
}

TEST_CASE("Chat 换骨总钉子: 同一对话两角色旧路与四角色新路出口逐字节相等") {
    const auto legacy = api::chat::BuildRequestJson(TwoRoleConversation());
    const auto four_role = api::chat::BuildRequestJson(FourRoleConversation());
    CHECK(four_role.dump() == legacy.dump());
}

// ---------------------------------------------------------------------------
// Responses 换骨(第二棒,差距清单 §8.2 第 4 条):System 顶置
// instructions、Tool 折 function_call_output
// ---------------------------------------------------------------------------

TEST_CASE("Responses 换骨: System 消息顶置 instructions,input 里一条不落") {
    const auto body = api::responses::BuildRequestJson(FourRoleConversation());
    REQUIRE(body.contains("instructions"));
    CHECK(body.at("instructions") == "你是鲁班,守规矩。");
    // 上下文根不重复注入:input 里没有 system 角色的 message item。
    for (const auto& item : body.at("input")) {
        if (item.at("type") == "message") {
            CHECK(item.at("role") != "system");
        }
    }
}

TEST_CASE("Responses 换骨: Request.system 先行,System 消息按序接后") {
    api::Request request;
    request.model = "m";
    request.system = "根指令";

    api::Message first;
    first.role = api::Role::System;
    first.content.push_back(api::TextBlock{"第一段追加"});
    request.messages.push_back(first);

    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"问一句"});
    request.messages.push_back(user);

    api::Message second;
    second.role = api::Role::System;
    second.content.push_back(api::TextBlock{"换 soul 后的新版"});
    request.messages.push_back(second);

    const auto body = api::responses::BuildRequestJson(request);
    REQUIRE(body.contains("instructions"));
    CHECK(body.at("instructions") == "根指令\n第一段追加\n换 soul 后的新版");
    // System 消息不占 input 位。
    REQUIRE(body.at("input").size() == 1);
    CHECK(body.at("input").at(0).at("role") == "user");
}

TEST_CASE("Responses 换骨: Tool 角色折 function_call_output,文本块落 input_text") {
    // 角色三元核对:TextPartType 只认 assistant 一角——Tool 角色的文本块
    // 折 user 侧(input_text),不是 output_text;ToolResultBlock 照旧
    // function_call_output item(协议形状与角色无关)。
    api::Request request;
    request.model = "m";

    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ToolUseBlock{"call_x", "read_file", nlohmann::json::object()});
    request.messages.push_back(assistant);

    api::Message tool;
    tool.role = api::Role::Tool;
    tool.content.push_back(api::TextBlock{"附言"});
    tool.content.push_back(api::ToolResultBlock{"call_x", "这是结果", false});
    request.messages.push_back(tool);

    const auto body = api::responses::BuildRequestJson(request);
    const auto& input = body.at("input");
    REQUIRE(input.size() == 3);
    CHECK(input.at(0).at("type") == "function_call");
    CHECK(input.at(0).at("call_id") == "call_x");
    CHECK(input.at(1).at("type") == "message");
    CHECK(input.at(1).at("role") == "user");
    CHECK(input.at(1).at("content").at(0).at("type") == "input_text");
    CHECK(input.at(1).at("content").at(0).at("text") == "附言");
    CHECK(input.at(2).at("type") == "function_call_output");
    CHECK(input.at(2).at("call_id") == "call_x");
    CHECK(input.at(2).at("output") == "这是结果");
}

TEST_CASE("Responses 换骨总钉子: 同一对话两角色旧路与四角色新路出口逐字节相等") {
    const auto legacy = api::responses::BuildRequestJson(TwoRoleConversation());
    const auto four_role = api::responses::BuildRequestJson(FourRoleConversation());
    CHECK(four_role.dump() == legacy.dump());
}

// ---------------------------------------------------------------------------
// Gemini 换骨(第二棒,差距清单 §8.2 第 5 条):System 顶置
// systemInstruction、Tool 折 role=user 的 functionResponse
// ---------------------------------------------------------------------------

TEST_CASE("Gemini 换骨: System 消息顶置 systemInstruction,contents 里一条不落") {
    const auto body = api::gemini::BuildRequestJson(FourRoleConversation());
    REQUIRE(body.contains("systemInstruction"));
    CHECK(body.at("systemInstruction").at("parts").at(0).at("text") == "你是鲁班,守规矩。");
    // Gemini 的 contents 没有 system 这一角。
    for (const auto& content : body.at("contents")) {
        CHECK(content.at("role") != "system");
    }
}

TEST_CASE("Gemini 换骨: Tool 角色折 role=user 的 functionResponse,函数名按历史对回") {
    const auto body = api::gemini::BuildRequestJson(FourRoleConversation());
    const auto& contents = body.at("contents");
    // ToolNameByUseId 对回表不扫消息角色:Tool 角色消息携带的 tool_call_id
    // 照旧从 assistant 的 ToolUseBlock 对回函数名。functionResponse 的
    // role 经 WireRole 落位("user" 现行形状保持)。
    REQUIRE(contents.size() == 7);
    CHECK(contents.at(4).at("role") == "user");
    CHECK(contents.at(4).at("parts").at(0).at("functionResponse").at("name") == "read_file");
    CHECK(contents.at(4).at("parts").at(0).at("functionResponse").at("response").at("result") ==
          "#include <cstdio>\nint main() {}");
    CHECK(contents.at(5).at("role") == "user");
    CHECK(contents.at(5).at("parts").at(0).at("functionResponse").at("name") == "list_dir");
}

TEST_CASE("Gemini 换骨: Tool 消息对不上号时退 id 当函数名") {
    // 对回表认 tool 消息的防御面:上游少了那条 assistant 调用消息时,
    // Tool 角色的 tool_call_id 对不上号,退回 id 本身当函数名——与旧路
    //(User 容器)同一兜底,不悄悄丢结果。
    api::Request request;
    request.model = "m";
    api::Message tool;
    tool.role = api::Role::Tool;
    tool.content.push_back(api::ToolResultBlock{"orphan_call", "孤儿结果", false});
    request.messages.push_back(tool);

    const auto body = api::gemini::BuildRequestJson(request);
    const auto& contents = body.at("contents");
    REQUIRE(contents.size() == 1);
    CHECK(contents.at(0).at("role") == "user");
    CHECK(contents.at(0).at("parts").at(0).at("functionResponse").at("name") == "orphan_call");
    CHECK(contents.at(0).at("parts").at(0).at("functionResponse").at("response").at("result") == "孤儿结果");
}

TEST_CASE("Gemini 换骨总钉子: 同一对话两角色旧路与四角色新路出口逐字节相等") {
    const auto legacy = api::gemini::BuildRequestJson(TwoRoleConversation());
    const auto four_role = api::gemini::BuildRequestJson(FourRoleConversation());
    CHECK(four_role.dump() == legacy.dump());
}

// ---------------------------------------------------------------------------
// 混装换骨横切:正文与工具结果同框的内部消息,四家新旧两路出口逐字节相等
// ---------------------------------------------------------------------------

TEST_CASE("混装横切: 正文与结果同框的 Tool 消息,四家新旧两路出口逐字节相等") {
    // 混装是最容易走岔的形状(一内变两外)。旧路:User 容器混装 Text +
    // ToolResultBlock(合同册横切节钉过形状);新路:Tool 角色混装同样
    // 的块。四家出口逐字节相等 = 角色一换,拍平纹丝不动。
    api::Request legacy;
    legacy.model = "m";
    api::Message mixed;
    mixed.role = api::Role::User;
    mixed.content.push_back(api::TextBlock{"这是正文"});
    mixed.content.push_back(api::ToolResultBlock{"call_x", "这是结果", false});
    legacy.messages.push_back(mixed);

    api::Request four_role = legacy;
    four_role.messages.back().role = api::Role::Tool;

    CHECK(api::anthropic::BuildRequestJson(four_role).dump() ==
          api::anthropic::BuildRequestJson(legacy).dump());
    CHECK(api::chat::BuildRequestJson(four_role).dump() == api::chat::BuildRequestJson(legacy).dump());
    CHECK(api::responses::BuildRequestJson(four_role).dump() ==
          api::responses::BuildRequestJson(legacy).dump());
    CHECK(api::gemini::BuildRequestJson(four_role).dump() == api::gemini::BuildRequestJson(legacy).dump());
}

// ---------------------------------------------------------------------------
// 差距 6(§8.2 第 6 条):thinking/redacted_thinking 原生块——解析、留档、
// 四家进出;空签名不虚构,K2.6 回传路不回退
// ---------------------------------------------------------------------------

namespace {

api::SseFrame RedactedFrame(std::string data_json) {
    return api::SseFrame{"message", std::move(data_json)};
}

}  // namespace

TEST_CASE("差距6: redacted_thinking 流入——解析器认原生块,assembler 落事实块") {
    // 差距清单 §8.2 第 6 条:anthropic 解析器原先没有该分支,块被静默
    // 跳过。现在 content_block_start 带 redacted_thinking 即映射出
    // RedactedThinking(整块到齐,无增量),assembler 攒进 assistant
    // content——块序即流序。
    const auto event = api::anthropic::parse_event(RedactedFrame(
        R"({"type":"content_block_start","index":1,"content_block":{"type":"redacted_thinking","data":"b3BhcXVlLXNlcmdl"}})"));
    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<api::RedactedThinking>(*event));
    CHECK(std::get<api::RedactedThinking>(*event).data == "b3BhcXVlLXNlcmdl");

    // 流式拼装:thinking(空签名)在前、redacted 居中、正文收尾——顺序
    // 原样落历史,不重排不丢块。
    api::MessageAssembler assembler;
    assembler.Feed(api::ThinkingDelta{"想一想", ""});
    assembler.Feed(api::ContentBlockDone{0});
    assembler.Feed(*event);
    assembler.Feed(api::TextDelta{"正文来了"});
    assembler.Feed(api::ContentBlockDone{2});
    assembler.Feed(api::MessageDone{"end_turn", {}, false, false});
    const api::Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 3);
    REQUIRE(std::holds_alternative<api::ThinkingBlock>(message.content[0]));
    REQUIRE(std::holds_alternative<api::RedactedThinkingBlock>(message.content[1]));
    CHECK(std::get<api::RedactedThinkingBlock>(message.content[1]).data == "b3BhcXVlLXNlcmdl");
    REQUIRE(std::holds_alternative<api::TextBlock>(message.content[2]));
}

TEST_CASE("差距6: Anthropic 出口 redacted_thinking 原样回传,空签名照实不虚构") {
    // §4.42 不透明块无损回传:wire 给过的 data 一字不少地带回去;兼容端
    // 回的空签名 thinking 块照实回传空串(差距清单 §8.2 第 6 条"不得按
    // Claude 非空签名要求虚构"),既不造一枚假签名也不丢块。
    api::Request request;
    request.model = "m";
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ThinkingBlock{"想想", ""});  // 兼容端空签名
    assistant.content.push_back(api::RedactedThinkingBlock{"b3BhcXVlLXNlcmdl"});
    assistant.content.push_back(api::TextBlock{"结论"});
    request.messages.push_back(assistant);

    const auto body = api::anthropic::BuildRequestJson(request);
    const auto& content = body.at("messages").at(0).at("content");
    REQUIRE(content.size() == 3);
    CHECK(content.at(0).at("type") == "thinking");
    CHECK(content.at(0).at("signature") == "");  // 空签名照实,不虚构
    CHECK(content.at(1).at("type") == "redacted_thinking");
    CHECK(content.at(1).at("data") == "b3BhcXVlLXNlcmdl");
    CHECK(content.at(1) == nlohmann::json({{"data", "b3BhcXVlLXNlcmdl"}, {"type", "redacted_thinking"}}));
    CHECK(content.at(2).at("type") == "text");
}

TEST_CASE("差距6: chat/responses/gemini 三家加密思考块不出门") {
    // 不透明载荷没有可回传的形状:chat 的 reasoning 回传只吃 ThinkingBlock
    // 的正文(K2.6 跨轮保留路不掺加密块);responses 的 reasoning 与 gemini
    // 的 thought 都是一次性,照旧跳过。红线:一个字节都不许漏到 wire。
    api::Request request;
    request.model = "m";
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ThinkingBlock{"明文思考", "sig"});
    assistant.content.push_back(api::RedactedThinkingBlock{"c2VjcmV0LWRhdGEK"});
    assistant.content.push_back(api::TextBlock{"正文"});
    request.messages.push_back(assistant);

    // chat(Always 回传,K2.6 跨轮保留同款路):reasoning_content 只有明文
    // 思考,加密载荷不混进去。
    api::chat::ChatRequestOptions options;
    options.reasoning_replay = api::chat::ReasoningReplayPolicy::Always;
    const auto chat = api::chat::BuildRequestJson(request, nlohmann::json::object(), options);
    REQUIRE(chat.at("messages").at(0).contains("reasoning_content"));
    CHECK(chat.at("messages").at(0).at("reasoning_content") == "明文思考");
    CHECK(chat.dump().find("c2VjcmV0LWRhdGEK") == std::string::npos);

    // responses/gemini:思考块(明文与加密)都不回传。
    CHECK(api::responses::BuildRequestJson(request).dump().find("c2VjcmV0LWRhdGEK") == std::string::npos);
    CHECK(api::gemini::BuildRequestJson(request).dump().find("c2VjcmV0LWRhdGEK") == std::string::npos);
    // 只装思考块的 assistant 在这两家不产任何元素(明文与加密同跳过)。
    api::Request thinking_only;
    thinking_only.model = "m";
    api::Message silent;
    silent.role = api::Role::Assistant;
    silent.content.push_back(api::ThinkingBlock{"想", "sig"});
    silent.content.push_back(api::RedactedThinkingBlock{"c2VjcmV0LWRhdGEK"});
    thinking_only.messages.push_back(silent);
    CHECK(api::responses::BuildRequestJson(thinking_only).at("input").size() == 0);
    CHECK(api::gemini::BuildRequestJson(thinking_only).at("contents").size() == 0);
}

TEST_CASE("差距6: ShouldRecoverTaggedThinking 认加密思考为思考在场") {
    // assistant(redacted_thinking + tool_use) -> 末条工具结果:与明文
    // thinking 同为"这轮在思考"的证据,兼容门同开;正规流只多一道
    // <think> 探测,零影响。
    api::Request request;
    request.model = "m";
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::RedactedThinkingBlock{"b3BhcXVlLXNlcmdl"});
    assistant.content.push_back(api::ToolUseBlock{"c1", "read_file", nlohmann::json::object()});
    request.messages.push_back(assistant);
    api::Message tail;
    tail.role = api::Role::Tool;
    tail.content.push_back(api::ToolResultBlock{"c1", "结果", false});
    request.messages.push_back(tail);
    CHECK(api::anthropic::ShouldRecoverTaggedThinking(request));
}

// ---------------------------------------------------------------------------
// 差距 7(§8.2 第 7 条):请求快照 messageRef↔wire 映射——四家拍平对照
// ---------------------------------------------------------------------------

TEST_CASE("差距7: anthropic 映射逐条对位,System 顶置顶层不占位") {
    const auto map = api::anthropic::BuildMessageWireMap(FourRoleConversation());
    CHECK(map.container == "messages");
    REQUIRE(map.message_to_wire.size() == 6);  // S/U/A/T1/T2/A2
    CHECK(map.message_to_wire[0].empty());     // System 顶置顶层 system
    REQUIRE(map.message_to_wire[1].size() == 1);
    CHECK(map.message_to_wire[1][0] == 0);
    REQUIRE(map.message_to_wire[2].size() == 1);
    CHECK(map.message_to_wire[2][0] == 1);
    CHECK(map.message_to_wire[3] == std::vector<std::size_t>{2});
    CHECK(map.message_to_wire[4] == std::vector<std::size_t>{3});
    CHECK(map.message_to_wire[5] == std::vector<std::size_t>{4});
    CHECK(map.wire_element_count == 5);
}

TEST_CASE("差距7: chat 映射 system 多源共指 wire[0],正文与工具结果一裂二") {
    const auto map = api::chat::BuildMessageWireMap(FourRoleConversation());
    CHECK(map.container == "messages");
    REQUIRE(map.message_to_wire.size() == 6);
    CHECK(map.message_to_wire[0] == std::vector<std::size_t>{0});  // System -> wire[0]
    CHECK(map.message_to_wire[1] == std::vector<std::size_t>{1});
    CHECK(map.message_to_wire[2] == std::vector<std::size_t>{2});
    CHECK(map.message_to_wire[3] == std::vector<std::size_t>{3});
    CHECK(map.message_to_wire[4] == std::vector<std::size_t>{4});
    CHECK(map.message_to_wire[5] == std::vector<std::size_t>{5});
    CHECK(map.wire_element_count == 6);

    // "两个内部 messageRef 对应同一 wire message"(§八正文):Request::
    // system 与多条 System 消息在 chat 拼成一条 system 消息——映射里
    // 两条内部消息都指 wire[0];空壳 System 没出过字,如实记空。
    api::Request request;
    request.model = "m";
    request.system = "根系统提示";
    api::Message first;
    first.role = api::Role::System;
    first.content.push_back(api::TextBlock{"第一段"});
    request.messages.push_back(first);
    api::Message empty_system;
    empty_system.role = api::Role::System;  // 空正文:没出过字
    request.messages.push_back(empty_system);
    api::Message second;
    second.role = api::Role::System;
    second.content.push_back(api::TextBlock{"第二段"});
    request.messages.push_back(second);
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"问一句"});
    request.messages.push_back(user);

    const auto merged = api::chat::BuildMessageWireMap(request);
    REQUIRE(merged.message_to_wire.size() == 4);
    CHECK(merged.message_to_wire[0] == std::vector<std::size_t>{0});
    CHECK(merged.message_to_wire[1].empty());
    CHECK(merged.message_to_wire[2] == std::vector<std::size_t>{0});
    CHECK(merged.message_to_wire[3] == std::vector<std::size_t>{1});
    CHECK(merged.wire_element_count == 2);

    // 混装一条(正文 + 工具结果)裂成两条 wire 消息:user 在前 tool 在后。
    api::Request mixed_request;
    mixed_request.model = "m";
    api::Message mixed;
    mixed.role = api::Role::Tool;
    mixed.content.push_back(api::TextBlock{"附言"});
    mixed.content.push_back(api::ToolResultBlock{"call_x", "结果", false});
    mixed_request.messages.push_back(mixed);
    const auto mixed_map = api::chat::BuildMessageWireMap(mixed_request);
    REQUIRE(mixed_map.message_to_wire.size() == 1);
    CHECK(mixed_map.message_to_wire[0] == (std::vector<std::size_t>{0, 1}));
    CHECK(mixed_map.wire_element_count == 2);
}

TEST_CASE("差距7: responses/gemini 映射逐块裂元素,思考跳过后的空档如实") {
    // S/U/A(thinking+text+call×2)/T1/T2/A2:responses 出 7 个 item,
    // assistant 一条裂三个(正文 message + 两枚 function_call),thinking
    // 跳过不占位;gemini 同数,assistant 正文一条 + functionCall 两条。
    const auto responses = api::responses::BuildMessageWireMap(FourRoleConversation());
    CHECK(responses.container == "input");
    REQUIRE(responses.message_to_wire.size() == 6);
    CHECK(responses.message_to_wire[0].empty());
    CHECK(responses.message_to_wire[1] == std::vector<std::size_t>{0});
    CHECK(responses.message_to_wire[2] == (std::vector<std::size_t>{1, 2, 3}));
    CHECK(responses.message_to_wire[3] == std::vector<std::size_t>{4});
    CHECK(responses.message_to_wire[4] == std::vector<std::size_t>{5});
    CHECK(responses.message_to_wire[5] == std::vector<std::size_t>{6});
    CHECK(responses.wire_element_count == 7);

    const auto gemini = api::gemini::BuildMessageWireMap(FourRoleConversation());
    CHECK(gemini.container == "contents");
    REQUIRE(gemini.message_to_wire.size() == 6);
    CHECK(gemini.message_to_wire[0].empty());
    CHECK(gemini.message_to_wire[1] == std::vector<std::size_t>{0});
    CHECK(gemini.message_to_wire[2] == (std::vector<std::size_t>{1, 2, 3}));
    CHECK(gemini.message_to_wire[3] == std::vector<std::size_t>{4});
    CHECK(gemini.message_to_wire[4] == std::vector<std::size_t>{5});
    CHECK(gemini.message_to_wire[5] == std::vector<std::size_t>{6});
    CHECK(gemini.wire_element_count == 7);

    // 只剩思考块的消息(明文 + 加密都被跳过)在两家都不产元素:对照
    // 记空,计数不涨。
    api::Request thinking_only;
    thinking_only.model = "m";
    api::Message silent;
    silent.role = api::Role::Assistant;
    silent.content.push_back(api::ThinkingBlock{"想", "sig"});
    silent.content.push_back(api::RedactedThinkingBlock{"c2VjcmV0LWRhdGEK"});
    thinking_only.messages.push_back(silent);
    const auto silent_responses = api::responses::BuildMessageWireMap(thinking_only);
    REQUIRE(silent_responses.message_to_wire.size() == 1);
    CHECK(silent_responses.message_to_wire[0].empty());
    CHECK(silent_responses.wire_element_count == 0);
    const auto silent_gemini = api::gemini::BuildMessageWireMap(thinking_only);
    REQUIRE(silent_gemini.message_to_wire.size() == 1);
    CHECK(silent_gemini.message_to_wire[0].empty());
    CHECK(silent_gemini.wire_element_count == 0);
}

TEST_CASE("差距7: 横切钉子——四家映射的元素计数与实际出口容器长度恒等") {
    // 表是拍平的只读影子:wire_element_count 对不上 BuildRequestJson 实际
    // 产出的容器长度,就是拍平和表劈了。数量映射(5/6/7/7)与合同册
    // 横切节同一副牌。
    const api::Request request = FourRoleConversation();
    const auto anthropic_map = api::anthropic::BuildMessageWireMap(request);
    CHECK(anthropic_map.wire_element_count == api::anthropic::BuildRequestJson(request).at("messages").size());
    const auto chat_map = api::chat::BuildMessageWireMap(request);
    CHECK(chat_map.wire_element_count == api::chat::BuildRequestJson(request).at("messages").size());
    const auto responses_map = api::responses::BuildMessageWireMap(request);
    CHECK(responses_map.wire_element_count == api::responses::BuildRequestJson(request).at("input").size());
    const auto gemini_map = api::gemini::BuildMessageWireMap(request);
    CHECK(gemini_map.wire_element_count == api::gemini::BuildRequestJson(request).at("contents").size());

    // 全部下标落在容器界内(验尸侧按表取元素不越界)。
    const auto in_bounds = [](const api::WireMessageMap& map) {
        for (const auto& indices : map.message_to_wire) {
            for (const std::size_t index : indices) {
                if (index >= map.wire_element_count) {
                    return false;
                }
            }
        }
        return true;
    };
    CHECK(in_bounds(anthropic_map));
    CHECK(in_bounds(chat_map));
    CHECK(in_bounds(responses_map));
    CHECK(in_bounds(gemini_map));
}

// ---------------------------------------------------------------------------
// 差距 8(§8.2 第 8 条):容量检查吃 extra_body 覆盖后的形状——取数口与
// 四家 backend 的有效上限/写侧覆盖
// ---------------------------------------------------------------------------

TEST_CASE("差距8: IntKeyFromExtraBody 覆盖序——provider 先、请求级后,非整数不算") {
    const nlohmann::json none;
    const nlohmann::json provider = nlohmann::json{{"max_tokens", 4096}};
    const nlohmann::json request_level = nlohmann::json{{"max_tokens", 8192}};
    const nlohmann::json non_integer = nlohmann::json{{"max_tokens", "many"}};

    CHECK_FALSE(api::IntKeyFromExtraBody(none, none, "max_tokens").has_value());
    CHECK(api::IntKeyFromExtraBody(provider, none, "max_tokens") == 4096);
    CHECK(api::IntKeyFromExtraBody(none, request_level, "max_tokens") == 8192);
    CHECK(api::IntKeyFromExtraBody(provider, request_level, "max_tokens") == 8192);  // 请求级压 provider
    CHECK_FALSE(api::IntKeyFromExtraBody(non_integer, none, "max_tokens").has_value());
    CHECK(api::IntKeyFromExtraBody(provider, nlohmann::json{{"别的键", 1}}, "max_tokens") == 4096);
}

TEST_CASE("差距8: 四家 backend 有效上限——anthropic 必填兜底,键名各认各的") {
    api::Request request;
    request.model = "m";

    // anthropic:max_tokens 必填——无覆盖时落公开兜底,不会是 nullopt。
    const api::anthropic::AnthropicBackend anthropic_plain("https://a", "t");
    const auto plain = anthropic_plain.GetEffectiveOutputLimit(request);
    CHECK(plain.tokens == api::kRequiredMaxOutputTokensFallback);
    CHECK_FALSE(plain.overridden);
    request.max_tokens = 1024;
    const auto declared = anthropic_plain.GetEffectiveOutputLimit(request);
    CHECK(declared.tokens == 1024);
    CHECK_FALSE(declared.overridden);

    const api::anthropic::AnthropicBackend anthropic_override(
        "https://a", "t", 1000, 30, false,
        nlohmann::json{{"max_tokens", 12345}});
    const auto overridden = anthropic_override.GetEffectiveOutputLimit(request);
    REQUIRE(overridden.tokens.has_value());
    CHECK(*overridden.tokens == 12345);  // 覆盖压过 request.max_tokens=1024
    CHECK(overridden.overridden);

    // chat:键名 max_tokens,unset 如实 nullopt。
    const api::chat::ChatCompletionsBackend chat_plain("https://c", "t");
    api::Request unset_request;
    unset_request.model = "m";
    const auto chat_unset = chat_plain.GetEffectiveOutputLimit(unset_request);
    CHECK_FALSE(chat_unset.tokens.has_value());
    CHECK_FALSE(chat_unset.overridden);
    const api::chat::ChatCompletionsBackend chat_override(
        "https://c", "t", 1000, 30,
        nlohmann::json{{"max_tokens", 777}});
    const auto chat_overridden = chat_override.GetEffectiveOutputLimit(unset_request);
    REQUIRE(chat_overridden.tokens.has_value());
    CHECK(*chat_overridden.tokens == 777);
    CHECK(chat_overridden.overridden);

    // responses:键名 max_output_tokens(不是 max_tokens)。
    const api::responses::ResponsesBackend responses_override(
        "https://r", "t", 1000, 30, false,
        nlohmann::json{{"max_tokens", 555}});
    const auto responses_wrong_key = responses_override.GetEffectiveOutputLimit(unset_request);
    CHECK_FALSE(responses_wrong_key.overridden);  // 键名不对不算覆盖
    const api::responses::ResponsesBackend responses_right(
        "https://r", "t", 1000, 30, false,
        nlohmann::json{{"max_output_tokens", 999}});
    const auto responses_overridden = responses_right.GetEffectiveOutputLimit(unset_request);
    REQUIRE(responses_overridden.tokens.has_value());
    CHECK(*responses_overridden.tokens == 999);
    CHECK(responses_overridden.overridden);

    // gemini:键深一层(generationConfig.maxOutputTokens)。
    const api::gemini::GeminiBackend gemini_plain("https://g", "t");
    CHECK_FALSE(gemini_plain.GetEffectiveOutputLimit(unset_request).overridden);
    const api::gemini::GeminiBackend gemini_override(
        "https://g", "t", 1000, 30,
        nlohmann::json{{"generationConfig", nlohmann::json{{"maxOutputTokens", 666}}}});
    const auto gemini_overridden = gemini_override.GetEffectiveOutputLimit(unset_request);
    REQUIRE(gemini_overridden.tokens.has_value());
    CHECK(*gemini_overridden.tokens == 666);
    CHECK(gemini_overridden.overridden);
}

TEST_CASE("差距8: ForceMaxOutputTokensOverride 只在有覆盖时落笔,不无中生有") {
    // 写侧(应急/降级收窄穿透覆盖):extra_body 写过键,窄值落请求级
    // 覆盖位(合并序最后,压过 provider 级);没写过键的请求一个键不造,
    // 出口形状与从前逐字节一致。
    api::Request request;
    request.model = "m";
    request.max_tokens = 2048;

    const api::anthropic::AnthropicBackend plain("https://a", "t");
    plain.ForceMaxOutputTokensOverride(request, 1024);
    CHECK_FALSE(request.extra_body.contains("max_tokens"));  // 没覆盖不造键

    const api::anthropic::AnthropicBackend overridden(
        "https://a", "t", 1000, 30, false,
        nlohmann::json{{"max_tokens", 12345}});
    overridden.ForceMaxOutputTokensOverride(request, 1024);
    REQUIRE(request.extra_body.contains("max_tokens"));
    CHECK(request.extra_body.at("max_tokens") == 1024);
    // 请求级原有覆盖同理被窄值压过。
    api::Request request_level;
    request_level.model = "m";
    request_level.extra_body = nlohmann::json{{"max_tokens", 12345}};
    plain.ForceMaxOutputTokensOverride(request_level, 512);
    REQUIRE(request_level.extra_body.contains("max_tokens"));
    CHECK(request_level.extra_body.at("max_tokens") == 512);

    // gemini 的写侧走深一层的 generationConfig.maxOutputTokens。
    const api::gemini::GeminiBackend gemini_plain("https://g", "t");
    api::Request gemini_request;
    gemini_request.model = "m";
    gemini_plain.ForceMaxOutputTokensOverride(gemini_request, 256);
    CHECK_FALSE(gemini_request.extra_body.contains("generationConfig"));
    const api::gemini::GeminiBackend gemini_override(
        "https://g", "t", 1000, 30,
        nlohmann::json{{"generationConfig", nlohmann::json{{"maxOutputTokens", 666}}}});
    gemini_override.ForceMaxOutputTokensOverride(gemini_request, 256);
    REQUIRE(gemini_request.extra_body.contains("generationConfig"));
    CHECK(gemini_request.extra_body.at("generationConfig").at("maxOutputTokens") == 256);
}
