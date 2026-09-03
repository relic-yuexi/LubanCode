// 主/Subagent 面板工具调用同构渲染单:查看态(子代理面板)的会话投影。
// BuildAgentTaskBlocks 把事件账折成会话块(当前查看代理自己的工具一律
// Tool,不再无条件 SubTool),渲染交给 cli::RenderSessionBlocks——同一颗
// 紧凑/详细开关统管思考与工具,块间空白走间距表。这里钉三桩:
//   1. 投影坐标:查看根自己的工具顶格(Tool),不缩四格、不挤成一串;
//   2. 开关统管:Ctrl+O 展开档同时铺开思考全文与工具参数,紧凑档都收;
//   3. 端到端:AgentPanelPresenter::TaskTranscriptLines 走同一投影,画面
//      与纯函数版一致(面板只换数据源,不换 renderer)。

#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "app/agent_panel_presenter.hpp"
#include "cli/i18n.hpp"  // SetLanguage
#include "cli/line_editor.hpp"  // DisplayWidthUtf8
#include "cli/theme.hpp"
#include "cli/transcript.hpp"
#include "tools/agent_tool.hpp"
#include "tools/task_ledger.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 台账测试用假后端:send_stream 不会被调到(不走 RunTask,直写事件账)。
class StubBackend : public api::Backend {
public:
    std::expected<void, api::Error> send_stream(
        const api::Request&,
        const std::function<void(const api::StreamEvent&)>&,
        const std::atomic<bool>* = nullptr) override {
        return std::unexpected(api::Error{api::ErrorKind::Api, "StubBackend: 不发请求", 0});
    }
};

tools::AgentTaskEvent MakeEvent(tools::AgentTaskEventKind kind) {
    tools::AgentTaskEvent event;
    event.kind = kind;
    return event;
}

tools::AgentTaskEvent ToolStartEvent(const std::string& name, const std::string& input_json,
                                     const std::string& tool_use_id, const std::string& step_id = std::string()) {
    auto event = MakeEvent(tools::AgentTaskEventKind::ToolStart);
    event.tool_name = name;
    event.input_json = input_json;
    event.tool_use_id = tool_use_id;
    event.step_id = step_id;
    return event;
}

tools::AgentTaskEvent ToolResultEvent(const std::string& name, const std::string& result,
                                      const std::string& tool_use_id, bool is_error = false,
                                      tools::AgentTaskToolStatus status = tools::AgentTaskToolStatus::None) {
    auto event = MakeEvent(tools::AgentTaskEventKind::ToolResult);
    event.tool_name = name;
    event.result = result;
    event.tool_use_id = tool_use_id;
    event.is_error = is_error;
    event.tool_status = status;
    return event;
}

tools::AgentTaskEvent ReasoningEvent(const std::string& text, bool streaming) {
    auto event = MakeEvent(tools::AgentTaskEventKind::AssistantReasoning);
    event.text = text;
    event.streaming = streaming;
    return event;
}

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string joined;
    for (const auto& line : lines) {
        joined += line + "\n";
    }
    return joined;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// 投影坐标(P0 §3.1):查看根自己的工具是这张面板的顶层 Tool。
// ---------------------------------------------------------------------------

TEST_CASE("投影:查看根自己的工具投 Tool,不缩四格;紧凑档不隐藏它们") {
    cli::SetLanguage("zh");
    const auto theme = cli::BuiltinTheme("plain");
    const std::vector<tools::AgentTaskEvent> events{
        ToolStartEvent("run_command", R"({"command":"git log"})", "t1"),
        ToolResultEvent("run_command", "[退出码 0]\n第一行", "t1"),
        ToolStartEvent("read_file", R"({"path":"a.txt"})", "t2"),
        ToolResultEvent("read_file", "1  hi", "t2"),
    };
    const auto blocks = app::BuildAgentTaskBlocks(events, theme);
    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks[0].kind == cli::SessionBlock::Kind::Items);
    REQUIRE(blocks[0].items.size() == 2);
    for (const auto& item : blocks[0].items) {
        CHECK(item.kind == cli::TranscriptKind::Tool);  // 相对查看根:顶层
    }

    // 紧凑档:FormatTranscriptItems 只收 SubTool;Tool 全在。
    const auto compact = cli::RenderSessionBlocks(blocks, theme, 100, /*expanded=*/false);
    const std::string joined = JoinLines(compact);
    CHECK(Contains(joined, "[OK] run_command(git log)"));
    CHECK(Contains(joined, "[OK] read_file(a.txt)"));
    CHECK(joined.find("    [OK]") == std::string::npos);  // 没有四格缩进的卡
    // 两枚顶层工具之间恰一枚空行——不再"挤成一串"。
    CHECK(Contains(joined, "\n\n[OK] read_file(a.txt)"));
}

