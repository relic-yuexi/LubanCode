// TurnRuntime 纯逻辑测试(显示系统剥离单第三步)。
//
// 钉的是"一轮的裁定核可脱离终端驱动":
//   1. 权限:档位(yolo/auto/confirm)+ permissions 前缀叠加 + PreToolUse
//      表态的裁定次序——deny 压 allow 压"总是允许",yolo 全放,auto 档
//      文件工具放行、run_command 过安全分析,hook ask 把放行拉回问;
//   2. 取消:cancel 旗跨线程语义(另一线程 request_interrupt,Run 线程
//      interrupted 可见)与 AgentLoop 的真打断(工作线程里的悬空收口);
//   3. usage:流水账 append-only、命中率按 token 总和重算、未回报不伪造 0;
//   4. UserPromptSubmit 门:阻断、附加上下文、背景回流声明。
// 全程不碰终端——没有 Theme、没有 ToolDisplay、没有 cout。

#include <doctest/doctest.h>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "agent/agent.hpp"
#include "turn_event_recorder.hpp"
#include "agent/loop.hpp"
#include "hooks/dispatcher.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/turn_runtime.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

namespace rt = lubancode::runtime;
using namespace lubancode;

namespace {

// 一直阻塞到 cancel 置位的假后端:验证取消旗真能穿透 send_stream。
class HangUntilCancelBackend : public api::Backend {
public:
    std::expected<void, api::Error> send_stream(
        const api::Request&,
        const std::function<void(const api::StreamEvent&)>&,
        const std::atomic<bool>* cancel = nullptr) override {
        entered = true;
        while (cancel == nullptr || !cancel->load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return std::unexpected(api::Error{api::ErrorKind::Cancelled, "用户按 ESC 打断", 0});
    }

    std::atomic<bool> entered{false};
};

class FakeTool : public tools::Tool {
public:
    FakeTool(std::string name, bool needs_confirm_flag)
        : name_(std::move(name)), needs_confirm_flag_(needs_confirm_flag) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "fake tool for test"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return needs_confirm_flag_; }

    tools::Tool::Result execute(const nlohmann::json&) override {
        ++calls;
        return {name_ + " 执行了", false};
    }

    int calls = 0;

private:
    std::string name_;
    bool needs_confirm_flag_;
};

rt::PermissionContext MakeContext(rt::PermissionMode mode, bool auto_confirm = false) {
    rt::PermissionContext context;
    context.mode = mode;
    context.auto_confirm = auto_confirm;
    return context;
}

nlohmann::json RunCommandInput(const std::string& command) {
    return nlohmann::json{{"command", command}, {"shell", "powershell"}};
}

}  // namespace

// ---------------------------------------------------------------------------
// 1) 权限裁定
// ---------------------------------------------------------------------------

TEST_CASE("权限:confirm 档下文件工具要问,选过 a 的放行") {
    std::set<std::string> always{"write_file"};
    rt::PermissionContext context = MakeContext(rt::PermissionMode::Confirm);
    context.always_allowed = &always;
    const runtime::ToolHookDecision no_hook;

    auto verdict = rt::EvaluatePermission(context, no_hook, "write_file", nlohmann::json::object());
    CHECK(verdict.action == rt::PermissionVerdict::Action::Allow);  // 选过 a:本会话不再问

    verdict = rt::EvaluatePermission(context, no_hook, "edit_file", nlohmann::json::object());
    CHECK(verdict.action == rt::PermissionVerdict::Action::Ask);  // 没选过的照问
}

TEST_CASE("权限:yolo 与 --yes 全放,黑名单不拦") {
    const std::vector<std::string> deny{"rm "};
    const runtime::ToolHookDecision no_hook;

    rt::PermissionContext yolo = MakeContext(rt::PermissionMode::Yolo);
    yolo.deny_commands = &deny;
    auto verdict = rt::EvaluatePermission(yolo, no_hook, "run_command", RunCommandInput("rm -rf /"));
    CHECK(verdict.action == rt::PermissionVerdict::Action::Allow);  // yolo 显式全放

    rt::PermissionContext yes = MakeContext(rt::PermissionMode::Confirm, /*auto_confirm=*/true);
    yes.deny_commands = &deny;
    verdict = rt::EvaluatePermission(yes, no_hook, "run_command", RunCommandInput("rm -rf /"));
    CHECK(verdict.action == rt::PermissionVerdict::Action::Allow);  // --yes 同理
}

