// 无人值守护栏(one-shot 墙钟预算与收工前催验证单):纯函数钉阈值,假后端
// 钉接线——附注每档一次、步顶到线不发请求、工具上下文带死线、run_command
// 超时封顶;催验证判据五态,经 DriveTurn 续投源真多发一轮且请求里带续跑
// 消息。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "agent/prompt_assembler.hpp"
#include "agent/turn_harness.hpp"
#include "agent/unattended_guard.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/run_command.hpp"
#include "tools/todo_tool.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

using Clock = std::chrono::steady_clock;

// 按脚本吐事件的假后端(同 test_tool_input_fallback 的款)。
class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
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

// 按名冒充的假工具:记下调用次数与执行上下文里有没有死线。
class NamedTool : public tools::Tool {
public:
    explicit NamedTool(std::string name) : name_(std::move(name)) {}
    std::string name() const override { return name_; }
    std::string description() const override { return "fake tool"; }
    nlohmann::json input_schema() const override {
        return nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}};
    }
    bool needs_confirm() const override { return false; }
    tools::Tool::Result execute(const nlohmann::json& /*input*/) override {
        ++call_count;
        return {"ok", false};
    }
    tools::Tool::Result execute(const nlohmann::json& input, const tools::ToolExecutionContext& context) override {
        saw_deadline = context.deadline.has_value();
        return execute(input);
    }

    int call_count = 0;
    bool saw_deadline = false;

private:
    std::string name_;
};

std::vector<api::StreamEvent> ToolScript(const std::string& id, const std::string& tool_name) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, id, tool_name},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

