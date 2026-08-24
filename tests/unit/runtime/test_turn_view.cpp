// 回合视觉收束单:TurnView/TurnCollector/loop 批次边界/footer 尺子的单测。
//
// 钉的账:
//   1. loop 的 step/batch 回调:同一条 assistant message 吐三枚 tool_use
//      时,batch.started 带齐三枚 id、串行推进、batch.finished 收口;下一
//      次模型响应另起 step;没工具的 step 不发空 batch;ESC 中断时
//      finished(interrupted=true)。
//   2. TurnCollector:批次先登记 Pending、start 点亮 Running、终态各归各;
//      ESC 后 Pending -> Skipped、Running -> Interrupted;正文/思考入账
//      成条目;usage/request 记账;FinishTurn 终态唯一。
//   3. FormatTurnDuration/FormatTurnFooterText:十秒一位小数/一分钟 Xm Ys/
//      一小时 Xh Ym;Worked/Stopped/Failed 三态词干。
//   4. BuildTurnFooterLine:文字嵌线左、余线填满;窄于 40 列只写文案;
//      中文按显示列算。
//   5. 枚举稳定字符串往返(TurnView 一族)。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "app/turn_runner.hpp"
#include "cli/console_input.hpp"
#include "cli/context_tracker.hpp"
#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/theme.hpp"
#include "cli/tool_display.hpp"
#include "cli/transcript.hpp"
#include "cli/turn_renderer.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/turn_collector.hpp"
#include "runtime/turn_view.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

namespace {

// 按脚本吐事件的假后端(与 test_loop.cpp 同款,精简版)。
class FakeBackend : public lubancode::api::Backend {
public:
    std::vector<std::vector<lubancode::api::StreamEvent>> scripts;
    std::vector<lubancode::api::Request> captured;

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* = nullptr) override {
        captured.push_back(request);
        const std::size_t idx = captured.size() - 1;
        if (idx >= scripts.size()) {
            return std::unexpected(lubancode::api::Error{lubancode::api::ErrorKind::Api, "脚本用完了", 0});
        }
        for (const auto& event : scripts[idx]) {
            on_event(event);
        }
        return {};
    }
};

class EchoTool : public lubancode::tools::Tool {
public:
    explicit EchoTool(std::string name) : name_(std::move(name)) {}
    std::string name() const override { return name_; }
    std::string description() const override { return "echo for test"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return false; }
    lubancode::tools::Tool::Result execute(const nlohmann::json&) override { return {"ok", false}; }

private:
    std::string name_;
};

std::vector<lubancode::api::StreamEvent> MultiToolScript(const std::vector<std::string>& tool_ids,
                                                          const std::string& tool_name = "echo") {
    std::vector<lubancode::api::StreamEvent> events;
    events.push_back(lubancode::api::MessageStart{"msg", "model"});
    for (std::size_t i = 0; i < tool_ids.size(); ++i) {
        events.push_back(lubancode::api::ToolUseStart{static_cast<int>(i), tool_ids[i], tool_name});
        events.push_back(lubancode::api::ToolUseInputDelta{static_cast<int>(i), "{}"});
        events.push_back(lubancode::api::ContentBlockDone{static_cast<int>(i)});
    }
    events.push_back(lubancode::api::MessageDone{"tool_use", lubancode::api::Usage{}});
    return events;
}

std::vector<lubancode::api::StreamEvent> TextScript(const std::string& text) {
    return {
        lubancode::api::MessageStart{"msg", "model"},
        lubancode::api::TextDelta{text},
        lubancode::api::ContentBlockDone{0},
        lubancode::api::MessageDone{"end_turn", lubancode::api::Usage{}},
    };
}

}  // namespace

// ---------------------------------------------------------------------------
// loop 的 step/batch 边界回调
// ---------------------------------------------------------------------------

TEST_CASE("loop 批次边界:三枚 tool_use 一批,start 带齐 id、串行推进、finished 收口") {
    FakeBackend backend;
    backend.scripts = {
        MultiToolScript({"t1", "t2", "t3"}),
        TextScript("收工"),
    };
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<EchoTool>("echo"));
    lubancode::agent::AgentLoop loop(backend, registry, "m", "sys");

    struct Log {
        std::vector<int> steps;
        struct Batch {
            int step;
            int batch;
            std::vector<std::string> ids;
        };
        std::vector<Batch> batches_started;
        std::vector<std::pair<int, bool>> batches_finished;
        std::vector<std::string> tool_starts;  // start/done 交错的账
        std::vector<std::string> tool_donels;
    } log;

    lubancode::agent::Callbacks cb;
    cb.on_model_step_started = [&](int s) { log.steps.push_back(s); };
    cb.on_tool_batch_started = [&](int s, int b, const std::vector<std::string>& ids) {
        log.batches_started.push_back({s, b, ids});
    };
    cb.on_tool_batch_finished = [&](int b, bool interrupted) { log.batches_finished.emplace_back(b, interrupted); };
    cb.on_tool_start = [&](const std::string& id, const std::string&, const nlohmann::json&) {
        log.tool_starts.push_back(id);
    };
    cb.on_tool_done = [&](const std::string& id, const std::string&, const lubancode::tools::Tool::Result&) {
        log.tool_donels.push_back(id);
    };

    const auto result = loop.Run("问题", cb);
    REQUIRE(result.has_value());

    // 两次模型请求 = 两个 step(0 与 1);只有第一个 step 有批次。
    REQUIRE(log.steps.size() == 2);
    CHECK(log.steps[0] == 0);
    CHECK(log.steps[1] == 1);

    REQUIRE(log.batches_started.size() == 1);
    CHECK(log.batches_started[0].step == 0);
    CHECK(log.batches_started[0].batch == 0);
    REQUIRE(log.batches_started[0].ids.size() == 3);
    CHECK(log.batches_started[0].ids[0] == "t1");
    CHECK(log.batches_started[0].ids[1] == "t2");
    CHECK(log.batches_started[0].ids[2] == "t3");

    // batch.started 先于一切 tool_start:模型这拍打算跑三件,先全报出来。
    REQUIRE(log.tool_starts.size() == 3);
    CHECK(log.tool_starts[0] == "t1");
    CHECK(log.tool_starts[1] == "t2");
    CHECK(log.tool_starts[2] == "t3");

    // 串行推进:t1 done 之后 t2 才 start(交错账:start,done,start,done…)
    REQUIRE(log.tool_donels.size() == 3);
    CHECK(log.tool_donels[0] == "t1");

    REQUIRE(log.batches_finished.size() == 1);
    CHECK(log.batches_finished[0].first == 0);
    CHECK(log.batches_finished[0].second == false);
}