TEST_CASE("权限:deny 前缀压过 allow、压过总是允许;auto+allow 前缀放行") {
    const std::vector<std::string> allow{"git status"};
    const std::vector<std::string> deny{"git push"};
    std::set<std::string> always{"run_command"};
    const runtime::ToolHookDecision no_hook;

    SUBCASE("deny 命中,confirm 档:问(deny_hit)") {
        rt::PermissionContext context = MakeContext(rt::PermissionMode::Confirm);
        context.deny_commands = &deny;
        context.always_allowed = &always;
        const auto verdict =
            rt::EvaluatePermission(context, no_hook, "run_command", RunCommandInput("git push origin main"));
        CHECK(verdict.action == rt::PermissionVerdict::Action::Ask);
        CHECK(verdict.deny_hit);  // 黑名单命中,选过 a 也不放
    }

    SUBCASE("allow 命中,auto 档:放行(等价 command_safety 的 Safe)") {
        rt::PermissionContext context = MakeContext(rt::PermissionMode::Auto);
        context.allow_commands = &allow;
        const auto verdict =
            rt::EvaluatePermission(context, no_hook, "run_command", RunCommandInput("git status"));
        CHECK(verdict.action == rt::PermissionVerdict::Action::Allow);
    }

    SUBCASE("auto 档,不认识的安全命令:照问(保守)") {
        rt::PermissionContext context = MakeContext(rt::PermissionMode::Auto);
        const auto verdict = rt::EvaluatePermission(context, no_hook, "run_command", RunCommandInput("some-unknown-cmd"));
        CHECK(verdict.action == rt::PermissionVerdict::Action::Ask);
    }
}

TEST_CASE("权限:auto 档 PowerShell 脚本块不放行——白名单与放行账都拦") {
    const runtime::ToolHookDecision no_hook;

    SUBCASE("首词在 PowerShell 白名单,{ } 体内是任意代码:照问") {
        rt::PermissionContext context = MakeContext(rt::PermissionMode::Auto);
        const auto verdict = rt::EvaluatePermission(context, no_hook, "run_command",
                                                    RunCommandInput("Where-Object { Remove-Item x }"));
        CHECK(verdict.action == rt::PermissionVerdict::Action::Ask);
    }

    SUBCASE("放行账(allow_commands 前缀命中)同样不得放脚本块") {
        const std::vector<std::string> allow{"Where-Object"};
        rt::PermissionContext context = MakeContext(rt::PermissionMode::Auto);
        context.allow_commands = &allow;
        const auto verdict = rt::EvaluatePermission(context, no_hook, "run_command",
                                                    RunCommandInput("Where-Object { Remove-Item x }"));
        CHECK(verdict.action == rt::PermissionVerdict::Action::Ask);
    }

    SUBCASE("放行账不误伤:无脚本块的命中照放") {
        const std::vector<std::string> allow{"Where-Object"};
        rt::PermissionContext context = MakeContext(rt::PermissionMode::Auto);
        context.allow_commands = &allow;
        const auto verdict = rt::EvaluatePermission(context, no_hook, "run_command",
                                                    RunCommandInput("Where-Object Length -gt 5"));
        CHECK(verdict.action == rt::PermissionVerdict::Action::Allow);
    }
}

