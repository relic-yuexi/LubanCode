// 前缀缓存守恒单(§四验收):宿主目录通知是"追加",不动已发送前缀。
//
// 断点背景:worktree enter/exit 改 cwd 后重拼 system,目录一换 system 指纹
// 变,同一份已发送消息前缀整段作废(实测命中率 86%→30%)。修法:目录更新
// 走宿主追加通知(user 消息),system 环境段冻结为"会话启动目录"。本册用
// 真实 anthropic wire 适配器(api::anthropic::BuildRequestJson)直接比较切换
// 前后的请求快照与 wire 编码——不只断言 epoch 数字:
//   1. 空闲路(slash 切换):通知注入后下一请求 system/tools 逐字节不变,
//      旧消息逐条不变(wire 元素序逐条相等),新内容只在尾部。
//   2. 工具路(turn 内 enter/exit):通知经 AgentWiring.inbox 在工具结果
//      提交后的合法消息边界注入——tool_use/tool_result 配对不拆,通知块
//      追加在结果之后,system/tools 不变。
//   3. 病灶对照:旧路 SetSystemPrompt 重拼目录,wire 前缀当场断——两相对
//      照,断的是行为不是数字。
#include <doctest/doctest.h>

#include <atomic>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/anthropic/client.hpp"  // 真实 wire 适配器
#include "api/types.hpp"
#include "runtime/trajectory_session.hpp"  // FormatHostDirectoryNoticeText
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;

    std::string SerializeForDiagnostics(const api::Request& request) const override {
        return api::anthropic::BuildRequestJson(request).dump();
    }

    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured_requests.push_back(request);
        const std::size_t idx = captured_requests.size() - 1;
        if (idx >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        for (const auto& event : scripts[idx]) {
            on_event(event);
        }
        return {};
    }
};

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& tool_name) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

class FakeTool : public tools::Tool {
public:
    std::string name() const override { return "fake"; }
    std::string description() const override { return "fake tool for test"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::Tool::Result execute(const nlohmann::json&) override { return {"done", false}; }
};

nlohmann::json WireJson(const api::Request& request) {
    return api::anthropic::BuildRequestJson(request);
}

api::Message HostNoticeMessage() {
    api::Message notice;
    notice.role = api::Role::User;
    notice.content.push_back(api::TextBlock{lubancode::runtime::FormatHostDirectoryNoticeText(
        "D:/repo", "D:/repo/.lubancode/worktrees/x", "user /worktree new")});
    return notice;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 空闲路:slash 切换 → 宿主通知注入 → 下一请求追加律成立
// ---------------------------------------------------------------------------
TEST_CASE("宿主目录通知(空闲路): system/tools 逐字节不变,旧消息逐条不变") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("在的"), TextOnlyScript("继续干")};
    tools::ToolRegistry registry;

    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system 基线"});

    const int epoch_before_turn1 = loop.cache_epoch();
    CHECK(loop.Run("你好", agent::TurnWiring{}).has_value());
    REQUIRE(backend.captured_requests.size() == 1);
    // 拷贝存值:vector 后续 push 会扩容,引用会悬空。
    const api::Request request1 = backend.captured_requests[0];
    const nlohmann::json j1 = WireJson(request1);

    // slash 切换:宿主在空闲边界注入目录通知(与控制器
    // DeliverPendingDirectoryNoticeNow 同一只 InjectIncoming 口)。
    loop.context().InjectIncoming(HostNoticeMessage());

    CHECK(loop.Run("继续", agent::TurnWiring{}).has_value());
    REQUIRE(backend.captured_requests.size() == 2);
    const api::Request request2 = backend.captured_requests[1];
    const nlohmann::json j2 = WireJson(request2);

    // (a) 请求快照:system 一字不动;tools 表不动。
    CHECK(request2.system == request1.system);
    CHECK(request2.tools.size() == request1.tools.size());
    // 旧消息逐条保留,新内容只在尾部:首份请求只带首条 user(assistant
    // 是响应,不进请求),第二份在其后追加 assistant1 + 通知 + 新输入。
    REQUIRE(request2.messages.size() == request1.messages.size() + 3);

    // (b) wire 编码:system/tools 顶层元素相等;messages 数组前 N 条逐元素
    //     相等(这是协议适配器真正发出去的形状)。
    CHECK(j2.at("system") == j1.at("system"));
    if (j1.contains("tools")) {
        CHECK(j2.at("tools") == j1.at("tools"));
    }
    const auto& msgs1 = j1.at("messages");
    const auto& msgs2 = j2.at("messages");
    REQUIRE(msgs1.size() == 1);  // 首份请求:只有首条 user
    REQUIRE(msgs2.size() == 4);  // user + assistant + 通知 + 新输入
    for (std::size_t i = 0; i < msgs1.size(); ++i) {
        CHECK(msgs2[i] == msgs1[i]);
    }
    // 通知消息带来源标识与新目录;新用户输入垫底。
    const std::string notice_text = msgs2[2].dump();
    CHECK(notice_text.find("[宿主通知]") != std::string::npos);
    CHECK(notice_text.find("worktrees/x") != std::string::npos);
    CHECK(msgs2[3].dump().find("继续") != std::string::npos);