TEST_CASE("投影:思考与工具受同一颗开关统管,与 Main 的 Ctrl+O 同规矩") {
    cli::SetLanguage("zh");
    const auto theme = cli::BuiltinTheme("plain");
    const std::vector<tools::AgentTaskEvent> events{
        ReasoningEvent("先想第一步\n再想第二步", /*streaming=*/false),
        ToolStartEvent("search", R"({"query":"根节点"})", "t1"),
        ToolResultEvent("search", "命中 2 处", "t1"),
    };
    const auto blocks = app::BuildAgentTaskBlocks(events, theme);
    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks[0].items.size() == 2);
    CHECK(blocks[0].items[0].kind == cli::TranscriptKind::Thinking);
    CHECK(blocks[0].items[1].kind == cli::TranscriptKind::Tool);

    // 紧凑档:思考只留标题一行,工具不露参数。
    const auto compact = JoinLines(cli::RenderSessionBlocks(blocks, theme, 100, false));
    CHECK(Contains(compact, "思考"));
    CHECK_FALSE(Contains(compact, "先想第一步"));
    CHECK_FALSE(Contains(compact, "参数:"));

    // 详细档:思考全文与工具参数同时铺出——一颗开关管两样。
    const auto expanded = JoinLines(cli::RenderSessionBlocks(blocks, theme, 100, true));
    CHECK(Contains(expanded, "先想第一步"));
    CHECK(Contains(expanded, "参数: {\"query\":\"根节点\"}"));
}

TEST_CASE("投影:流尾未收口的工具留 Running 卡,正文/介入/检查点各成块且顺序不乱") {
    cli::SetLanguage("zh");
    const auto theme = cli::BuiltinTheme("plain");
    std::vector<tools::AgentTaskEvent> events{
        MakeEvent(tools::AgentTaskEventKind::UserMessage),
        ReasoningEvent("想了想", false),
        ToolStartEvent("run_command", R"({"command":"make"})", "t9"),  // 没等来结果
        MakeEvent(tools::AgentTaskEventKind::CompactCheckpoint),
        MakeEvent(tools::AgentTaskEventKind::Completion),
    };
    events[0].text = "查一下构建";
    events[4].text = "收工结论";
    const auto blocks = app::BuildAgentTaskBlocks(events, theme);
    // 块序:用户(markdown)-> 思考+Running 工具(items)-> 压缩(notice)-> 终局(markdown)。
    REQUIRE(blocks.size() == 4);
    CHECK(blocks[0].kind == cli::SessionBlock::Kind::Markdown);
    CHECK(blocks[0].role == cli::BlockRole::UserPrompt);
    CHECK(blocks[1].kind == cli::SessionBlock::Kind::Items);
    REQUIRE(blocks[1].items.size() == 2);
    CHECK(blocks[1].items[0].kind == cli::TranscriptKind::Thinking);
    CHECK(blocks[1].items[1].status == cli::TranscriptStatus::Running);  // 不吞卡
    CHECK(blocks[1].items[1].kind == cli::TranscriptKind::Tool);
    CHECK(blocks[2].kind == cli::SessionBlock::Kind::Notice);
    CHECK(blocks[3].kind == cli::SessionBlock::Kind::Markdown);
    CHECK(blocks[3].role == cli::BlockRole::TurnFooter);

    const auto joined = JoinLines(cli::RenderSessionBlocks(blocks, theme, 100, false));
    CHECK(Contains(joined, "> 你"));
    CHECK(Contains(joined, "查一下构建"));
    CHECK(Contains(joined, "[RUNNING] run_command(make)"));
    CHECK(Contains(joined, "上下文压缩"));
    CHECK(Contains(joined, "收工结论"));
}