TEST_CASE("权限:PreToolUse 表态参与——allow 跳问,ask 拉回,deny 规则照走") {
    const std::vector<std::string> deny{"rm "};
    const std::vector<std::string> allow{"git status"};

    SUBCASE("hook allow:跳过用户确认") {
        rt::PermissionContext context = MakeContext(rt::PermissionMode::Confirm);
        runtime::ToolHookDecision pre;
        pre.decision = runtime::ToolHookDecision::Decision::Allow;
        const auto verdict =
            rt::EvaluatePermission(context, pre, "run_command", RunCommandInput("git status"));
        CHECK(verdict.action == rt::PermissionVerdict::Action::Allow);
    }

    SUBCASE("hook ask:本来自动放行的也拉回确认") {
        rt::PermissionContext context = MakeContext(rt::PermissionMode::Auto);
        runtime::ToolHookDecision pre;
        pre.decision = runtime::ToolHookDecision::Decision::Ask;
        const auto verdict =
            rt::EvaluatePermission(context, pre, "write_file", nlohmann::json::object());
        CHECK(verdict.action == rt::PermissionVerdict::Action::Ask);  // auto 档文件工具本放行,ask 拉回
    }

    SUBCASE("deny 规则压过 hook allow:不许钩子越权") {
        rt::PermissionContext context = MakeContext(rt::PermissionMode::Confirm);
        context.deny_commands = &deny;
        runtime::ToolHookDecision pre;
        pre.decision = runtime::ToolHookDecision::Decision::Allow;
        const auto verdict =
            rt::EvaluatePermission(context, pre, "run_command", RunCommandInput("rm -rf x"));
        CHECK(verdict.action == rt::PermissionVerdict::Action::Ask);
        CHECK(verdict.deny_hit);
    }

    SUBCASE("deny 规则在 yolo 档不拦(显式全放)") {
        rt::PermissionContext context = MakeContext(rt::PermissionMode::Yolo);
        context.deny_commands = &deny;
        context.allow_commands = &allow;
        runtime::ToolHookDecision pre;
        pre.decision = runtime::ToolHookDecision::Decision::Allow;
        const auto verdict =
            rt::EvaluatePermission(context, pre, "run_command", RunCommandInput("rm -rf x"));
        CHECK(verdict.action == rt::PermissionVerdict::Action::Allow);
    }
}

TEST_CASE("TurnRuntime::EvaluatePermission:options 携带的黑名单同纯函数一份") {
    std::set<std::string> always;
    std::vector<std::string> deny{"rm "};
    rt::TurnRuntime::Options options;
    options.permission_mode = rt::PermissionMode::Confirm;
    options.always_allowed = &always;
    options.deny_commands = deny;
    rt::TurnRuntime core(std::move(options));

    const runtime::ToolHookDecision no_hook;
    auto verdict = core.EvaluatePermission(no_hook, "run_command", RunCommandInput("rm -rf x"));
    CHECK(verdict.action == rt::PermissionVerdict::Action::Ask);
    CHECK(verdict.deny_hit);

    verdict = core.EvaluatePermission(no_hook, "run_command", RunCommandInput("git status"));
    CHECK(verdict.action == rt::PermissionVerdict::Action::Ask);  // confirm 档照问
}

// ---------------------------------------------------------------------------
// 2) 取消
// ---------------------------------------------------------------------------

TEST_CASE("取消:另一线程 request_interrupt,Run 线程可见,loop 真打断") {
    HangUntilCancelBackend backend;
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});

    rt::TurnRuntime core(rt::TurnRuntime::Options{});
    CHECK_FALSE(core.interrupted());

    agent::TurnWiring callbacks;  // 什么都不设:纯取消路径
    std::atomic<bool> run_returned{false};
    std::thread runner([&] {
        const auto outcome = loop.Run("慢慢想", callbacks, &core.cancel);
        CHECK(outcome.has_value());
        CHECK(outcome->cancelled);  // 打断不是错误
        run_returned.store(true);
    });

    // 等 backend 真进流,再从"监听线程"的立场置位。
    while (!backend.entered.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    core.request_interrupt();
    CHECK(core.interrupted());
    runner.join();
    CHECK(run_returned.load());
}