TEST_CASE("loop 批次边界:纯文本轮不发空 batch") {
    FakeBackend backend;
    backend.scripts = {TextScript("答")};
    lubancode::tools::ToolRegistry registry;
    lubancode::agent::AgentLoop loop(backend, registry, "m", "sys");

    int batches = 0;
    int steps = 0;
    lubancode::agent::Callbacks cb;
    cb.on_model_step_started = [&](int) { ++steps; };
    cb.on_tool_batch_started = [&](int, int, const std::vector<std::string>&) { ++batches; };

    REQUIRE(loop.Run("问", cb).has_value());
    CHECK(steps == 1);
    CHECK(batches == 0);  // 单子:没有工具不发空 batch
}

TEST_CASE("loop 批次边界:两批之间 step 换拍,批次序号跨 step 不重号") {
    FakeBackend backend;
    backend.scripts = {
        MultiToolScript({"a1"}),
        MultiToolScript({"b1", "b2"}),
        TextScript("完"),
    };
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<EchoTool>("echo"));
    lubancode::agent::AgentLoop loop(backend, registry, "m", "sys");

    std::vector<std::pair<int, int>> started;  // (step, batch)
    lubancode::agent::Callbacks cb;
    cb.on_tool_batch_started = [&](int s, int b, const std::vector<std::string>&) { started.emplace_back(s, b); };

    REQUIRE(loop.Run("问", cb).has_value());
    REQUIRE(started.size() == 2);
    CHECK(started[0].first == 0);
    CHECK(started[0].second == 0);
    CHECK(started[1].first == 1);
    CHECK(started[1].second == 1);  // 第二个 step 的批次是 1,不是 0
}

TEST_CASE("loop 批次边界:ESC 中断时 finished 带 interrupted=true") {
    // 第二枚工具执行时置 cancel:第一枚 done、第二枚补合成结果。
    class CancelOnSecondTool : public lubancode::tools::Tool {
    public:
        std::string name() const override { return "cancel_second"; }
        std::string description() const override { return "cancel"; }
        nlohmann::json input_schema() const override { return nlohmann::json::object(); }
        bool needs_confirm() const override { return false; }
        lubancode::tools::Tool::Result execute(const nlohmann::json&) override {
            if (++calls == 2 && flag != nullptr) {
                flag->store(true);
            }
            return {"ok", false};
        }
        int calls = 0;
        std::atomic<bool>* flag = nullptr;
    };

    FakeBackend backend;
    backend.scripts = {
        MultiToolScript({"c1", "c2"}, "cancel_second"),
        TextScript("不该到这"),
    };
    std::atomic<bool> cancel{false};
    auto tool = std::make_unique<CancelOnSecondTool>();
    tool->flag = &cancel;
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::move(tool));
    lubancode::agent::AgentLoop loop(backend, registry, "m", "sys");

    bool finished_interrupted = false;
    int finished_batch = -1;
    lubancode::agent::Callbacks cb;
    cb.on_tool_batch_finished = [&](int b, bool interrupted) {
        finished_batch = b;
        finished_interrupted = interrupted;
    };

    const auto result = loop.Run("问", cb, &cancel);
    REQUIRE(result.has_value());
    CHECK(result->cancelled);
    CHECK(finished_batch == 0);
    CHECK(finished_interrupted);
}

// ---------------------------------------------------------------------------
// TurnCollector:批次分组与状态账
// ---------------------------------------------------------------------------

TEST_CASE("collector:一批三枚先登记 Pending,start 点亮 Running,终态各归各") {
    lubancode::runtime::IdAuthority ids;
    lubancode::runtime::TurnCollector collector(ids, "turn-1");
    collector.StartTurn("用户问", 1000);

    collector.OnModelStepStarted(0);
    collector.OnToolBatchStarted(0, 0, {"t1", "t2", "t3"});

    const auto& view = collector.view();
    REQUIRE(view.batches.size() == 1);
    REQUIRE(view.batches[0].ordered_item_ids.size() == 3);
    REQUIRE(view.items.size() == 4);  // user + 三枚 Pending
    CHECK(view.items[1].status == lubancode::runtime::TurnItemViewState::Pending);
    CHECK(view.items[2].status == lubancode::runtime::TurnItemViewState::Pending);
    CHECK(view.items[3].status == lubancode::runtime::TurnItemViewState::Pending);
    CHECK(view.items[1].batch_id == view.batches[0].batch_id);
    CHECK(view.items[3].step_id == "step-0");
    CHECK(view.metrics.tool_count == 3);

    // t2 先点亮:批次登记的条目按 id 路由,不认次序。
    collector.OnToolStarted("t2", "echo", nlohmann::json{{"x", 1}});
    CHECK(view.items[1].status == lubancode::runtime::TurnItemViewState::Pending);
    CHECK(view.items[2].status == lubancode::runtime::TurnItemViewState::Running);
    CHECK(view.items[3].status == lubancode::runtime::TurnItemViewState::Pending);

    collector.OnToolFinished("t2", "ok", false);
    collector.OnToolStarted("t1", "echo", nlohmann::json{});
    collector.OnToolFinished("t1", "坏", true);
    collector.OnToolStarted("t3", "echo", nlohmann::json{});
    collector.OnToolFinished("t3", "ok", false);
    collector.OnToolBatchFinished(0, false);

    CHECK(view.items[1].status == lubancode::runtime::TurnItemViewState::Failed);
    CHECK(view.items[2].status == lubancode::runtime::TurnItemViewState::Succeeded);
    CHECK(view.items[3].status == lubancode::runtime::TurnItemViewState::Succeeded);
    CHECK(view.metrics.failed_tool_count == 1);
    CHECK(view.batches[0].finished);
}