// ---------------------------------------------------------------------------
// 端到端:presenter 的查看页走同一投影(面板只换数据源,不换 renderer)。
// ---------------------------------------------------------------------------

TEST_CASE("端到端:TaskTranscriptLines 的工具顶格、留口气,展开档铺参数") {
    cli::SetLanguage("zh");
    StubBackend backend;
    tools::ToolRegistry registry;
    auto agent_tool = std::make_unique<tools::AgentTool>(backend, registry, "/work/dir");
    auto task = agent_tool->ledger().Register(tools::AgentTaskSnapshot{});
    task->snapshot.agent_type = "general-purpose";
    task->snapshot.title = "查构建";
    task->snapshot.prompt = "查一下构建";
    {
        std::lock_guard<std::mutex> lock(agent_tool->ledger().mutex);
        auto& ledger = agent_tool->ledger();
        ledger.AppendEventLocked(task, MakeEvent(tools::AgentTaskEventKind::UserMessage));
        ledger.AppendEventLocked(task, ToolStartEvent("run_command", R"({"command":"git log"})", "t1"));
        ledger.AppendEventLocked(task, ToolResultEvent("run_command", "[退出码 0]\n第一行", "t1"));
        ledger.AppendEventLocked(task, ToolStartEvent("read_file", R"({"path":"a.txt"})", "t2"));
        ledger.AppendEventLocked(task, ToolResultEvent("read_file", "1  hi", "t2"));
    }
    agent_tool->ledger().Touch();

    // presenter 持有 Theme 的引用,须给具名长命对象(临时量悬垂)。
    const cli::Theme view_theme = cli::BuiltinTheme("plain");
    app::AgentPanelPresenter presenter(view_theme);
    const auto compact = JoinLines(presenter.TaskTranscriptLines(agent_tool.get(), task->snapshot.id, 100, false));
    CHECK(Contains(compact, "[OK] run_command(git log)"));
    CHECK(Contains(compact, "[OK] read_file(a.txt)"));
    CHECK(compact.find("    [OK]") == std::string::npos);  // 不再四格缩进
    CHECK(Contains(compact, "\n\n[OK] read_file(a.txt)"));  // 恰一口(垫在前卡摘要行后)
    CHECK(Contains(compact, "── 压缩") == false);

    // 展开档(agent_view_expanded):参数铺出。
    const auto expanded =
        JoinLines(presenter.TaskTranscriptLines(agent_tool.get(), task->snapshot.id, 100, true));
    CHECK(Contains(expanded, "参数: {\"command\":\"git log\"}"));
    CHECK(Contains(expanded, "参数: {\"path\":\"a.txt\"}"));

    // 事件账不被查看改写:再看一遍,画面一致。
    const auto again = JoinLines(presenter.TaskTranscriptLines(agent_tool.get(), task->snapshot.id, 100, false));
    CHECK(again == compact);
}

// ---------------------------------------------------------------------------
// 工具配对(P1 §7.2):按 tool_use_id 对账;缺/重/迟到都有稳定投影。
// ---------------------------------------------------------------------------