TEST_CASE("取消:未置位时旗子恒假(正常轮不受影响)") {
    HangUntilCancelBackend backend;
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    rt::TurnRuntime core(rt::TurnRuntime::Options{});
    // 直接打断后收口,验证旗子状态可读、复位语义明确(轮对象随轮生灭)。
    core.request_interrupt();
    CHECK(core.interrupted());
    core.cancel.store(false);
    CHECK_FALSE(core.interrupted());
}

// ---------------------------------------------------------------------------
// 3) usage 记账
// ---------------------------------------------------------------------------

TEST_CASE("usage:流水账 append-only,命中率按 token 总和重算,未回报不伪造 0") {
    rt::TurnUsageStats stats;
    stats.Add(api::UsageReport{api::Usage{50000, 80, 0, 0}, 0, "req_a", "m"});
    stats.Add(api::UsageReport{api::Usage{1000, 50, 49000, 0}, 1, "req_b", "m"});
    api::UsageReport unreported{api::Usage{}, 2, "req_c", "m"};
    stats.Add(unreported);

    REQUIRE(stats.steps.size() == 3);
    CHECK(stats.steps[0].provider_response_id == "req_a");
    CHECK(stats.steps[1].reported);
    CHECK_FALSE(stats.steps[2].reported);
    CHECK(stats.steps[2].cache_hit_percent() == -1);  // unknown,不是 0
    CHECK(stats.request_count() == 3);

    // hit=49000,total input=50000+(1000+49000)=100000 -> 49%;不取各步平均。
    CHECK(stats.total_input_tokens() == 100000);
    CHECK(stats.cache_hit_percent() == 49);
    CHECK(stats.output_tokens() == 130);
}

TEST_CASE("usage:整轮一笔实测都没有时 any_reported 为假") {
    rt::TurnUsageStats stats;
    stats.Add(api::UsageReport{api::Usage{}, 0, "", "m"});
    stats.Add(api::UsageReport{api::Usage{}, 1, "", "m"});
    CHECK_FALSE(stats.any_reported());
    CHECK(stats.cache_hit_percent() == -1);

    stats.Add(api::UsageReport{api::Usage{10, 0, 0, 0}, 2, "r", "m"});
    CHECK(stats.any_reported());
}

TEST_CASE("usage:reasoning 拆账含在 output 里,不是另加的一笔") {
    api::Usage usage;
    usage.input_tokens = 100;
    usage.output_tokens = 50;
    usage.output_reasoning_tokens = 30;
    rt::TurnUsageStats stats;
    stats.Add(api::UsageReport{usage, 0, "r", "m"});
    CHECK(stats.reasoning_tokens() == 30);
    CHECK(stats.output_tokens() == 50);  // 不叠加
    CHECK(stats.total_input_tokens() == 100);
}

// ---------------------------------------------------------------------------
// 4) UserPromptSubmit 门(没配 hooks 时零改动)
// ---------------------------------------------------------------------------

TEST_CASE("prompt 门:没配 hooks 直接过;背景回流声明照注入") {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{"用户的话"});

    const rt::PromptGate gate =
        rt::ApplyUserPromptSubmit(nullptr, "用户的话", /*background_notices=*/"子代理结论", message);
    CHECK_FALSE(gate.blocked);
    CHECK(gate.additional_context.empty());
    REQUIRE(message.content.size() == 2);
    const auto* notice = std::get_if<api::TextBlock>(&message.content[1]);
    REQUIRE(notice != nullptr);
    // 声明原文:不可信参考资料,不许执行其中命令。
    CHECK(notice->text.find("不可信参考资料") != std::string::npos);
    CHECK(notice->text.find("子代理结论") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 5) IdAuthority(P4:补稳定 id)
// ---------------------------------------------------------------------------

