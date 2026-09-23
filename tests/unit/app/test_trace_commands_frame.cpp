// TUI 排版批 5b(tui优化todo.todo:/trace 全族)的输出形状册。
//   - export/failures/errors/裸敲/详细档全进 frame:错误与空态走键值对框
//     (error 档语义色),errors 档一行一枚进表格(exec/tool/outcome/
//     error/ms/rel 列,outcome 失败走 error 色);
//   - 详细档单枚:summary 行 + error_code/source/recovery 键值对框;
//   - plain 主题(--no-color/T3 降级路径)钉零转义字节、无框字形——合同
//     第 3 条;
//   - /trace export 落盘的 JSON 诊断包是文件面不是 stdout 机器面,本批
//     只动 stdout 回执;源码核实 /trace 无 --json/--format 分支。
//
// 走真 HandleTraceCommand(手造 ToolTraceHub + OnTrace 投递真事件,
// TermPort 改道捕获),断言只看形状与相对位置,不看绝对宽度。

#include <doctest/doctest.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "agent/tool_trace.hpp"
#include "app/commands/trace_commands.hpp"
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "runtime/id_authority.hpp"  // IdAuthority:hub 构造的发号局
#include "runtime/tool_trace_hub.hpp"

using namespace lubancode;

namespace {

class OutputCapture {
public:
    OutputCapture() { cli::TermPort().Redirect(&stream_, nullptr); }
    ~OutputCapture() { cli::TermPort().Reset(); }
    std::string text() const { return stream_.str(); }

private:
    std::ostringstream stream_;
};

std::string StripAnsi(const std::string& text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() &&
                   !((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z'))) {
                ++i;
            }
            if (i < text.size()) {
                ++i;  // 吃掉终结字母
            }
            continue;
        }
        out += text[i];
        ++i;
    }
    return out;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

constexpr const char* kBoxLightTopLeft = "\xe2\x94\x8c";  // ┌
constexpr const char* kBoxLightVert = "\xe2\x94\x82";     // │

// 一枚 execution 的三事件(scheduled/started/finished)齐投,execution_id
// 对齐——与 runtime 域 test_tool_trace.cpp 的手造先例同一套。
void FeedExecution(runtime::ToolTraceHub& hub, const std::string& id, const std::string& tool,
                   int seq, agent::ToolOutcome outcome, const std::string& error_code,
                   std::int64_t duration_ms) {
    agent::ToolTraceEvent scheduled;
    scheduled.kind = agent::ToolTraceEventKind::Scheduled;
    scheduled.execution_id = id;
    scheduled.tool_use_id = "u-" + id;
    scheduled.batch_id = "batch-1";
    scheduled.sequence_in_batch = seq;
    scheduled.tool_name = tool;
    hub.OnTrace(scheduled);
    agent::ToolTraceEvent started;
    started.kind = agent::ToolTraceEventKind::ExecutionStarted;
    started.execution_id = id;
    started.tool_use_id = "u-" + id;
    started.batch_id = "batch-1";
    started.sequence_in_batch = seq;
    started.tool_name = tool;
    hub.OnTrace(started);
    agent::ToolTraceEvent finished;
    finished.kind = agent::ToolTraceEventKind::ExecutionFinished;
    finished.execution_id = id;
    finished.tool_use_id = "u-" + id;
    finished.batch_id = "batch-1";
    finished.sequence_in_batch = seq;
    finished.tool_name = tool;
    finished.outcome = outcome;
    finished.error_code = error_code;
    finished.duration_ms = duration_ms;
    hub.OnTrace(finished);
}

struct TraceRig {
    runtime::IdAuthority ids;
    runtime::ToolTraceHub hub{ids};
    app::TraceCommandContext ctx{&hub, nullptr};
    cli::Theme theme{cli::BuiltinTheme("dark")};
    cli::Theme plain_theme{cli::BuiltinTheme("plain")};

    TraceRig() { ctx.theme = &theme; }

    std::string Run(const std::string& args, const cli::Theme& use_theme) {
        ctx.theme = &use_theme;
        OutputCapture capture;
        app::HandleTraceCommand(ctx, args);
        return capture.text();
    }
};

}  // namespace