TEST_CASE("配对:start(A), start(B), result(B), result(A) 按 id 各归各,不串结果") {
    cli::SetLanguage("zh");
    const auto theme = cli::BuiltinTheme("plain");
    const std::vector<tools::AgentTaskEvent> events{
        ToolStartEvent("run_command", R"({"command":"make a"})", "A"),
        ToolStartEvent("run_command", R"({"command":"make b"})", "B"),
        ToolResultEvent("run_command", "B 的结果", "B"),
        ToolResultEvent("run_command", "A 的结果", "A"),
    };
    const auto blocks = app::BuildAgentTaskBlocks(events, theme);
    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks[0].items.size() == 2);
    // 卡落在各自 start 的位置(id 原位收口),结果按 id 归位。
    CHECK(blocks[0].items[0].title == "run_command(make a)");
    CHECK(blocks[0].items[0].status == cli::TranscriptStatus::Ok);
    CHECK(blocks[0].items[0].summary_lines[0] == "A 的结果");
    CHECK(blocks[0].items[1].title == "run_command(make b)");
    CHECK(blocks[0].items[1].summary_lines[0] == "B 的结果");
}

TEST_CASE("配对:缺 start、缺 result、重复 result 都不吞卡不误伤旁卡") {
    cli::SetLanguage("zh");
    const auto theme = cli::BuiltinTheme("plain");
    const std::vector<tools::AgentTaskEvent> events{
        ToolStartEvent("search", R"({"query":"x"})", "A"),          // 没等来 result
        ToolStartEvent("read_file", R"({"path":"a"})", "B"),
        ToolResultEvent("read_file", "读到 3 行", "B"),
        ToolResultEvent("read_file", "读到 3 行", "B"),             // 重复 result:不二开卡
        ToolResultEvent("search", "迟到的结果", "孤儿"),             // 缺 start:只有结果的卡
    };
    const auto blocks = app::BuildAgentTaskBlocks(events, theme);
    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks[0].items.size() == 3);
    // A:Running 卡(不吞);B:一张终态卡(重复 result 不二开);孤儿:结果卡。
    CHECK(blocks[0].items[0].status == cli::TranscriptStatus::Running);
    CHECK(blocks[0].items[1].status == cli::TranscriptStatus::Ok);
    CHECK(blocks[0].items[1].summary_lines[0] == "读到 3 行");
    CHECK(blocks[0].items[2].title == "search({})");  // 伪 start 清了入参,空对象走通用摘要
    CHECK(blocks[0].items[2].summary_lines[0] == "迟到的结果");
}

TEST_CASE("终态细分:中断不冒充失败,拒绝/跳过灰灯,旧账按 is_error 折") {
    cli::SetLanguage("zh");
    const auto theme = cli::BuiltinTheme("plain");
    const std::vector<tools::AgentTaskEvent> events{
        ToolStartEvent("run_command", R"({"command":"a"})", "t1"),
        ToolResultEvent("run_command", "(工具被取消,结果不明)", "t1", /*is_error=*/true,
                        tools::AgentTaskToolStatus::Interrupted),
        ToolStartEvent("write_file", R"({"path":"b"})", "t2"),
        ToolResultEvent("write_file", std::string(), "t2", false, tools::AgentTaskToolStatus::Declined),
        ToolStartEvent("search", R"({"query":"c"})", "t3"),
        ToolResultEvent("search", "错了", "t3", /*is_error=*/true),  // 旧账:无细分
    };
    const auto blocks = app::BuildAgentTaskBlocks(events, theme);
    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks[0].items.size() == 3);
    CHECK(blocks[0].items[0].status == cli::TranscriptStatus::Interrupted);
    CHECK(cli::TranscriptStatusWord(blocks[0].items[0].status) == "[INTERRUPTED]");
    CHECK(blocks[0].items[1].status == cli::TranscriptStatus::Cancelled);
    CHECK(blocks[0].items[1].summary_lines[0] == "(工具被拒绝执行)");
    CHECK(blocks[0].items[2].status == cli::TranscriptStatus::Error);  // None 退 is_error
}