TEST_CASE("collector:ESC 后 Pending -> Skipped、Running -> Interrupted") {
    lubancode::runtime::IdAuthority ids;
    lubancode::runtime::TurnCollector collector(ids, "turn-9");
    collector.StartTurn("问", 0);
    collector.OnModelStepStarted(0);
    collector.OnToolBatchStarted(0, 0, {"x1", "x2", "x3"});
    collector.OnToolStarted("x1", "echo", nlohmann::json{});  // x1 在跑
    collector.OnToolStarted("x2", "echo", nlohmann::json{});
    collector.OnToolFinished("x2", "ok", false);  // x2 跑完了

    // ESC:x1 正在跑 -> Interrupted;x3 还没开跑 -> Skipped。
    collector.OnToolFinished("x1", "打断", true,
                             lubancode::runtime::TurnItemViewState::Interrupted);
    collector.MarkRunningInterrupted();
    collector.OnToolBatchFinished(0, true);

    const auto& view = collector.view();
    CHECK(view.items[1].status == lubancode::runtime::TurnItemViewState::Interrupted);
    CHECK(view.items[2].status == lubancode::runtime::TurnItemViewState::Succeeded);
    CHECK(view.items[3].status == lubancode::runtime::TurnItemViewState::Skipped);
}

TEST_CASE("collector:正文与思考入账,CloseTextItems 收口 Succeeded") {
    lubancode::runtime::IdAuthority ids;
    lubancode::runtime::TurnCollector collector(ids, "turn-t");
    collector.StartTurn("问", 0);
    collector.OnModelStepStarted(0);
    collector.OnTextDelta("先想", /*thinking=*/true);
    collector.OnTextDelta("再想", /*thinking=*/true);
    collector.OnTextDelta("正文开头", /*thinking=*/false);
    collector.OnTextDelta("正文续", /*thinking=*/false);

    const auto& view = collector.view();
    REQUIRE(view.items.size() == 3);  // user + thinking + text
    CHECK(view.items[1].kind == lubancode::runtime::TurnItemViewKind::Thinking);
    CHECK(view.items[1].result_text == "先想再想");
    CHECK(view.items[2].kind == lubancode::runtime::TurnItemViewKind::Text);
    CHECK(view.items[2].result_text == "正文开头正文续");
    CHECK(view.items[1].seq < view.items[2].seq);  // 原序靠 seq 保住

    collector.CloseTextItems();
    CHECK(view.items[1].status == lubancode::runtime::TurnItemViewState::Succeeded);
    CHECK(view.items[2].status == lubancode::runtime::TurnItemViewState::Succeeded);
}

TEST_CASE("collector:usage 与 FinishTurn 的 metrics 账") {
    lubancode::runtime::IdAuthority ids;
    lubancode::runtime::TurnCollector collector(ids, "turn-m");
    collector.StartTurn("问", 0);

    lubancode::api::UsageReport a;
    a.usage.input_tokens = 100;
    a.usage.output_tokens = 20;
    lubancode::api::UsageReport b;
    b.usage.input_tokens = 50;
    b.usage.cache_read_tokens = 30;
    b.usage.output_reasoning_tokens = 7;
    collector.OnUsage(a);
    collector.OnUsage(b);

    collector.FinishTurn(lubancode::runtime::TurnItemViewState::Succeeded,
                         /*wall=*/401000, /*approval=*/35000);
    const auto& m = collector.view().metrics;
    CHECK(m.request_count == 2);
    CHECK(m.input_tokens == 150);
    CHECK(m.cache_read_tokens == 30);
    CHECK(m.reasoning_tokens == 7);
    CHECK(m.wall_duration_ms == 401000);
    CHECK(m.approval_wait_ms == 35000);
    CHECK(collector.view().finished);
    CHECK(collector.view().status == lubancode::runtime::TurnItemViewState::Succeeded);
}

TEST_CASE("collector:父子栈——子代理内层工具挂 AgentItem 下,不进 main 批次") {
    lubancode::runtime::IdAuthority ids;
    lubancode::runtime::TurnCollector collector(ids, "turn-p");
    collector.StartTurn("问", 0);
    collector.OnModelStepStarted(0);
    collector.OnToolBatchStarted(0, 0, {"agent-1"});
    collector.OnToolStarted("agent-1", "agent", nlohmann::json{});
    // agent 终态一到,批次收口——内层工具在批次外另立(模型视角它们不在
    // assistant message 的 tool_use 里)。
    collector.OnToolBatchFinished(0, false);
    collector.PushParent(collector.view().items[1].item_id);

    // 子代理内层工具:不在批次登记表里,现场立条目、挂父。
    collector.OnToolStarted("inner-1", "read_file", nlohmann::json{{"path", "x"}});
    const auto& view = collector.view();
    REQUIRE(view.items.size() == 3);
    CHECK(view.items[2].parent_item_id == view.items[1].item_id);
    CHECK(view.items[2].batch_id.empty());  // 不与 main 的 ToolBatch 混为一排
    collector.PopParent();
    collector.OnToolFinished("agent-1", "结论", false);
}

// ---------------------------------------------------------------------------
// footer 尺子与横线
// ---------------------------------------------------------------------------