    // (c) epoch 只是佐证:追加注入不涨本地前缀 epoch。
    CHECK(loop.cache_epoch() == epoch_before_turn1);
}

// ---------------------------------------------------------------------------
// 2. 工具路:turn 内 enter/exit → inbox 边界注入,配对不拆
// ---------------------------------------------------------------------------
TEST_CASE("宿主目录通知(工具路): 工具结果提交后追加,tool_use/tool_result 配对不拆") {
    FakeBackend backend;
    // step0:模型要工具(worktree enter 的形状);step1:收口。
    backend.scripts = {ToolUseScript("t1", "fake"), TextOnlyScript("进房了,继续")};
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>());

    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system 基线"});

    // 工具路的挂账交付:AgentWiring.inbox 在工具结果收口后的循环顶轮询
    //(与控制器 TakePendingDirectoryNoticeAsMessage 同款——只回一次)。
    bool notice_pending = true;
    agent::AgentWiring wiring;
    wiring.inbox = [&notice_pending]() -> std::optional<api::Message> {
        if (!notice_pending) {
            return std::nullopt;
        }
        notice_pending = false;
        return HostNoticeMessage();
    };
    loop.SetWiring(std::move(wiring));

    const int epoch_before = loop.cache_epoch();
    CHECK(loop.Run("帮我在隔离房里干活", agent::TurnWiring{}).has_value());
    REQUIRE(backend.captured_requests.size() == 2);
    const nlohmann::json j1 = WireJson(backend.captured_requests[0]);
    const nlohmann::json j2 = WireJson(backend.captured_requests[1]);

    // system/tools 不变。
    CHECK(j2.at("system") == j1.at("system"));
    if (j1.contains("tools")) {
        CHECK(j2.at("tools") == j1.at("tools"));
    }

    // 第二请求(工具结果提交后的下一请求):[user, assistant(tool_use),
    // user(tool_result + 通知文本块)]——收口的 assistant 是本步响应,不进
    // 请求;通知块追加在 tool_result 之后,没插进未闭合的调用组。
    const auto& msgs2 = j2.at("messages");
    REQUIRE(msgs2.size() == 3);
    const auto& result_msg = msgs2[2].at("content");
    bool has_tool_result = false;
    bool has_notice_text = false;
    for (const auto& block : result_msg) {
        if (block.value("type", std::string()) == "tool_result") {
            has_tool_result = true;
            CHECK(block.at("tool_use_id").get<std::string>() == "t1");  // 配对没拆
        }
        if (block.value("type", std::string()) == "text" &&
            block.at("text").get<std::string>().find("[宿主通知]") != std::string::npos) {
            has_notice_text = true;
        }
    }
    CHECK(has_tool_result);
    CHECK(has_notice_text);  // 通知追加在结果之后,没插进未闭合的调用组

    // 追加律:本地前缀 epoch 不涨(同轮第二请求对第一请求是纯追加)。
    CHECK(loop.cache_epoch() == epoch_before);
}

// ---------------------------------------------------------------------------
// 3. 病灶对照:重拼 system(旧路)当场改掉 wire 的 system 字段——服务端
//    前缀缓存按内容块序列(system 在 messages 之前)判命中,system 一变
//    整段前缀作废;追加通知(案 1)system 逐字节不变,只有尾部追加。
// ---------------------------------------------------------------------------
TEST_CASE("病灶对照: SetSystemPrompt 重拼目录即改 wire system 字段") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("在的"), TextOnlyScript("继续")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system 基线"});
    CHECK(loop.Run("你好", agent::TurnWiring{}).has_value());

    // 旧路:切房重拼 system(目录行变化)。
    loop.SetSystemPrompt("system 基线(工作目录已换)");
    CHECK(loop.Run("继续", agent::TurnWiring{}).has_value());

    const nlohmann::json j1 = WireJson(backend.captured_requests[0]);
    const nlohmann::json j2 = WireJson(backend.captured_requests[1]);
    // system 字段被改写:这就是实测 86%→30% 那场病理的根因形状——服务端
    // 按 system+messages 的内容块序列判缓存前缀,system 一变全部旧请求
    // 前缀作废,不是命中率数字的游戏。
    CHECK_FALSE(j2.at("system") == j1.at("system"));
}