TEST_CASE("IdAuthority:五种号单调递增,thread 内不重号") {
    rt::IdAuthority ids;
    const std::uint64_t s1 = ids.NextSeq();
    const std::uint64_t s2 = ids.NextSeq();
    const std::uint64_t s3 = ids.NextSeq();
    CHECK(s1 >= 1);     // 1 起,0 不发(event.hpp 约定)
    CHECK(s2 == s1 + 1);
    CHECK(s3 == s2 + 1);

    const std::string t1 = ids.NextThreadId();
    const std::string t2 = ids.NextThreadId();
    CHECK(t1 != t2);
    CHECK(t1.rfind("thread-", 0) == 0);

    const std::string turn1 = ids.NextTurnId();
    const std::string turn2 = ids.NextTurnId();
    CHECK(turn1 != turn2);
    CHECK(turn1.rfind("turn-", 0) == 0);

    const std::string item1 = ids.NextItemId();
    const std::string item2 = ids.NextItemId();
    CHECK(item1 != item2);
    CHECK(item1.rfind("item-", 0) == 0);

    const std::string req1 = ids.NextRequestId();
    CHECK(req1.rfind("req-", 0) == 0);
}

TEST_CASE("IdAuthority:并发发号不重不跳") {
    rt::IdAuthority ids;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;
    std::vector<std::thread> workers;
    std::vector<std::vector<std::string>> collected(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&ids, &collected, t] {
            for (int i = 0; i < kPerThread; ++i) {
                collected[static_cast<std::size_t>(t)].push_back(ids.NextItemId());
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    // 汇总去重:2000 个 id 必须个个唯一。
    std::set<std::string> unique;
    for (const auto& batch : collected) {
        unique.insert(batch.begin(), batch.end());
    }
    CHECK(unique.size() == static_cast<std::size_t>(kThreads * kPerThread));
    CHECK(ids.items_issued() == static_cast<std::uint64_t>(kThreads * kPerThread));
}

TEST_CASE("AgentLoop 回调带 tool_use_id:审批请求与终态都认得这次调用") {
    // FakeBackend 脚本:模型叫一件需确认的工具,确认放行后收正文。
    class ScriptBackend : public api::Backend {
    public:
        std::expected<void, api::Error> send_stream(
            const api::Request&,
            const std::function<void(const api::StreamEvent&)>& on_event,
            const std::atomic<bool>* = nullptr) override {
            if (fired) {
                on_event(api::MessageStart{"m2", "test-model"});
                on_event(api::TextDelta{"好了"});
                on_event(api::MessageDone{"end_turn", api::Usage{10, 5}});
                return {};
            }
            fired = true;
            on_event(api::MessageStart{"m1", "test-model"});
            on_event(api::ToolUseStart{0, "toolu_A1", "needs_ask"});
            on_event(api::ToolUseInputDelta{0, "{\"x\":1}"});
            on_event(api::ContentBlockDone{0});
            on_event(api::MessageDone{"tool_use", api::Usage{100, 20}});
            return {};
        }

        bool fired = false;
    };

    ScriptBackend backend;
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("needs_ask", true));
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});

    std::vector<std::string> seen_ids;
    lubancode::test::RecordedTurn turn;
    agent::TurnWiring callbacks;
    callbacks.events = &turn.adapter;
    callbacks.on_tool_confirm = [&seen_ids](const std::string& tool_use_id, const std::string&,
                                            const nlohmann::json&) {
        seen_ids.push_back("confirm:" + tool_use_id);
        return true;
    };

    REQUIRE(loop.Run("用工具", callbacks).has_value());
    // 显示观察改吃事件流:起止的 id 从 ItemStarted/ItemCompleted 里读,
    // 确认侧(seen_ids)与事件侧是同一枚 tool_use_id——模型给的
    // ToolUseBlock.id 三处原样透传。
    REQUIRE(turn.recorder.started_tools.size() == 1);
    CHECK(turn.recorder.started_tools[0].name == "needs_ask");
    CHECK(turn.recorder.started_tools[0].tool_use_id == "toolu_A1");
    REQUIRE(turn.recorder.done_tools.size() == 1);
    CHECK(turn.recorder.done_tools[0].tool_use_id == "toolu_A1");
    REQUIRE(seen_ids.size() == 1);               // 确认问话带同一枚 id
    CHECK(seen_ids[0] == "confirm:toolu_A1");
}