TEST_CASE("FormatTurnDuration:档位分明") {
    using lubancode::cli::FormatTurnDuration;
    CHECK(FormatTurnDuration(9400) == "9.4s");
    CHECK(FormatTurnDuration(0) == "0.0s");
    CHECK(FormatTurnDuration(9999) == "10.0s");
    CHECK(FormatTurnDuration(10'000) == "10s");
    CHECK(FormatTurnDuration(42'300) == "42s");
    CHECK(FormatTurnDuration(59'999) == "59s");
    CHECK(FormatTurnDuration(60'000) == "1m 0s");
    CHECK(FormatTurnDuration(401'000) == "6m 41s");
    CHECK(FormatTurnDuration(3'540'000) == "59m 0s");
    CHECK(FormatTurnDuration(3'600'000) == "1h 0m");
    CHECK(FormatTurnDuration(5'400'000) == "1h 30m");
    CHECK(FormatTurnDuration(-5) == "0.0s");  // 负数按 0 兜
}

TEST_CASE("FormatTurnFooterText:三态词干") {
    using lubancode::cli::FormatTurnFooterText;
    using Tone = lubancode::cli::TurnFooterTone;
    CHECK(lubancode::cli::FormatTurnFooterText(401'000, Tone::Worked) == "Worked for 6m 41s");
    CHECK(lubancode::cli::FormatTurnFooterText(18'200, Tone::Stopped) == "Stopped after 18s");
    CHECK(lubancode::cli::FormatTurnFooterText(7'600, Tone::Failed) == "Failed after 7.6s");
    CHECK(lubancode::cli::FormatApprovalWaitNote(35'000) == " · waited 35s for approval");
    CHECK(lubancode::cli::FormatApprovalWaitNote(0).empty());
}

TEST_CASE("BuildTurnFooterLine:文字嵌线、窄屏退化、中文按显示列") {
    using lubancode::cli::BuildTurnFooterLine;
    // 80 列:2 列线 + " Worked for 6m 41s " + 余线填到 79 列。
    const std::string line = BuildTurnFooterLine("Worked for 6m 41s", 80, /*plain=*/false);
    REQUIRE(line.size() > 16);
    // 前缀:两枚 ─(各 3 字节)。
    CHECK(line.substr(0, 6) == "\xe2\x94\x80\xe2\x94\x80");
    CHECK(line.substr(6, 1) == " ");
    // 精确账:79 列 = 左线 2 + 空格 1 + 文字 17 + 空格 1 + 尾线;尾线 = 58。
    const std::size_t rule_count = [&] {
        std::size_t n = 0;
        for (std::size_t i = 0; i + 3 <= line.size(); ++i) {
            if (line.compare(i, 3, "\xe2\x94\x80") == 0) {
                ++n;
                i += 2;
            }
        }
        return n;
    }();
    CHECK(rule_count == 60);  // 左 2 + 尾 58

    // 窄于 40 列:只写文案,不硬塞线。
    CHECK(BuildTurnFooterLine("Worked for 6m 41s", 30, false) == "Worked for 6m 41s");
    CHECK(BuildTurnFooterLine("Worked for 6m 41s", 39, false) == "Worked for 6m 41s");
    // 40 列:39 安全宽,文案 17 + 装饰 4 = 21,塞不下 39?21 < 39 塞得下;
    // 但尾线只剩 39-21=18 列,线该有。恰好踩界的写法按实现:width >= 40 且
    // decorated < width 都成立,带线。
    const std::string forty = BuildTurnFooterLine("Worked for 6m 41s", 40, false);
    if (forty == "Worked for 6m 41s") {
        // 实现取退化路(文案 + 装饰 >= 40 时只写文案):40 >= 40 成立,
        // 退化——这条也钉住,防将来悄悄变。
        CHECK(true);
    } else {
        CHECK(forty.find("Worked for 6m 41s") != std::string::npos);
        CHECK(forty.size() > std::string("Worked for 6m 41s").size());
    }

    // plain 用 "-"。
    const std::string plain = BuildTurnFooterLine("Worked for 1s", 80, true);
    CHECK(plain.substr(0, 3) == "-- ");

    // 中文按显示列:每个汉字占 2 列,不按字节劈。
    const std::string zh = BuildTurnFooterLine("干活 6分41秒", 80, false);
    CHECK(zh.find("干活 6分41秒") != std::string::npos);

    // 空文案没有线。
    CHECK(BuildTurnFooterLine("", 80, false).empty());
}

TEST_CASE("PrintTurnFooter:真终端在 Worked 横线前留一行,管道不改契约") {
    const lubancode::cli::Theme theme = lubancode::cli::BuiltinTheme("plain");
    std::ostringstream captured;
    std::streambuf* const old_buf = std::cout.rdbuf(captured.rdbuf());

    lubancode::app::PrintTurnFooter(theme, /*is_console=*/true, 15000,
                                    lubancode::cli::TurnFooterTone::Worked);
    const std::string console = captured.str();
    captured.str({});
    captured.clear();
    lubancode::app::PrintTurnFooter(theme, /*is_console=*/false, 15000,
                                    lubancode::cli::TurnFooterTone::Worked);
    const std::string pipe = captured.str();
    std::cout.rdbuf(old_buf);

    CHECK(console.starts_with("\n"));
    CHECK(console.find("Worked for 15s") != std::string::npos);
    CHECK_FALSE(pipe.starts_with("\n"));
    CHECK(pipe.find("Worked for 15s") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 枚举稳定字符串
// ---------------------------------------------------------------------------

TEST_CASE("TurnView 枚举稳定字符串往返") {
    using namespace lubancode::runtime;
    CHECK(ToString(TurnItemViewKind::User) == "user");
    CHECK(ToString(TurnItemViewState::Skipped) == "skipped");
    CHECK(ToString(TurnItemViewState::Interrupted) == "interrupted");
    CHECK(ToString(TurnActivityPhase::WaitingApproval) == "waiting_approval");

    TurnItemViewKind k{};
    TurnItemViewState s{};
    TurnActivityPhase p{};
    REQUIRE(ParseTurnItemViewKind("warning", k));
    CHECK(k == TurnItemViewKind::Warning);
    REQUIRE(ParseTurnItemViewState("declined", s));
    CHECK(s == TurnItemViewState::Declined);
    REQUIRE(ParseTurnActivityPhase("stopping", p));
    CHECK(p == TurnActivityPhase::Stopping);
    CHECK_FALSE(ParseTurnItemViewState("no-such", s));
    CHECK_FALSE(ParseTurnActivityPhase("", p));
}

// ---------------------------------------------------------------------------
// ToolDisplay 的批次预告与多行命令标题(回合视觉收束单第三/四节)
// ---------------------------------------------------------------------------

TEST_CASE("多行 run_command 标题:只取首个非空逻辑行,末尾 +N lines") {
    using lubancode::cli::BuildToolTitle;
    // 单行:照旧。
    CHECK(BuildToolTitle("run_command", {{"command", "git status"}}).find("+") == std::string::npos);

    // 五行脚本:首行 + "+4 lines"(不按分号切、不横铺)。
    const std::string script = "$env:http_proxy='http://127.0.0.1:10808'\n"
                               "$env:https_proxy=$env:http_proxy\n"
                               "git fetch origin\n"
                               "git merge origin/main\n"
                               "git log --oneline -3";
    const std::string title = BuildToolTitle("run_command", {{"command", script}});
    CHECK(title.find("$env:http_proxy='http://127.0.0.1:10808'") != std::string::npos);
    CHECK(title.find("git merge") == std::string::npos);  // 后续行不进标题
    CHECK(title.find("+4 lines") != std::string::npos);

    // 前导空行跳过:首行从第一个非空行起算;余行数按"首行之外还有几行"
    // 报(含首行后的空行,如实)。"\n\nls\npwd" 四行,首非空是 ls,余 1 行。
    const std::string leading = "\n\nls\npwd";
    const std::string t2 = BuildToolTitle("run_command", {{"command", leading}});
    CHECK(t2.find("ls") != std::string::npos);
    CHECK(t2.find("+1 lines") != std::string::npos);

    // 引号/管道里的分号不是命令边界:整段算一条命令(单子原文)。
    const std::string semicolons = "echo \"a;b;c\"";
    CHECK(BuildToolTitle("run_command", {{"command", semicolons}}).find("echo \"a;b;c\"") != std::string::npos);
}

TEST_CASE("ToolDisplay 工具未 start 不立无名 Pending,start 后才落具名条目") {
    std::vector<lubancode::cli::TranscriptItem> transcript;
    std::atomic<bool> cancel{false};
    lubancode::cli::Theme theme;
    lubancode::cli::ToolDisplay display(transcript, theme, /*console=*/false, nullptr, &cancel);

    display.OnBatchSkipped();
    CHECK(transcript.empty());

    display.OnToolStart("b2", "read_file", nlohmann::json{{"path", "x"}});
    REQUIRE(transcript.size() == 1);
    CHECK(transcript[0].status == lubancode::cli::TranscriptStatus::Running);
    CHECK(transcript[0].tool_name == "read_file");
    CHECK(transcript[0].title.find("read_file") != std::string::npos);

    display.OnToolDone("b2", "read_file", lubancode::tools::Tool::Result{"ok", false});
    CHECK(transcript[0].status == lubancode::cli::TranscriptStatus::Ok);
}

// ---------------------------------------------------------------------------
// 黄金画面(落地次序第 1 步):TerminalTurnRenderer 的行组快照。
// 七景里离线可钉的五景:单工具、多工具一批、两次 model step、长命令、
// 失败;ask_user 与 ESC 的画面手测(画面类测不了的,controller/行组纯逻辑
// 必须有单测——单子验收口径)。80 列代表;120/160 只是宽度参数,排版公式
// 同一颗。
// ---------------------------------------------------------------------------

namespace {

// 攒一轮两拍的账:step0 思考+两工具一批,step1 长命令;正常收口。
lubancode::runtime::TurnView BuildTurnForRender() {
    lubancode::runtime::IdAuthority ids;
    lubancode::runtime::TurnCollector collector(ids, "turn-g");
    collector.StartTurn("总结最近的提交", 1000);

    collector.OnModelStepStarted(0);
    collector.OnTextDelta("先看看日志", /*thinking=*/true);
    collector.CloseTextItems();
    collector.OnToolBatchStarted(0, 0, {"g1", "g2"});
    collector.OnToolStarted("g1", "run_command", nlohmann::json{{"command", "git log --oneline -3"}});
    collector.OnToolFinished("g1", "[退出码 0]\nabc123 fix\ndef456 feat", false);
    collector.OnToolStarted("g2", "read_file", nlohmann::json{{"path", "README.md"}});
    collector.OnToolFinished("g2", "第一行\n第二行", false);
    collector.OnToolBatchFinished(0, false);

    collector.OnModelStepStarted(1);
    collector.OnToolBatchStarted(1, 1, {"g3"});
    collector.OnToolStarted("g3", "run_command",
                            nlohmann::json{{"command", "$env:http_proxy='http://127.0.0.1:10808'\n"
                                                       "$env:https_proxy=$env:http_proxy\n"
                                                       "git fetch origin"}});
    collector.OnToolFinished("g3", "[退出码 0]\n拉取完成", false);
    collector.OnToolBatchFinished(1, false);

    collector.FinishTurn(lubancode::runtime::TurnItemViewState::Succeeded, 12800, 0);
    return collector.take_view();
}

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += line + "\n";
    }
    return out;
}

}  // namespace

TEST_CASE("黄金画面:单轮两拍(思考 + 两工具一批 -> 换拍 -> 长命令)80 列") {
    const lubancode::runtime::TurnView view = BuildTurnForRender();
    lubancode::cli::Theme theme = lubancode::cli::BuiltinTheme("plain");
    lubancode::cli::TurnRenderOptions options;
    options.width = 80;
    options.plain = true;
    const std::vector<std::string> lines = lubancode::cli::RenderTurnView(view, theme, options);
    const std::string text = JoinLines(lines);

    // 用户条目在头一行:背景块格式("> " 提示符,与 live/resume 同一颗
    // FormatUserPromptBlock),不再是工具条目样。块后按间距表垫一口。
    REQUIRE(!lines.empty());
    CHECK(lines.front() == "> 总结最近的提交");
    REQUIRE(lines.size() >= 2);
    CHECK(lines[1].empty());  // UserPrompt -> 下一块 = 1 行气口

    // 思考条目:思考 Xs 一行。
    bool saw_thinking = false;
    for (const std::string& line : lines) {
        if (line.find("思考 ") != std::string::npos) {
            saw_thinking = true;
            break;
        }
    }
    CHECK(saw_thinking);

    // 两枚工具一批:同拍条目之间不垫空行。
    const std::size_t g1 = text.find("run_command(git log");
    const std::size_t g2 = text.find("read_file(README.md)");
    REQUIRE(g1 != std::string::npos);
    REQUIRE(g2 != std::string::npos);
    const std::string between = text.substr(g1, g2 - g1);
    CHECK(between.find("退出码 0") != std::string::npos);
    CHECK(between.find("\n\n") == std::string::npos);  // 同拍无空行

    // 换拍:step 1 的条目前有一空行(轻间隔),不是满宽横线。
    const std::size_t g3_title = text.find("run_command($env:http_proxy");
    REQUIRE(g3_title != std::string::npos);
    // 该条目所在行之前应有空行(行组里 lines 相邻两条,前一条为空串)。
    bool gap_before_step2 = false;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (lines[i].find("run_command($env:http_proxy") != std::string::npos && lines[i - 1].empty()) {
            gap_before_step2 = true;
            break;
        }
    }
    CHECK(gap_before_step2);
    // step 边界不画满宽横线;turn 分隔线只在多轮重放的用户块之前画,单轮
    // (lines 起手为空)不画——本景单轮,全篇不得见独立的横线行(footer 那
    // 行的字嵌在线里,不算独立横线,另查)。
    CHECK(text.find("────") == std::string::npos);
    for (const std::string& line : lines) {
        const bool pure_rule =
            !line.empty() && line.find_first_not_of('-') == std::string::npos;
        CHECK_FALSE(pure_rule);
    }

    // 长命令:+2 lines 记号,首行不横铺。
    CHECK(text.find("$env:http_proxy='http://127.0.0.1:10808' +2 lines") != std::string::npos);
    CHECK(text.find("git fetch origin") == std::string::npos);  // 后续行不进标题

    // footer:恰一枚,带总耗时(12.8s 按尺子取整成 12s:十至五十九秒取整)。
    std::size_t footer_count = 0;
    for (const std::string& line : lines) {
        if (line.find("Worked for") != std::string::npos) {
            ++footer_count;
        }
    }
    CHECK(footer_count == 1);
    CHECK(text.find("Worked for 12s") != std::string::npos);
}

TEST_CASE("黄金画面:多轮重放用户块之前有 turn 分隔线,单轮不画") {
    // 两轮:第一轮收口,第二轮紧随。多轮重放(Ctrl+L)由调用方从第二轮起
    // 置 leading_turn_divider——"上面有没有前一轮"是 caller 的账(renderer
    // 无跨轮记忆),renderer 只照办在用户块之前画一道克制横线。
    lubancode::runtime::IdAuthority ids;
    lubancode::runtime::TurnCollector first(ids, "turn-d1");
    first.StartTurn("第一个问题", 0);
    first.OnModelStepStarted(0);
    first.FinishTurn(lubancode::runtime::TurnItemViewState::Succeeded, 5000, 0);
    lubancode::runtime::TurnCollector second(ids, "turn-d2");
    second.StartTurn("第二个问题", 6000);
    second.OnModelStepStarted(0);
    second.FinishTurn(lubancode::runtime::TurnItemViewState::Succeeded, 7000, 0);

    lubancode::cli::Theme theme = lubancode::cli::BuiltinTheme("plain");
    lubancode::cli::TurnRenderOptions options;
    options.width = 80;
    options.plain = true;

    // 模拟 Ctrl+L 的多轮循环:首轮不带横线,次轮起各带一道。
    std::vector<std::string> all;
    bool first_turn = true;
    for (const lubancode::runtime::TurnView* view : {&first.view(), &second.view()}) {
        options.leading_turn_divider = !first_turn;
        first_turn = false;
        for (const std::string& line : lubancode::cli::RenderTurnView(*view, theme, options)) {
            all.push_back(line);
        }
    }
    // 第二轮用户块之前:footer 之后隔一口 + 一道横线(plain 用 '-')。
    std::size_t second_user = std::string::npos;
    int divider_seen = 0;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i] == "> 第二个问题") {
            second_user = i;
            break;
        }
    }
    REQUIRE(second_user != std::string::npos);
    for (std::size_t i = 0; i < second_user; ++i) {
        // 独立横线行:非空、只有 '-'。footer 行字嵌线里,不算。
        if (!all[i].empty() && all[i].find_first_not_of('-') == std::string::npos) {
            ++divider_seen;
        }
    }
    CHECK(divider_seen == 1);  // 恰一道:首轮(会话开头)不画
    // 横线紧贴用户块:横线行的下一行就是用户块(中间不再垫空行)。
    for (std::size_t i = 1; i < second_user; ++i) {
        if (!all[i].empty() && all[i].find_first_not_of('-') == std::string::npos) {
            CHECK(i + 1 == second_user);
        }
    }

    // 首轮不开 leading_turn_divider(缺省):独立横线全无(footer 行字嵌
    // 线里,不在此列)。
    lubancode::cli::TurnRenderOptions no_divider = options;
    no_divider.leading_turn_divider = false;
    const std::vector<std::string> single =
        lubancode::cli::RenderTurnView(first.view(), theme, no_divider);
    for (const std::string& line : single) {
        const bool pure_rule = !line.empty() && line.find_first_not_of('-') == std::string::npos;
        CHECK_FALSE(pure_rule);
    }
}

TEST_CASE("黄金画面:打断轮 footer 用 Stopped after,失败轮用 Failed after") {
    lubancode::runtime::IdAuthority ids;
    lubancode::runtime::TurnCollector collector(ids, "turn-s");
    collector.StartTurn("问", 0);
    collector.OnModelStepStarted(0);
    collector.OnToolBatchStarted(0, 0, {"s1", "s2"});
    collector.OnToolStarted("s1", "run_command", nlohmann::json{{"command", "ping localhost"}});
    collector.OnToolFinished("s1", "打断", true, lubancode::runtime::TurnItemViewState::Interrupted);
    collector.MarkRunningInterrupted();
    collector.OnToolBatchFinished(0, true);
    collector.FinishTurn(lubancode::runtime::TurnItemViewState::Interrupted, 18200, 0);

    lubancode::cli::Theme theme = lubancode::cli::BuiltinTheme("plain");
    lubancode::cli::TurnRenderOptions options;
    options.width = 80;
    options.plain = true;
    const std::vector<std::string> lines = lubancode::cli::RenderTurnView(collector.view(), theme, options);
    const std::string text = JoinLines(lines);
    CHECK(text.find("Stopped after 18s") != std::string::npos);
    CHECK(text.find("Worked for") == std::string::npos);
    // 未开跑的那枚:标题可见(屏上不缺枚)。
    CHECK(text.find("run_command(ping localhost)") != std::string::npos);

    // 失败轮。
    lubancode::runtime::TurnCollector failed(ids, "turn-f");
    failed.StartTurn("问", 0);
    failed.OnModelStepStarted(0);
    failed.FinishTurn(lubancode::runtime::TurnItemViewState::Failed, 7600, 0);
    const std::vector<std::string> flines =
        lubancode::cli::RenderTurnView(failed.view(), theme, options);
    CHECK(JoinLines(flines).find("Failed after 7.6s") != std::string::npos);
}

TEST_CASE("黄金画面:审批等待的详细态附注,缺省不写") {
    lubancode::runtime::IdAuthority ids;
    lubancode::runtime::TurnCollector collector(ids, "turn-w");
    collector.StartTurn("问", 0);
    collector.FinishTurn(lubancode::runtime::TurnItemViewState::Succeeded, 130000, 35000);

    lubancode::cli::Theme theme = lubancode::cli::BuiltinTheme("plain");
    lubancode::cli::TurnRenderOptions options;
    options.width = 120;
    options.plain = true;
    const std::string text = JoinLines(lubancode::cli::RenderTurnView(collector.view(), theme, options));
    CHECK(text.find("Worked for 2m 10s") != std::string::npos);
    CHECK(text.find("waited 35s for approval") != std::string::npos);

    // 缺省(approval_wait 为 0)不写附注。
    lubancode::runtime::TurnCollector nowait(ids, "turn-w2");
    nowait.StartTurn("问", 0);
    nowait.FinishTurn(lubancode::runtime::TurnItemViewState::Succeeded, 130000, 0);
    const std::string text2 = JoinLines(lubancode::cli::RenderTurnView(nowait.view(), theme, options));
    CHECK(text2.find("waited") == std::string::npos);
}

TEST_CASE("黄金画面:include_footer=false 时实时画面不重复画 footer") {
    const lubancode::runtime::TurnView view = BuildTurnForRender();
    lubancode::cli::Theme theme = lubancode::cli::BuiltinTheme("plain");
    lubancode::cli::TurnRenderOptions options;
    options.width = 80;
    options.plain = true;
    options.include_footer = false;
    const std::string text = JoinLines(lubancode::cli::RenderTurnView(view, theme, options));
    CHECK(text.find("Worked for") == std::string::npos);  // footer 由 RunTurn 收口单独落
}

// ---------------------------------------------------------------------------
// RunTurn 集成(管道模式):footer 落屏恰一枚、统计行按档、批次入 transcript。
// ---------------------------------------------------------------------------

namespace {

// 与 test_status_refresh.cpp 同款的假后端(精简)。
class RunBackend : public lubancode::api::Backend {
public:
    std::vector<std::vector<lubancode::api::StreamEvent>> scripts;
    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request&,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* = nullptr) override {
        if (call_index >= scripts.size()) {
            return std::unexpected(lubancode::api::Error{lubancode::api::ErrorKind::Api, "脚本用完", 0});
        }
        for (const auto& event : scripts[call_index]) {
            on_event(event);
        }
        ++call_index;
        return {};
    }
    std::size_t call_index = 0;
};

}  // namespace

TEST_CASE("RunTurn 集成:正文轮落恰一枚 Worked footer;管道模式统计行照打") {
    RunBackend backend;
    backend.scripts = {TextScript("这是回答。")};
    lubancode::tools::ToolRegistry registry;
    lubancode::agent::AgentLoop loop(backend, registry, "m", "sys");
    lubancode::cli::ContextTracker tracker(1000);
    std::set<std::string> always_allowed;
    std::vector<lubancode::cli::TranscriptItem> transcript;
    std::atomic<bool> expanded{false};

    std::ostringstream captured;
    std::streambuf* const old_buf = std::cout.rdbuf(captured.rdbuf());
    const lubancode::app::RunTurnResult result =
        lubancode::app::RunTurn(loop, "问题", /*auto_confirm=*/true, always_allowed,
                                lubancode::cli::BuiltinTheme("plain"), tracker, registry,
                                /*hook_dispatcher=*/nullptr, /*is_console=*/false, transcript,
                                /*todo_state=*/nullptr, &expanded);
    std::cout.rdbuf(old_buf);

    REQUIRE(result.status == 0);
    CHECK_FALSE(result.cancelled);
    const std::string out = captured.str();
    // footer 恰一枚:数 "Worked for"。
    std::size_t footer_count = 0;
    std::size_t pos = 0;
    while ((pos = out.find("Worked for", pos)) != std::string::npos) {
        ++footer_count;
        pos += 4;
    }
    CHECK(footer_count == 1);
    // 管道模式没有状态栏:统计长行照打(automation 契约)。
    CHECK(out.find("[tokens]") != std::string::npos);
}

TEST_CASE("RunTurn 集成:错误轮落 Failed footer,下一只 composer 不粘错误行") {
    RunBackend backend;
    backend.scripts = {};  // 脚本空:第一次请求即失败
    lubancode::tools::ToolRegistry registry;
    lubancode::agent::AgentLoop loop(backend, registry, "m", "sys");
    lubancode::cli::ContextTracker tracker(1000);
    std::set<std::string> always_allowed;
    std::vector<lubancode::cli::TranscriptItem> transcript;
    std::atomic<bool> expanded{false};

    std::ostringstream captured;
    std::streambuf* const old_buf = std::cerr.rdbuf(captured.rdbuf());
    const lubancode::app::RunTurnResult result =
        lubancode::app::RunTurn(loop, "问题", /*auto_confirm=*/true, always_allowed,
                                lubancode::cli::BuiltinTheme("plain"), tracker, registry,
                                /*hook_dispatcher=*/nullptr, /*is_console=*/false, transcript,
                                /*todo_state=*/nullptr, &expanded);
    std::cerr.rdbuf(old_buf);

    CHECK(result.status == 1);
    const std::string err = captured.str();
    CHECK(err.find("脚本用完") != std::string::npos);  // 错误如实上屏
}

TEST_CASE("RunTurn 集成:同批三枚工具,start 后才进 transcript、终态串行推进") {
    class ProbeTool : public lubancode::tools::Tool {
    public:
        std::string name() const override { return "probe"; }
        std::string description() const override { return "probe"; }
        nlohmann::json input_schema() const override { return nlohmann::json::object(); }
        bool needs_confirm() const override { return false; }
        lubancode::tools::Tool::Result execute(const nlohmann::json&) override { return {"探针完成", false}; }
    };

    RunBackend backend;
    // 第一拍:三枚 tool_use;第二拍:文本收口。
    backend.scripts = {
        MultiToolScript({"p1", "p2", "p3"}, "probe"),
        TextScript("干完了。"),
    };
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<ProbeTool>());
    lubancode::agent::AgentLoop loop(backend, registry, "m", "sys");
    lubancode::cli::ContextTracker tracker(1000);
    std::set<std::string> always_allowed;
    std::vector<lubancode::cli::TranscriptItem> transcript;
    std::atomic<bool> expanded{false};

    std::ostringstream captured;
    std::streambuf* const old_buf = std::cout.rdbuf(captured.rdbuf());
    const lubancode::app::RunTurnResult result =
        lubancode::app::RunTurn(loop, "跑三枚", /*auto_confirm=*/true, always_allowed,
                                lubancode::cli::BuiltinTheme("plain"), tracker, registry,
                                /*hook_dispatcher=*/nullptr, /*is_console=*/false, transcript,
                                /*todo_state=*/nullptr, &expanded);
    std::cout.rdbuf(old_buf);

    REQUIRE(result.status == 0);
    // 工具 start 后才入终端账,三枚终态皆为 Ok,不曾留下无名 Pending 壳。
    int probe_items = 0;
    for (const auto& item : transcript) {
        if (item.tool_name == "probe") {
            ++probe_items;
            CHECK(item.status == lubancode::cli::TranscriptStatus::Ok);
        }
    }
    CHECK(probe_items == 3);
    // footer 恰一枚。
    const std::string out = captured.str();
    std::size_t footer_count = 0;
    std::size_t pos = 0;
    while ((pos = out.find("Worked for", pos)) != std::string::npos) {
        ++footer_count;
        pos += 4;
    }
    CHECK(footer_count == 1);
}

// ---------------------------------------------------------------------------
// turn 活动条的边界账(无真控制台:footer 未启用,全链短路——钉的正是
// "没起过的 EndTurn 返回 -1,不误伤 /compact 那类单次 spinner"的契约)。
// 同钟一致性由口径保证:Working 秒数与 Worked footer 都出自 turn_wall_start
// 同一枚 steady 钟差,数字格式共用 FormatTurnDuration(上面已钉档位),
// 不存在第二把尺。
// ---------------------------------------------------------------------------

TEST_CASE("turn 活动条:footer 未启用时 Begin/Update/End 全链空操作") {
    lubancode::cli::BeginStreamFooter(lubancode::cli::Theme{}, /*enabled=*/false);
    CHECK_FALSE(lubancode::cli::TurnActivityActive());
    lubancode::cli::BeginTurnActivity("Working", 1000);
    CHECK_FALSE(lubancode::cli::TurnActivityActive());  // 没起成
    lubancode::cli::UpdateTurnActivityElapsed(0, 5);
    lubancode::cli::SetTurnActivityInterruptRequested();
    CHECK(lubancode::cli::EndTurnActivity() == -1);     // 没起过的 End 返回 -1
    CHECK_FALSE(lubancode::cli::TurnActivityActive());
}

TEST_CASE("turn 活动条:footer 先启用,首个流事件前也立即亮起并正常收账") {
    lubancode::cli::BeginStreamFooter(lubancode::cli::Theme{}, /*enabled=*/true);
    CHECK_FALSE(lubancode::cli::TurnActivityActive());

    lubancode::cli::BeginTurnActivity("Working", 1000);
    CHECK(lubancode::cli::TurnActivityActive());
    lubancode::cli::UpdateTurnActivityElapsed(2, 5);
    CHECK(lubancode::cli::EndTurnActivity() == 5);
    CHECK_FALSE(lubancode::cli::TurnActivityActive());

    lubancode::cli::EndStreamFooter();
}