api::Message UserText(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantCalls(std::initializer_list<std::string> names) {
    api::Message message;
    message.role = api::Role::Assistant;
    int i = 0;
    for (const std::string& name : names) {
        api::ToolUseBlock use;
        use.id = "toolu_" + std::to_string(i++);
        use.name = name;
        use.input = nlohmann::json::object();
        message.content.push_back(std::move(use));
    }
    return message;
}

bool MessageHasText(const api::Message& message, const std::string& needle) {
    for (const auto& block : message.content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block); text != nullptr) {
            if (text->text.find(needle) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

std::string ToolResultText(const agent::Agent& loop, std::size_t index) {
    const auto& message = loop.history().at(index);
    for (const auto& block : message.content) {
        if (const auto* result = std::get_if<api::ToolResultBlock>(&block); result != nullptr) {
            return result->content;
        }
    }
    return std::string();
}

}  // namespace

// ---------------------------------------------------------------------------
// 墙钟预算:附注档位
// ---------------------------------------------------------------------------

TEST_CASE("附注:没跨线不发,跨 25% 发一次,再跨 120 秒再发一次,此后不刷") {
    agent::WallBudgetNoteState state;
    const std::int64_t total = 900000;  // 900 秒
    CHECK_FALSE(agent::NextWallBudgetNote(total, 600000, state, true).has_value());
    // 剩 200 秒 < 25%(225 秒),还没到 120 秒:发比例档。
    const auto quarter = agent::NextWallBudgetNote(total, 200000, state, true);
    REQUIRE(quarter.has_value());
    CHECK(quarter->find("[宿主附注]") == 0);
    CHECK(quarter->find("200") != std::string::npos);
    CHECK(state.quarter_sent);
    CHECK_FALSE(state.low_sent);
    CHECK_FALSE(agent::NextWallBudgetNote(total, 150000, state, true).has_value());  // 同档不重发
    // 跨 120 秒:发绝对档。
    const auto low = agent::NextWallBudgetNote(total, 100000, state, false);
    REQUIRE(low.has_value());
    CHECK(low->find("[host note]") == 0);
    CHECK(state.low_sent);
    CHECK_FALSE(agent::NextWallBudgetNote(total, 30000, state, true).has_value());
    CHECK_FALSE(agent::NextWallBudgetNote(total, -5000, state, true).has_value());
}

TEST_CASE("附注:同一刻跨两档只念一回,两档一并记账;没预算恒不发") {
    agent::WallBudgetNoteState state;
    // 总 300 秒:25% = 75 秒 < 120 秒。剩 60 秒时两档同时跨过。
    REQUIRE(agent::NextWallBudgetNote(300000, 60000, state, true).has_value());
    CHECK(state.quarter_sent);
    CHECK(state.low_sent);
    CHECK_FALSE(agent::NextWallBudgetNote(300000, 50000, state, true).has_value());

    agent::WallBudgetNoteState none;
    CHECK_FALSE(agent::NextWallBudgetNote(0, 1000, none, true).has_value());
}

TEST_CASE("步顶闸:剩余不足 20 秒即停发请求") {
    CHECK_FALSE(agent::WallBudgetShouldStopRequests(60000));
    CHECK_FALSE(agent::WallBudgetShouldStopRequests(20001));
    CHECK(agent::WallBudgetShouldStopRequests(20000));
    CHECK(agent::WallBudgetShouldStopRequests(-1));
}

TEST_CASE("WallClockBudget:死线与剩余按起算点算") {
    const Clock::time_point started = Clock::now();
    agent::WallClockBudget budget(started, 900000, true);
    CHECK(budget.deadline() == started + std::chrono::milliseconds(900000));
    CHECK(budget.RemainingMsAt(started + std::chrono::seconds(100)) == 800000);
    CHECK(budget.RemainingMsAt(started + std::chrono::seconds(1000)) == -100000);
}

// ---------------------------------------------------------------------------
// run_command 超时封顶纯函数
// ---------------------------------------------------------------------------

TEST_CASE("超时封顶:剩余减 30 秒余量,下限 5 秒,只往下压") {
    using tools::run_command_detail::CapTimeoutToDeadlineMs;
    CHECK(CapTimeoutToDeadlineMs(120000, std::nullopt) == 120000);  // 没预算原样
    CHECK(CapTimeoutToDeadlineMs(120000, 600000) == 120000);        // 宽裕:不抬不压
    CHECK(CapTimeoutToDeadlineMs(900000, 300000) == 270000);        // 默认长档被压
    CHECK(CapTimeoutToDeadlineMs(86400000, 100000) == 70000);       // 显式 24 小时也受封
    CHECK(CapTimeoutToDeadlineMs(600000, 31000) == 5000);           // 剩 1 秒可用:落下限
    CHECK(CapTimeoutToDeadlineMs(600000, -20000) == 5000);          // 已过死线:仍给下限
    CHECK(CapTimeoutToDeadlineMs(2000, 31000) == 2000);             // 本来就短:不抬到下限
}

// ---------------------------------------------------------------------------
// 收工前催验证:判据
// ---------------------------------------------------------------------------

TEST_CASE("写了没验:按序扫工具调用,写后跑过命令才算验过") {
    std::vector<api::Message> history;
    CHECK_FALSE(agent::HasUnverifiedWrites(history));
    history.push_back(UserText("干活"));
    history.push_back(AssistantCalls({"read_file"}));
    CHECK_FALSE(agent::HasUnverifiedWrites(history));  // 没写过
    history.push_back(AssistantCalls({"write_file"}));
    CHECK(agent::HasUnverifiedWrites(history));  // 写了没验
    history.push_back(AssistantCalls({"run_command"}));
    CHECK_FALSE(agent::HasUnverifiedWrites(history));  // 写后验过
    history.push_back(AssistantCalls({"run_command", "edit_file"}));
    CHECK(agent::HasUnverifiedWrites(history));  // 同一条里先验后改:又欠一笔
}

TEST_CASE("todo 未完:pending/in_progress 都算") {
    tools::TodoListState state;
    CHECK_FALSE(agent::HasOpenTodos(state));
    state.items.push_back(tools::TodoItem{"a", tools::TodoStatus::Completed});
    CHECK_FALSE(agent::HasOpenTodos(state));
    state.items.push_back(tools::TodoItem{"b", tools::TodoStatus::Pending});
    CHECK(agent::HasOpenTodos(state));
    state.items.back().status = tools::TodoStatus::InProgress;
    CHECK(agent::HasOpenTodos(state));
}

TEST_CASE("催不催:有写无验催、验过不催、todo 未完催、催满两回不催、预算不足不催") {
    agent::VerifyNudgeFacts facts;
    CHECK_FALSE(agent::ShouldNudgeVerification(facts));  // 啥都没有

    facts.unverified_writes = true;
    CHECK(agent::ShouldNudgeVerification(facts));

    facts.unverified_writes = false;
    facts.open_todos = true;
    CHECK(agent::ShouldNudgeVerification(facts));

    facts.nudges_sent = 2;
    CHECK_FALSE(agent::ShouldNudgeVerification(facts));
    facts.nudges_sent = 1;
    CHECK(agent::ShouldNudgeVerification(facts));

    facts.remaining_ms = 59999;
    CHECK_FALSE(agent::ShouldNudgeVerification(facts));
    facts.remaining_ms = 60000;
    CHECK(agent::ShouldNudgeVerification(facts));
}

TEST_CASE("文案:续跑消息与预算一句中英两份") {
    CHECK(agent::VerificationNudgeText(false).find("Before you finish") == 0);
    CHECK(agent::VerificationNudgeText(true).find("收工之前") == 0);
    CHECK(agent::WallBudgetPromptLine(900, false).find("about 900 seconds") != std::string::npos);
    CHECK(agent::WallBudgetPromptLine(900, true).find("900 秒") != std::string::npos);
    CHECK(agent::UnattendedPrefersChinese(""));
    CHECK(agent::UnattendedPrefersChinese("zh-CN"));
    CHECK_FALSE(agent::UnattendedPrefersChinese("en"));
}

TEST_CASE("无人值守段:有预算才在末尾报总预算") {
    const std::string plain = agent::UnattendedInstructionSegment("en");
    CHECK(plain.find("seconds of wall-clock") == std::string::npos);
    const std::string budgeted = agent::UnattendedInstructionSegment("en", 900);
    CHECK(budgeted.find(plain) == 0);
    CHECK(budgeted.find("about 900 seconds of wall-clock time in total") != std::string::npos);
    CHECK(agent::UnattendedInstructionSegment("zh", 600).find("600 秒") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 接线:假后端
// ---------------------------------------------------------------------------

TEST_CASE("墙钟预算接线:工具结果追附注每档一次,执行上下文带死线") {
    FakeBackend backend;
    backend.scripts = {ToolScript("toolu_1", "probe"), ToolScript("toolu_2", "probe"), TextOnlyScript("完事")};
    tools::ToolRegistry registry;
    auto owned = std::make_unique<NamedTool>("probe");
    NamedTool* tool = owned.get();
    registry.Register(std::move(owned));
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    // 总 100 秒、刚起算:剩余 < 120 秒,首个工具结果就跨档。
    agent::WallClockBudget budget(Clock::now(), 100000, true);
    agent::TurnWiring wiring;
    wiring.wall_budget = &budget;
    const auto run = loop.Run("干活", wiring);
    REQUIRE(run.has_value());
    CHECK_FALSE(run->hit_time_budget);
    CHECK(tool->call_count == 2);
    CHECK(tool->saw_deadline);
    REQUIRE(loop.history().size() >= 5);
    CHECK(ToolResultText(loop, 2).find("[宿主附注] 墙钟预算") != std::string::npos);
    CHECK(ToolResultText(loop, 4).find("[宿主附注]") == std::string::npos);  // 同档不重发
}

TEST_CASE("墙钟预算接线:没预算时上下文不带死线、结果不追附注") {
    FakeBackend backend;
    backend.scripts = {ToolScript("toolu_1", "probe"), TextOnlyScript("完事")};
    tools::ToolRegistry registry;
    auto owned = std::make_unique<NamedTool>("probe");
    NamedTool* tool = owned.get();
    registry.Register(std::move(owned));
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    const auto run = loop.Run("干活", agent::TurnWiring{});
    REQUIRE(run.has_value());
    CHECK_FALSE(tool->saw_deadline);
    CHECK(ToolResultText(loop, 2).find("[宿主附注]") == std::string::npos);
}

TEST_CASE("墙钟预算接线:剩余不足保留余量,步顶不发请求,按时间预算收场") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("不该发出")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    // 起算点在 1000 秒前、预算 900 秒:早过死线。
    agent::WallClockBudget budget(Clock::now() - std::chrono::seconds(1000), 900000, true);
    agent::TurnWiring wiring;
    wiring.wall_budget = &budget;
    const agent::DriveReport report = agent::DriveTurn(loop, wiring, UserText("干活"), agent::DriveOptions{});
    CHECK(report.ok);
    CHECK(report.time_budget_exhausted);
    CHECK(backend.captured_requests.empty());
    agent::TurnEndgame endgame;
    endgame.time_budget_exhausted = report.time_budget_exhausted;
    endgame.require_final_text = false;
    CHECK(agent::ClassifyTurnEnd(endgame).status == agent::TurnVerdict::Status::BudgetExhausted);
}

TEST_CASE("催验证接线:写了没验就收口,续投源多发一轮且请求里带续跑消息") {
    FakeBackend backend;
    backend.scripts = {ToolScript("toolu_1", "write_file"), TextOnlyScript("写好了"),
                       ToolScript("toolu_2", "run_command"), TextOnlyScript("验过了,完事")};
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<NamedTool>("write_file"));
    registry.Register(std::make_unique<NamedTool>("run_command"));
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    int nudged = 0;
    agent::VerifyNudgeOptions verify;
    verify.agent = &loop;
    verify.chinese = false;
    verify.on_nudge = [&nudged](int index) { nudged = index; };
    agent::DriveOptions options;
    options.continuation = agent::BuildVerifyNudgeContinuation(std::move(verify));
    const agent::DriveReport report = agent::DriveTurn(loop, agent::TurnWiring{}, UserText("写个文件"), options);
    CHECK(report.ok);
    CHECK(nudged == 1);  // 第二回收口时已验过,不再催
    REQUIRE(backend.captured_requests.size() == 4);
    const api::Request& nudge_request = backend.captured_requests[2];
    REQUIRE_FALSE(nudge_request.messages.empty());
    const api::Message& last = nudge_request.messages.back();
    CHECK(last.role == api::Role::User);
    CHECK(MessageHasText(last, "Before you finish: run a command to verify"));
    // 续跑消息是正规 user 消息,落进 durable history。
    bool in_history = false;
    for (const api::Message& message : loop.history()) {
        in_history = in_history || (message.role == api::Role::User &&
                                    MessageHasText(message, "Before you finish"));
    }
    CHECK(in_history);
}

TEST_CASE("催验证接线:写后验过不催;todo 未完催满两回为止;预算不足不催") {
    SUBCASE("写后验过:单轮即收") {
        FakeBackend backend;
        backend.scripts = {ToolScript("toolu_1", "write_file"), ToolScript("toolu_2", "run_command"),
                           TextOnlyScript("完事")};
        tools::ToolRegistry registry;
        registry.Register(std::make_unique<NamedTool>("write_file"));
        registry.Register(std::make_unique<NamedTool>("run_command"));
        agent::Agent loop(backend, registry,
                          agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
        agent::VerifyNudgeOptions verify;
        verify.agent = &loop;
        agent::DriveOptions options;
        options.continuation = agent::BuildVerifyNudgeContinuation(std::move(verify));
        const agent::DriveReport report = agent::DriveTurn(loop, agent::TurnWiring{}, UserText("干活"), options);
        CHECK(report.ok);
        CHECK(backend.captured_requests.size() == 3);
    }
    SUBCASE("todo 一直没完:整场至多催两回") {
        FakeBackend backend;
        backend.scripts = {TextOnlyScript("先这样"), TextOnlyScript("还没完"), TextOnlyScript("就这样吧"),
                           TextOnlyScript("不该发出")};
        tools::ToolRegistry registry;
        agent::Agent loop(backend, registry,
                          agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
        auto todo = std::make_shared<tools::TodoListState>();
        todo->items.push_back(tools::TodoItem{"跑测试", tools::TodoStatus::InProgress});
        agent::VerifyNudgeOptions verify;
        verify.agent = &loop;
        verify.todo_state = todo;
        agent::DriveOptions options;
        options.continuation = agent::BuildVerifyNudgeContinuation(std::move(verify));
        const agent::DriveReport report = agent::DriveTurn(loop, agent::TurnWiring{}, UserText("干活"), options);
        CHECK(report.ok);
        CHECK(backend.captured_requests.size() == 3);  // 首轮 + 两回催
        CHECK(MessageHasText(backend.captured_requests[1].messages.back(), "收工之前"));
    }
    SUBCASE("剩余不足 60 秒:写了没验也不催") {
        FakeBackend backend;
        backend.scripts = {ToolScript("toolu_1", "write_file"), TextOnlyScript("写好了"), TextOnlyScript("不该发出")};
        tools::ToolRegistry registry;
        registry.Register(std::make_unique<NamedTool>("write_file"));
        agent::Agent loop(backend, registry,
                          agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
        // 总 900 秒、起算在 850 秒前:剩约 50 秒(步顶闸 20 秒线之上,请求照发)。
        agent::WallClockBudget budget(Clock::now() - std::chrono::seconds(850), 900000, true);
        agent::VerifyNudgeOptions verify;
        verify.agent = &loop;
        verify.wall_budget = &budget;
        agent::DriveOptions options;
        options.continuation = agent::BuildVerifyNudgeContinuation(std::move(verify));
        agent::TurnWiring wiring;
        wiring.wall_budget = &budget;
        const agent::DriveReport report = agent::DriveTurn(loop, wiring, UserText("干活"), options);
        CHECK(report.ok);
        CHECK_FALSE(report.time_budget_exhausted);
        CHECK(backend.captured_requests.size() == 2);
    }
}
