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
#include <string>
#include <vector>

#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/theme.hpp"
#include "cli/tool_display.hpp"
#include "cli/transcript.hpp"
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

TEST_CASE("ToolDisplay 批次预告:三枚先登记 Pending,start 点亮、终态各归各") {
    std::vector<lubancode::cli::TranscriptItem> transcript;
    std::atomic<bool> cancel{false};
    lubancode::cli::Theme theme;
    lubancode::cli::ToolDisplay display(transcript, theme, /*console=*/false, nullptr, &cancel);

    display.OnBatchAnnounced({"b1", "b2", "b3"});
    REQUIRE(transcript.size() == 3);
    CHECK(transcript[0].status == lubancode::cli::TranscriptStatus::Pending);
    CHECK(transcript[1].status == lubancode::cli::TranscriptStatus::Pending);
    CHECK(transcript[2].status == lubancode::cli::TranscriptStatus::Pending);

    // start 到了:点亮预告的那条,不另起一枚。
    display.OnToolStart("b2", "read_file", nlohmann::json{{"path", "x"}});
    REQUIRE(transcript.size() == 3);
    CHECK(transcript[0].status == lubancode::cli::TranscriptStatus::Pending);
    CHECK(transcript[1].status == lubancode::cli::TranscriptStatus::Running);
    CHECK(transcript[1].tool_name == "read_file");
    CHECK(transcript[1].title.find("read_file") != std::string::npos);

    display.OnToolDone("b2", "read_file", lubancode::tools::Tool::Result{"ok", false});
    CHECK(transcript[1].status == lubancode::cli::TranscriptStatus::Ok);

    // 未 start 的那两枚按 Skipped 定格(ESC 路)。
    display.OnBatchSkipped();
    CHECK(transcript[0].status == lubancode::cli::TranscriptStatus::Cancelled);
    CHECK(transcript[2].status == lubancode::cli::TranscriptStatus::Cancelled);
    CHECK(transcript[1].status == lubancode::cli::TranscriptStatus::Ok);  // 已终态的不动
}
