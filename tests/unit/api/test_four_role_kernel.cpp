// 四角色内核第一棒(轨迹 v3·消息主轴,单子 §1.1/§4.46)。
//
// 换骨不换皮:内部消息模型升四角色(system/user/assistant/tool 一等
// 公民),wire 出口形状一字不变。本册钉三件事:
//
//   1. 内核件——api::Role 四枚枚举与 RoleToString 共用件的拍平语义;
//   2. Anthropic 换骨——System 消息顶置顶层 system、Tool 角色折 user
//      容器 tool_result 块、相邻不合并、ShouldRecoverTaggedThinking 认
//      tool 角色(旧式 User+ToolResultBlock 共存同认);
//   3. 总钉子——同一份对话用"两角色旧路"(Request::system + User 容器
//      ToolResultBlock)与"四角色新路"(System 消息 + Tool 角色)分别
//      组装,Anthropic 出口 JSON 逐字节相等。这就是"内里换骨、外观不变"
//      的直接证据;红了说明换骨漏了或拍平变了。
//
// 四家合同基线在 test_wire_role_contract.cpp(18 案,断言不动);本册只
// 钉四角色新增面,不重复四家横切对照。chat/responses/gemini 三家本棒未
// 换骨(仍走旧路),对 Tool/System 角色的行为不在此钉——钉了就是把未实现
// 形状写死,下一棒逐家换时自己立册。
//
// 断言纪律:json 缺键一律 contains() 判,禁止 const json 上 operator[]
// 查缺键(nlohmann UB)。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "api/anthropic/client.hpp"
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