TEST_CASE("step 边界:同 step 工具成批,换 step 冲组留轻间隔") {
    cli::SetLanguage("zh");
    const auto theme = cli::BuiltinTheme("plain");
    const std::vector<tools::AgentTaskEvent> events{
        ToolStartEvent("run_command", R"({"command":"a"})", "A", "step-0"),
        ToolStartEvent("run_command", R"({"command":"b"})", "B", "step-0"),
        ToolResultEvent("run_command", "A 完成", "A"),
        ToolResultEvent("run_command", "B 完成", "B"),
        ToolStartEvent("search", R"({"query":"c"})", "C", "step-1"),  // 下一次模型响应
        ToolResultEvent("search", "C 完成", "C"),
    };
    const auto blocks = app::BuildAgentTaskBlocks(events, theme);
    // 换 step 冲组:两个 Items 块,块间由间距表垫一口气。
    REQUIRE(blocks.size() == 2);
    REQUIRE(blocks[0].items.size() == 2);
    REQUIRE(blocks[1].items.size() == 1);
    const auto joined = JoinLines(cli::RenderSessionBlocks(blocks, theme, 100, false));
    CHECK(Contains(joined, "B 完成\n\n[OK] search("));  // step 间恰一口
    // 同 step 两枚之间也是一口(Tool->Tool=1,与 Main 同表)。
    CHECK(Contains(joined, "A 完成\n\n[OK] run_command(b)"));
}

TEST_CASE("legacy 兼容投影:无 id 旧账按事件顺序相邻配对,账面顶部标明") {
    cli::SetLanguage("zh");
    const auto theme = cli::BuiltinTheme("plain");
    const std::vector<tools::AgentTaskEvent> events{
        ToolStartEvent("run_command", R"({"command":"a"})", ""),
        ToolStartEvent("read_file", R"({"path":"x"})", ""),
        ToolResultEvent("read_file", "读到 1 行", ""),
        ToolResultEvent("run_command", "[退出码 0]", ""),
    };
    const auto blocks = app::BuildAgentTaskBlocks(events, theme);
    REQUIRE(blocks.size() == 2);
    // 顶部:legacy 诊断明账(不悄悄猜)。
    CHECK(blocks[0].kind == cli::SessionBlock::Kind::Notice);
    CHECK(Contains(blocks[0].line, "legacy_unstructured_transcript"));
    // 相邻配对:同名优先,交错也能各归各。
    REQUIRE(blocks[1].items.size() == 2);
    CHECK(blocks[1].items[0].title == "run_command(a)");
    CHECK(blocks[1].items[0].summary_lines[0] == "[退出码 0]");
    CHECK(blocks[1].items[1].title == "read_file(x)");
}

// ---------------------------------------------------------------------------
// 面板同构(P1 收拢/P2):同一份事实,Main/Subagent 一个画面;执行模式与
// 终态不改画法;宽度不越界。
// ---------------------------------------------------------------------------

TEST_CASE("同构:同一份会话事实,Main 条目组与 Subagent 查看页逐字一致") {
    cli::SetLanguage("zh");
    const auto theme = cli::BuiltinTheme("plain");
    const std::vector<tools::AgentTaskEvent> events{
        ReasoningEvent("想一想", false),
        ToolStartEvent("run_command", R"({"command":"git log"})", "t1"),
        ToolResultEvent("run_command", "[退出码 0]\n第一行", "t1"),
    };
    // Main 侧:同一事实拼成的条目组(顶层 Tool)。
    std::vector<cli::TranscriptItem> main_items;
    main_items.push_back(cli::MakeAgentTaskThinkingItem(1, "想一想", false));
    main_items.push_back(cli::MakeAgentTaskToolItem(2, "run_command", R"({"command":"git log"})", true, false,
                                                    "[退出码 0]\n第一行", cli::TranscriptKind::Tool));
    const std::string main_render = cli::FormatTranscriptItems(main_items, theme, 90, false);
    // Subagent 侧:事件账 -> 会话块 -> 共用 renderer。
    const auto blocks = app::BuildAgentTaskBlocks(events, theme);
    const std::string sub_render = JoinLines(cli::RenderSessionBlocks(blocks, theme, 90, false));
    // 去掉身份头之后,块类型、层级、间距、状态词、摘要一字不差。
    REQUIRE(blocks.size() == 1);
    CHECK(sub_render == main_render);
}