TEST_CASE("errors 档: 失败枚进表格,成功枚不进;outcome 列 error 语义色") {
    TraceRig rig;
    FeedExecution(rig.hub, "exe-001", "edit_file", 1, agent::ToolOutcome::ToolError,
                  "schema_rejected", 3);
    FeedExecution(rig.hub, "exe-002", "read_file", 2, agent::ToolOutcome::Succeeded, "", 5);
    const std::string out = rig.Run("errors", rig.theme);

    REQUIRE(Contains(out, kBoxLightTopLeft));
    const std::string plain = StripAnsi(out);
    INFO(out);  // 诊断:断言挂时把 errors 档原文打进日志(挂了才打印)
    // 表头:schema 名列头一行齐(exec/tool/outcome/error/ms/rel)。
    CHECK(Contains(plain, "exec"));
    CHECK(Contains(plain, "tool"));
    CHECK(Contains(plain, "outcome"));
    CHECK(Contains(plain, "error"));
    CHECK(Contains(plain, "ms"));
    // 失败枚在,成功枚被滤(errors 只收明确失败与 unknown)。
    CHECK(Contains(plain, "exe-001"));
    CHECK(Contains(plain, "edit_file"));
    CHECK(Contains(plain, "schema_rejected"));
    CHECK(plain.find("exe-002") == std::string::npos);
    // outcome 失败走 error 档(fail 不另立色,批 0 合同)。
    CHECK(Contains(out, rig.theme.error + "tool_error"));
}

TEST_CASE("export 档: --raw 拒/空路径/hub 缺席三态都进 error 框") {
    TraceRig rig;
    {
        const std::string out = rig.Run("export --raw", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "一律脱敏"));
        CHECK(Contains(out, rig.theme.error));
    }
    {
        const std::string out = rig.Run("export", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "用法"));
        CHECK(Contains(StripAnsi(out), "/trace export <路径>"));
    }
    {
        rig.ctx.trace_hub = nullptr;
        OutputCapture capture;
        app::HandleTraceCommand(rig.ctx, "export somewhere.json");
        const std::string out = capture.text();
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "没有追踪 hub"));
    }
}

TEST_CASE("export 档: 脱敏包落盘后回执进键值对框") {
    TraceRig rig;
    FeedExecution(rig.hub, "exe-001", "edit_file", 1, agent::ToolOutcome::Succeeded, "", 3);
    const std::filesystem::path out_path =
        std::filesystem::temp_directory_path() / "lubancode-trace-frame-test.json";
    const std::string out = rig.Run("export " + out_path.string(), rig.theme);
    REQUIRE(Contains(out, kBoxLightTopLeft));
    CHECK(Contains(StripAnsi(out), "已导出脱敏追踪账"));
    CHECK(Contains(StripAnsi(out), "1 枚 execution"));
    std::error_code ec;
    std::filesystem::remove(out_path, ec);
}

TEST_CASE("详细档: 单枚键值对框带 error_code;查无此枚给 notice 框") {
    TraceRig rig;
    FeedExecution(rig.hub, "exe-001", "edit_file", 1, agent::ToolOutcome::ToolError,
                  "schema_rejected", 3);
    {
        const std::string out = rig.Run("exe-001", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        const std::string plain = StripAnsi(out);
        CHECK(Contains(plain, "edit_file"));
        CHECK(Contains(plain, "error_code"));
        CHECK(Contains(plain, "schema_rejected"));
        CHECK(Contains(plain, "recovery"));
    }
    {
        const std::string out = rig.Run("exe-404", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "没有这枚 execution 的账"));
    }
}

TEST_CASE("errors/裸敲空态进框,不再裸打印") {
    TraceRig rig;
    {
        const std::string out = rig.Run("errors", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "没有明确失败或 unknown 的工具调用"));
    }
    {
        const std::string out = rig.Run("", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "还没有工具调用的追踪账"));
    }
}

TEST_CASE("plain 主题: 全族输出零转义字节、无框字形(T3/--no-color 路径)") {
    TraceRig rig;
    FeedExecution(rig.hub, "exe-001", "edit_file", 1, agent::ToolOutcome::ToolError,
                  "schema_rejected", 3);
    for (const std::string& args :
         {"errors", "", "exe-001", "exe-404", "export", "export --raw"}) {
        CAPTURE(args);
        const std::string out = rig.Run(args, rig.plain_theme);
        CHECK(out.find("\x1b") == std::string::npos);
        CHECK(!Contains(out, kBoxLightTopLeft));
        CHECK(!Contains(out, kBoxLightVert));
        // 信息一字不少:表头与内容都在,只是没了色与框。
        if (args == "errors") {
            CHECK(Contains(out, "outcome"));
            CHECK(Contains(out, "exe-001"));
            CHECK(Contains(out, "schema_rejected"));
        }
    }
}