TEST_CASE("执行模式不改画法:前台/后台任务的同一份事件账投影一致") {
    cli::SetLanguage("zh");
    const auto theme = cli::BuiltinTheme("plain");
    const std::vector<tools::AgentTaskEvent> events{
        ToolStartEvent("run_command", R"({"command":"a"})", "t1"),
        ToolResultEvent("run_command", "结果", "t1"),
    };
    StubBackend backend;
    tools::ToolRegistry registry;
    auto agent_tool = std::make_unique<tools::AgentTool>(backend, registry, "/work/dir");
    // 同一份事件账,两只任务:一只前台、一只后台。
    auto foreground = agent_tool->ledger().Register(tools::AgentTaskSnapshot{});
    foreground->snapshot.foreground = true;
    foreground->snapshot.state = tools::AgentTaskState::Done;
    auto background = agent_tool->ledger().Register(tools::AgentTaskSnapshot{});
    background->snapshot.foreground = false;
    background->snapshot.state = tools::AgentTaskState::Done;
    for (const auto& record : {foreground, background}) {
        std::lock_guard<std::mutex> lock(agent_tool->ledger().mutex);
        for (const auto& event : events) {
            agent_tool->ledger().AppendEventLocked(record, event);
        }
    }
    app::AgentPanelPresenter presenter(theme);
    const auto fg = JoinLines(presenter.TaskTranscriptLines(agent_tool.get(), foreground->snapshot.id, 90, false));
    const auto bg = JoinLines(presenter.TaskTranscriptLines(agent_tool.get(), background->snapshot.id, 90, false));
    // 会话正文(自首枚工具卡起)一个字不差;只有身份头([前台]/[后台]、
    // 任务号)有别——执行模式不得改变 panel 画法。
    const auto body_from = [](const std::string& text) {
        return text.substr(text.find("[OK] run_command(a)"));
    };
    CHECK(body_from(fg) == body_from(bg));
    CHECK(Contains(fg, "[前台]"));
    CHECK(Contains(bg, "[后台]"));
    CHECK(Contains(fg, "[OK] run_command(a)"));
    CHECK(Contains(bg, "[OK] run_command(a)"));
}

TEST_CASE("终态重放:任务完成后退出 Dock 再查看,画面仍是整段会话") {
    cli::SetLanguage("zh");
    StubBackend backend;
    tools::ToolRegistry registry;
    auto agent_tool = std::make_unique<tools::AgentTool>(backend, registry, "/work/dir");
    auto task = agent_tool->ledger().Register(tools::AgentTaskSnapshot{});
    task->snapshot.title = "完工任务";
    task->snapshot.state = tools::AgentTaskState::Done;
    task->snapshot.result = "最终结论一句话";
    {
        std::lock_guard<std::mutex> lock(agent_tool->ledger().mutex);
        auto& ledger = agent_tool->ledger();
        auto completion = MakeEvent(tools::AgentTaskEventKind::Completion);
        completion.text = "最终结论一句话";
        ledger.AppendEventLocked(task, ToolStartEvent("run_command", R"({"command":"a"})", "t1"));
        ledger.AppendEventLocked(task, ToolResultEvent("run_command", "工具结果", "t1"));
        ledger.AppendEventLocked(task, completion);
    }
    // presenter 持有 Theme 的引用,须给具名长命对象(临时量悬垂)。
    const cli::Theme view_theme = cli::BuiltinTheme("plain");
    app::AgentPanelPresenter presenter(view_theme);
    const auto lines = presenter.TaskTranscriptLines(agent_tool.get(), task->snapshot.id, 90, false);
    const auto joined = JoinLines(lines);
    CHECK(Contains(joined, "[OK] run_command(a)"));   // 工具卡还在
    CHECK(Contains(joined, "工具结果"));
    CHECK(Contains(joined, "最终结论一句话"));         // 终局结论也在,不只剩一句
}

TEST_CASE("宽度:20/40/80/120 列不越界,宽字符不撑破卡片") {
    cli::SetLanguage("zh");
    const auto theme = cli::BuiltinTheme("plain");
    const std::vector<tools::AgentTaskEvent> events{
        ToolStartEvent("run_command", R"({"command":"git log --oneline --graph --decorate --all"})", "t1"),
        ToolResultEvent("run_command", "一行特别长的中文结果,看看会不会被截断到越界之外去", "t1"),
        ReasoningEvent("思考正文也来一段足够长的汉字,凑够折叠与截断的宽度", false),
    };
    const auto blocks = app::BuildAgentTaskBlocks(events, theme);
    for (const int width : {20, 40, 80, 120}) {
        const auto lines = cli::RenderSessionBlocks(blocks, theme, width, false);
        for (const auto& line : lines) {
            if (line.find('\x1b') == std::string::npos) {
                CHECK(cli::DisplayWidthUtf8(line) <= static_cast<std::size_t>(width));
            }
        }
        // 同一份数据,重铺(改宽)前后内容同源——不因重建丢卡。
        const auto wide = cli::RenderSessionBlocks(blocks, theme, 120, false);
        CHECK(JoinLines(wide).find("run_command(") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// 间距钉子(底栏键贴场景单 P1-2):Subagent 查看页的连续工具卡恰留一口——
// 终态卡/Running 卡/思考卡混排都走同一张 GapBetween 表,不因"没有结果"
// 或"step 冲组"挤成一串,也不双打。
// ---------------------------------------------------------------------------

TEST_CASE("间距:连续两枚 Tool 之间恰一口——终态卡、Running 卡、思考前各场景") {
    cli::SetLanguage("zh");
    const auto theme = cli::BuiltinTheme("plain");

    // 终态两枚:间隔垫在前卡摘要行之后(同构渲染单已立,这里钉死口径)。
    {
        const std::vector<tools::AgentTaskEvent> events{
            ToolStartEvent("run_command", R"({"command":"git log"})", "A", "step-0"),
            ToolResultEvent("run_command", "[退出码 0]", "A"),
            ToolStartEvent("read_file", R"({"path":"a.txt"})", "B", "step-0"),
            ToolResultEvent("read_file", "1  hi", "B"),
        };
        const auto joined =
            JoinLines(cli::RenderSessionBlocks(app::BuildAgentTaskBlocks(events, theme), theme, 100, false));
        CHECK(Contains(joined, "[退出码 0]\n\n[OK] read_file(a.txt)"));  // 恰一口
        CHECK(joined.find("\n\n\n") == std::string::npos);               // 不双打
    }

    // Running 两枚(结果未到、没有摘要行):卡与卡之间仍恰一口,不挤一起。
    {
        const std::vector<tools::AgentTaskEvent> events{
            ToolStartEvent("web_search", R"({"query":"x"})", "A", "step-0"),
            ToolStartEvent("read_file", R"({"path":"a.txt"})", "B", "step-0"),
        };
        const auto joined =
            JoinLines(cli::RenderSessionBlocks(app::BuildAgentTaskBlocks(events, theme), theme, 100, false));
        CHECK(Contains(joined, "web_search(x)\n\n[RUNNING] read_file(a.txt)"));
        CHECK(joined.find("\n\n\n") == std::string::npos);
    }

    // 思考 -> 工具:收定的思考卡与下一枚工具卡之间恰一口(与主面板同表)。
    {
        const std::vector<tools::AgentTaskEvent> events{
            ReasoningEvent("琢磨了一下", false),
            ToolStartEvent("run_command", R"({"command":"git log"})", "A", "step-0"),
            ToolResultEvent("run_command", "[退出码 0]", "A"),
        };
        const auto joined =
            JoinLines(cli::RenderSessionBlocks(app::BuildAgentTaskBlocks(events, theme), theme, 100, false));
        const std::size_t thinking_at = joined.find("思考");  // 收定思考卡,标题「思考」
        const std::size_t tool_at = joined.find("[OK] run_command(git log)");
        REQUIRE(thinking_at != std::string::npos);
        REQUIRE(tool_at != std::string::npos);
        CHECK(joined.substr(thinking_at, tool_at - thinking_at).find("\n\n") != std::string::npos);
    }
}
