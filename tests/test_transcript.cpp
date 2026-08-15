// UI-B(0.12.0):工具条目化渲染的纯函数单测——FormatTranscriptItem 的
// 五种状态 × 彩色/plain、超宽截断、多行摘要缩进、子代理条目缩进,以及
// 各工具的参数摘要(BuildToolTitle)和结果摘要(run_command 退出码+耗时、
// write/edit 的 +N -M、错误前 5 行截断……)生成逻辑。

#include <doctest/doctest.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "cli/theme.hpp"
#include "cli/tool_display.hpp"
#include "cli/transcript.hpp"

using lubancode::cli::AgentDoneSummary;
using lubancode::cli::BuildToolTitle;
using lubancode::cli::BuiltinTheme;
using lubancode::cli::CountLines;
using lubancode::cli::ErrorSummaryLines;
using lubancode::cli::FormatTranscriptItem;
using lubancode::cli::FormatRestoredHistory;
using lubancode::cli::ParseRunCommandExitCode;
using lubancode::cli::ReadFileDoneSummary;
using lubancode::cli::RunCommandDoneSummary;
using lubancode::cli::SearchDoneSummary;
using lubancode::cli::TranscriptItem;
using lubancode::cli::TranscriptKind;
using lubancode::cli::TranscriptStatus;
using lubancode::cli::TranscriptStatusWord;
using lubancode::cli::TruncateUtf8Bytes;
using lubancode::cli::TruncateUtf8Codepoints;
using lubancode::cli::WriteDiffSummary;

namespace {

constexpr const char* kDot = "\xE2\x97\x8F";    // ●
constexpr const char* kElbow = "\xE2\x8E\xBF";  // ⎿

TranscriptItem MakeItem(TranscriptStatus status, TranscriptKind kind = TranscriptKind::Tool) {
    TranscriptItem item;
    item.id = 1;
    item.kind = kind;
    item.tool_name = "run_command";
    item.title = "run_command(git log --oneline -3)";
    item.summary_lines = {"Done · 退出码 0 · 1.2s"};
    item.status = status;
    return item;
}

}  // namespace

// ---- FormatTranscriptItem:五种状态 × 彩色/plain ---------------------------

TEST_CASE("FormatTranscriptItem: plain 主题下五种状态各自的文字状态灯") {
    const auto theme = BuiltinTheme("plain");
    const struct {
        TranscriptStatus status;
        const char* word;
    } cases[] = {
        {TranscriptStatus::Running, "[RUNNING]"},   {TranscriptStatus::Ok, "[OK]"},
        {TranscriptStatus::Error, "[ERROR]"},       {TranscriptStatus::Cancelled, "[CANCELLED]"},
        {TranscriptStatus::Interrupted, "[INTERRUPTED]"},
    };
    for (const auto& c : cases) {
        const std::string out = FormatTranscriptItem(MakeItem(c.status), theme, 120);
        CHECK(out.find(c.word) == 0);  // 状态词在行首
        CHECK(out.find("run_command(git log --oneline -3)") != std::string::npos);
        CHECK(out.find("\x1b") == std::string::npos);  // plain 不夹任何 ANSI
        CHECK(out.find(kDot) == std::string::npos);    // plain 不用 ● 字符
    }
    // 待确认态(plain)是 [CONFIRM]
    const std::string pending = FormatTranscriptItem(MakeItem(TranscriptStatus::Pending), theme, 120);
    CHECK(pending.find("[CONFIRM]") == 0);
    CHECK(TranscriptStatusWord(TranscriptStatus::Pending) == "[CONFIRM]");
}

TEST_CASE("FormatTranscriptItem: 彩色主题只染状态灯,正文参数不染色") {
    const auto theme = BuiltinTheme("dark");
    const struct {
        TranscriptStatus status;
        std::string color;
    } cases[] = {
        {TranscriptStatus::Running, theme.tool_line},           // 执行中:黄
        {TranscriptStatus::Pending, theme.tool_line},           // 待确认:黄
        {TranscriptStatus::Ok, theme.prompt},                   // 成功:绿
        {TranscriptStatus::Error, theme.error},                 // 失败:红
        {TranscriptStatus::Cancelled, theme.stats},             // 拒绝:灰
        {TranscriptStatus::Interrupted, "\x1b[2m" + theme.tool_line},  // 打断:灰黄(dim + 黄)
    };
    for (const auto& c : cases) {
        const std::string out = FormatTranscriptItem(MakeItem(c.status), theme, 120);
        // 状态灯:颜色 + ● + reset,紧跟一个空格,然后是不着色的正文
        const std::string light = c.color + kDot + theme.reset + " ";
        CHECK(out.find(light) == 0);
        // 正文(工具名)出现在 reset 之后——只染灯,不染参数
        CHECK(out.find("run_command(git log --oneline -3)") > out.find(theme.reset));
    }
}

TEST_CASE("FormatTranscriptItem: 摘要行缩进——⎿ 开头,续行再缩两空格") {
    const auto theme = BuiltinTheme("plain");
    TranscriptItem item = MakeItem(TranscriptStatus::Error);
    item.summary_lines = {"Error: 服务器未响应(超时 30s)", "连接在 tools/call 后关闭"};
    const std::string out = FormatTranscriptItem(item, theme, 120);
    CHECK(out.find("\n  " + std::string(kElbow) + " Error: 服务器未响应(超时 30s)\n") != std::string::npos);
    CHECK(out.find("\n    连接在 tools/call 后关闭\n") != std::string::npos);
}

TEST_CASE("FormatTranscriptItem: 子代理条目整体再缩四空格") {
    const auto theme = BuiltinTheme("plain");
    TranscriptItem item = MakeItem(TranscriptStatus::Ok, TranscriptKind::SubTool);
    item.summary_lines = {"读取 12 行", "第二行"};
    const std::string out = FormatTranscriptItem(item, theme, 120);
    CHECK(out.find("    [OK] ") == 0);
    CHECK(out.find("\n      " + std::string(kElbow) + " 读取 12 行\n") != std::string::npos);
    CHECK(out.find("\n        第二行\n") != std::string::npos);
}

TEST_CASE("FormatTranscriptItem: 首行超宽按终端宽截断加 ...") {
    const auto theme = BuiltinTheme("plain");
    TranscriptItem item = MakeItem(TranscriptStatus::Ok);
    item.title = "run_command(" + std::string(200, 'x') + ")";
    const int width = 40;
    const std::string out = FormatTranscriptItem(item, theme, width);
    const std::string first_line = out.substr(0, out.find('\n'));
    CHECK(first_line.size() <= static_cast<std::size_t>(width));  // 纯 ASCII,字节数即显示宽
    CHECK(first_line.find("...") != std::string::npos);
    // 宽度富余时不截断
    const std::string wide = FormatTranscriptItem(item, theme, 500);
    CHECK(wide.find(item.title) != std::string::npos);
}

TEST_CASE("FormatTranscriptItem: width<=0 不截断") {
    const auto theme = BuiltinTheme("plain");
    TranscriptItem item = MakeItem(TranscriptStatus::Ok);
    item.title = "run_command(" + std::string(300, 'y') + ")";
    const std::string out = FormatTranscriptItem(item, theme, 0);
    CHECK(out.find(item.title) != std::string::npos);
}

// ---- BuildToolTitle:各工具的参数摘要 --------------------------------------

TEST_CASE("BuildToolTitle: run_command 显示命令,read/write/edit 显示路径") {
    CHECK(BuildToolTitle("run_command", {{"command", "git status"}}) == "run_command(git status)");
    CHECK(BuildToolTitle("read_file", {{"path", "src/main.cpp"}}) == "read_file(src/main.cpp)");
    CHECK(BuildToolTitle("write_file", {{"path", "a.txt"}, {"content", "xxx"}}) == "write_file(a.txt)");
    CHECK(BuildToolTitle("edit_file", {{"path", "b.txt"}, {"old_string", "o"}, {"new_string", "n"}}) ==
          "edit_file(b.txt)");
}

TEST_CASE("BuildToolTitle: agent 只认真正短 title,不拿 prompt 片段冒充") {
    const std::string long_prompt(100, 'p');
    const std::string title = BuildToolTitle("agent", {{"title", "项目记忆升级"}, {"prompt", long_prompt}});
    CHECK(title == "agent(项目记忆升级)");
    CHECK(title.find('p') == std::string::npos);  // prompt 一个字都不上标题
}

TEST_CASE("BuildToolTitle: ask_user 显示第一道问题,不把整份参数糊上屏") {
    const nlohmann::json input = {
        {"questions", nlohmann::json::array({{{"question", "你想选哪一种实现?"},
                                               {"options", nlohmann::json::array()}}})},
    };
    CHECK(BuildToolTitle("ask_user", input) == "ask_user(你想选哪一种实现?)");
}

TEST_CASE("BuildToolTitle: web_search 显示查询词，起点参数未到时不画空对象") {
    CHECK(BuildToolTitle("web_search", nlohmann::json::object()) == "web_search()");
    CHECK(BuildToolTitle("web_search", {{"type", "search"}, {"query", "LLM 强化学习 OPD"}}) ==
          "web_search(LLM 强化学习 OPD)");
    CHECK(BuildToolTitle("web_search", {{"queries", nlohmann::json::array({"a", "b"})}}) ==
          "web_search(2 queries)");
}

TEST_CASE("FormatRestoredHistory: 重放用户、助手 Markdown 与配对工具，不把结果消息画成用户") {
    std::vector<lubancode::api::Message> messages;
    messages.push_back({lubancode::api::Role::User, {lubancode::api::TextBlock{"帮我读文件"}}});
    lubancode::api::Message assistant;
    assistant.role = lubancode::api::Role::Assistant;
    assistant.content.push_back(lubancode::api::TextBlock{"**正在查看**"});
    assistant.content.push_back(
        lubancode::api::ToolUseBlock{"call_1", "read_file", nlohmann::json{{"path", "a.txt"}}});
    messages.push_back(assistant);
    messages.push_back({lubancode::api::Role::User,
                        {lubancode::api::ToolResultBlock{"call_1", "第一行\n第二行\n", false}}});
    messages.push_back(
        {lubancode::api::Role::Assistant, {lubancode::api::TextBlock{"看完了。"}}});

    const std::string out = FormatRestoredHistory(messages, BuiltinTheme("plain"), 120, {2});
    CHECK(out.find("> 你") != std::string::npos);
    CHECK(out.find("帮我读文件") != std::string::npos);
    CHECK(out.find("● 助手") != std::string::npos);
    CHECK(out.find("正在查看") != std::string::npos);
    CHECK(out.find("read_file(a.txt)") != std::string::npos);
    CHECK(out.find("第一行") != std::string::npos);
    CHECK(out.find("另有 1 行") != std::string::npos);
    CHECK(out.find("上下文压缩") != std::string::npos);
    CHECK(out.find("看完了") != std::string::npos);

    std::size_t user_headers = 0;
    for (std::size_t pos = out.find("> 你"); pos != std::string::npos; pos = out.find("> 你", pos + 1)) {
        ++user_headers;
    }
    CHECK(user_headers == 1);
}

TEST_CASE("BuildToolTitle: MCP 工具显示参数紧凑 JSON,换行压成空格") {
    const nlohmann::json input = {{"a", 17}, {"b", 25}};
    CHECK(BuildToolTitle("mcp__test__add", input) == "mcp__test__add(" + input.dump() + ")");
    const std::string multi = BuildToolTitle("run_command", {{"command", "echo a\necho b"}});
    CHECK(multi.find('\n') == std::string::npos);
}

TEST_CASE("BuildToolTitle: todo_write 显示几项") {
    const nlohmann::json input = {{"items", nlohmann::json::array({{{"content", "a"}}, {{"content", "b"}}})}};
    CHECK(BuildToolTitle("todo_write", input) == "todo_write(2 项)");
    CHECK(BuildToolTitle("todo_update", input) == "todo_update(2 项)");
}

// ---- 行统计、退出码解析 ----------------------------------------------------

TEST_CASE("CountLines: 空串 0 行,末尾没换行的最后一截也算一行") {
    CHECK(CountLines("") == 0);
    CHECK(CountLines("a") == 1);
    CHECK(CountLines("a\n") == 1);
    CHECK(CountLines("a\nb") == 2);
    CHECK(CountLines("a\nb\nc\n") == 3);
}

TEST_CASE("ParseRunCommandExitCode: 认 [退出码 N] 前缀") {
    CHECK(ParseRunCommandExitCode("[退出码 0]\nok") == 0);
    CHECK(ParseRunCommandExitCode("[退出码 3]\n") == 3);
    CHECK(ParseRunCommandExitCode("[退出码 -1]\n") == -1);
    CHECK_FALSE(ParseRunCommandExitCode("随便什么").has_value());
    CHECK_FALSE(ParseRunCommandExitCode("[退出码 x]").has_value());
    CHECK_FALSE(ParseRunCommandExitCode("").has_value());
}

// ---- 各工具的结果摘要 ------------------------------------------------------

TEST_CASE("RunCommandDoneSummary: 退出码 + 耗时") {
    CHECK(RunCommandDoneSummary("[退出码 0]\nxxx", 1.23) == "Done · 退出码 0 · 1.2s");
    // 解析不出退出码就省掉那一节
    CHECK(RunCommandDoneSummary("没有退出码前缀", 0.5) == "Done · 0.5s");
}

TEST_CASE("ReadFileDoneSummary: 行数;空文件 0 行") {
    CHECK(ReadFileDoneSummary("     1\ta\n     2\tb\n") == "读取 2 行");
    CHECK(ReadFileDoneSummary("(空文件)") == "读取 0 行");
}

TEST_CASE("WriteDiffSummary: 新增 N 行,删除 M 行;新文件只有新增") {
    CHECK(WriteDiffSummary(39, 36) == "新增 39 行,删除 36 行");
    CHECK(WriteDiffSummary(5, std::nullopt) == "新增 5 行");
    CHECK(WriteDiffSummary(0, 3) == "新增 0 行,删除 3 行");
}

TEST_CASE("SearchDoneSummary: 命中数,没搜到算 0,截断提示行不计") {
    CHECK(SearchDoneSummary("a.cpp:1:hit\nb.cpp:2:hit\n") == "命中 2 处");
    CHECK(SearchDoneSummary("没搜到匹配的内容") == "命中 0 处");
    CHECK(SearchDoneSummary("没找到匹配的文件") == "命中 0 处");
    CHECK(SearchDoneSummary("a.cpp:1:hit\n……(结果超过 100 条,已截断,建议缩小 pattern 或 path 范围)\n") ==
          "命中 1 处");
}

TEST_CASE("AgentDoneSummary: 子代理轮数和子工具次数") {
    CHECK(AgentDoneSummary(3, 5) == "子代理 3 轮 · 5 次工具");
}

TEST_CASE("ErrorSummaryLines: 首行固定 Error:,最多前 5 行,超长带截断标注") {
    // 普通错误:首行 Error: + 原首行
    const auto short_err = ErrorSummaryLines("mcp__test__add", "服务器未响应(超时 30s)\n连接在 tools/call 后关闭");
    REQUIRE(short_err.size() == 2);
    CHECK(short_err[0] == "Error: 服务器未响应(超时 30s)");
    CHECK(short_err[1] == "连接在 tools/call 后关闭");

    // 长错误:只留前 5 行,追加截断标注
    std::string long_err;
    for (int i = 1; i <= 9; ++i) {
        long_err += "line" + std::to_string(i) + "\n";
    }
    const auto lines = ErrorSummaryLines("write_file", long_err);
    REQUIRE(lines.size() == 6);
    CHECK(lines[0] == "Error: line1");
    CHECK(lines[4] == "line5");
    CHECK(lines[5].find("共 9 行") != std::string::npos);
    CHECK(lines[5].find("Ctrl+E") != std::string::npos);

    // run_command 失败:首行改写成 "Error: 退出码 N"
    const auto run_err = ErrorSummaryLines("run_command", "[退出码 3]\n命令输出第一行");
    REQUIRE(run_err.size() == 2);
    CHECK(run_err[0] == "Error: 退出码 3");
    CHECK(run_err[1] == "命令输出第一行");

    // 空输出兜底
    const auto empty_err = ErrorSummaryLines("read_file", "");
    REQUIRE(empty_err.size() == 1);
    CHECK(empty_err[0] == "Error: (无输出)");
}

// ---- UTF-8 截断 ------------------------------------------------------------

TEST_CASE("TruncateUtf8Bytes: 不劈开多字节字符") {
    std::string cjk = "汉字汉字";  // 12 字节
    CHECK(TruncateUtf8Bytes(cjk, 12) == cjk);
    CHECK(TruncateUtf8Bytes(cjk, 11) == "汉字汉");  // 第 11 字节落在"字"中间,退到 9
    CHECK(TruncateUtf8Bytes(cjk, 7) == "汉字");
    CHECK(TruncateUtf8Bytes("abc", 2) == "ab");
}

TEST_CASE("TruncateUtf8Codepoints: 按码点截,超长加 ...") {
    CHECK(TruncateUtf8Codepoints("abcdef", 6) == "abcdef");
    CHECK(TruncateUtf8Codepoints("abcdef", 3) == "abc...");
    CHECK(TruncateUtf8Codepoints("汉字汉字", 2) == "汉字...");
    CHECK(TruncateUtf8Codepoints("", 5) == "");
}

// ---- UI-D(0.16.0):展开版 + 焦点标记 --------------------------------------

TEST_CASE("FormatTranscriptItem expanded: 完整参数 JSON + full_output 全文逐行铺") {
    const auto theme = BuiltinTheme("plain");
    TranscriptItem item = MakeItem(TranscriptStatus::Ok);
    item.input_json = R"({"command":"git status"})";
    item.full_output = "[退出码 0]\nline1\nline2\nline3";
    const std::string out = FormatTranscriptItem(item, theme, 120, /*expanded=*/true);
    // 紧凑部分原样在前(状态灯 + 标题 + 摘要)。
    CHECK(out.find("[OK] run_command(git log --oneline -3)") == 0);
    CHECK(out.find("Done · 退出码 0 · 1.2s") != std::string::npos);
    // 展开部分:参数一行 + 输出标题行 + 正文逐行(两空格缩进)。
    CHECK(out.find("\n  参数: {\"command\":\"git status\"}\n") != std::string::npos);
    CHECK(out.find("完整输出(4 行)") != std::string::npos);
    CHECK(out.find("\n  [退出码 0]\n") != std::string::npos);
    CHECK(out.find("\n  line1\n") != std::string::npos);
    CHECK(out.find("\n  line3\n") != std::string::npos);
    // 紧凑版(expanded=false)一个展开痕迹都不该有。
    const std::string compact = FormatTranscriptItem(item, theme, 120);
    CHECK(compact.find("参数:") == std::string::npos);
    CHECK(compact.find("完整输出") == std::string::npos);
    CHECK(compact.find("line1") == std::string::npos);
}

TEST_CASE("FormatTranscriptItem expanded: full_output 为空补一行占位") {
    const auto theme = BuiltinTheme("plain");
    TranscriptItem item = MakeItem(TranscriptStatus::Running);
    item.summary_lines = {"Running..."};
    const std::string out = FormatTranscriptItem(item, theme, 120, /*expanded=*/true);
    CHECK(out.find("(无完整输出)") != std::string::npos);
    // 没有入参 JSON 就不出参数行。
    CHECK(out.find("参数:") == std::string::npos);
}

TEST_CASE("FormatTranscriptItem expanded: diff 全文跟在工具结果后一起铺出") {
    const auto theme = BuiltinTheme("plain");
    TranscriptItem item = MakeItem(TranscriptStatus::Ok);
    item.tool_name = "edit_file";
    item.title = "edit_file(a.txt)";
    item.summary_lines = {"新增 1 行,删除 1 行"};
    // full_output 的存法跟 main.cpp FinalizeItem 一致:结果 + 空行 + 完整 diff。
    item.full_output = "成功替换 1 处\n\ndiff:\n   1    ctx\n   2  - old\n   2  + new";
    const std::string out = FormatTranscriptItem(item, theme, 0, /*expanded=*/true);
    CHECK(out.find("- old") != std::string::npos);
    CHECK(out.find("+ new") != std::string::npos);
    CHECK(out.find("成功替换 1 处") != std::string::npos);
}

TEST_CASE("FormatTranscriptItem expanded: width>0 时展开行按显示宽截断,width<=0 不截") {
    const auto theme = BuiltinTheme("plain");
    TranscriptItem item = MakeItem(TranscriptStatus::Ok);
    item.full_output = std::string(200, 'z');
    const int width = 40;
    const std::string truncated = FormatTranscriptItem(item, theme, width, /*expanded=*/true);
    // 每一行都不超过终端宽(物理折行会毁掉原地改写的行数记账)。
    std::size_t pos = 0;
    while (pos < truncated.size()) {
        std::size_t nl = truncated.find('\n', pos);
        if (nl == std::string::npos) {
            nl = truncated.size();
        }
        CHECK(nl - pos <= static_cast<std::size_t>(width));
        pos = nl + 1;
    }
    // width=0(Ctrl+E 聚焦查看):全文如实,不截。
    const std::string full = FormatTranscriptItem(item, theme, 0, /*expanded=*/true);
    CHECK(full.find(std::string(200, 'z')) != std::string::npos);
}

TEST_CASE("FormatTranscriptItem focused: 首行加 ► 前缀,plain/彩色都认") {
    const auto plain = BuiltinTheme("plain");
    TranscriptItem item = MakeItem(TranscriptStatus::Ok);
    const std::string out = FormatTranscriptItem(item, plain, 120, /*expanded=*/false, /*focused=*/true);
    CHECK(out.rfind("\xE2\x96\xBA [OK] ", 0) == 0);  // "► [OK] " 打头
    // 摘要行不带标记。
    CHECK(out.find("\n\xE2\x96\xBA") == std::string::npos);

    const auto dark = BuiltinTheme("dark");
    const std::string colored = FormatTranscriptItem(item, dark, 120, false, true);
    CHECK(colored.rfind("\xE2\x96\xBA ", 0) == 0);  // 彩色主题同样 ► 打头,标记不上色

    // 不聚焦时一个 ► 都没有。
    const std::string unfocused = FormatTranscriptItem(item, plain, 120);
    CHECK(unfocused.find("\xE2\x96\xBA") == std::string::npos);
}

TEST_CASE("FormatTranscriptItem focused+expanded: 子代理条目缩进不乱") {
    const auto theme = BuiltinTheme("plain");
    TranscriptItem item = MakeItem(TranscriptStatus::Ok, TranscriptKind::SubTool);
    item.input_json = "{\"path\":\"a.txt\"}";
    item.full_output = "内容";
    const std::string out = FormatTranscriptItem(item, theme, 120, /*expanded=*/true, /*focused=*/true);
    CHECK(out.rfind("\xE2\x96\xBA     [OK] ", 0) == 0);  // ► + 子代理四空格缩进
    CHECK(out.find("\n      参数: ") != std::string::npos);  // 4 + 2 缩进
    CHECK(out.find("\n      内容\n") != std::string::npos);
}

TEST_CASE("FormatTranscriptItems: Ctrl+O 详细档铺子工具全文,紧凑档收掉子工具") {
    const auto theme = BuiltinTheme("plain");
    TranscriptItem parent = MakeItem(TranscriptStatus::Running);
    parent.tool_name = "agent";
    parent.title = "agent(查文件)";
    parent.input_json = R"({"prompt":"查文件"})";

    TranscriptItem child = MakeItem(TranscriptStatus::Ok, TranscriptKind::SubTool);
    child.tool_name = "read_file";
    child.title = "read_file(a.txt)";
    child.input_json = R"({"path":"a.txt"})";
    child.full_output = "第一行\n第二行";

    const std::vector<TranscriptItem> items{parent, child};
    const std::string compact = FormatTranscriptItems(items, theme, 120, /*expanded=*/false);
    CHECK(compact.find("agent(查文件)") != std::string::npos);
    CHECK(compact.find("read_file(a.txt)") == std::string::npos);
    CHECK(compact.find("第一行") == std::string::npos);

    const std::string expanded = FormatTranscriptItems(items, theme, 120, /*expanded=*/true);
    CHECK(expanded.find("参数: {\"prompt\":\"查文件\"}") != std::string::npos);
    CHECK(expanded.find("read_file(a.txt)") != std::string::npos);
    CHECK(expanded.find("参数: {\"path\":\"a.txt\"}") != std::string::npos);
    CHECK(expanded.find("第一行") != std::string::npos);
}

// ---- 思考折叠块:Ctrl+O 就地展开(收定全文 + 进行中快照) --------------------

TEST_CASE("CountUtf8Codepoints: ASCII/汉字/emoji 各按码点计") {
    using lubancode::cli::CountUtf8Codepoints;
    CHECK(CountUtf8Codepoints("") == 0);
    CHECK(CountUtf8Codepoints("abc") == 3);
    CHECK(CountUtf8Codepoints("汉字") == 2);
    CHECK(CountUtf8Codepoints("a汉\xF0\x9F\x9A\x80") == 3);  // 🚀 算一个码点
}

TEST_CASE("FormatTranscriptItem 思考条目:紧凑档一行「思考 Xs」,正文不露") {
    const auto theme = BuiltinTheme("plain");
    TranscriptItem item = MakeItem(TranscriptStatus::Ok, TranscriptKind::Thinking);
    item.tool_name = "thinking";
    item.title = "思考 3.2s";
    item.summary_lines.clear();
    item.full_output = "先想第一步\n再想第二步";
    const std::string out = FormatTranscriptItem(item, theme, 120);
    CHECK(out.find("思考 3.2s\n") != std::string::npos);
    CHECK(out.find("先想第一步") == std::string::npos);
    CHECK(out.find("· ") == std::string::npos);  // 字数标注只在展开档出现
}

TEST_CASE("FormatTranscriptItem 思考条目展开(收定):标题带「· N 字」,正文全文铺,不限行") {
    const auto theme = BuiltinTheme("plain");
    TranscriptItem item = MakeItem(TranscriptStatus::Ok, TranscriptKind::Thinking);
    item.tool_name = "thinking";
    item.title = "思考 3.2s";
    item.summary_lines.clear();
    item.full_output = "line1\nline2";
    const std::string out = FormatTranscriptItem(item, theme, 120, /*expanded=*/true);
    CHECK(out.find("思考 3.2s · 11 字") != std::string::npos);  // 换行也算一个码点
    CHECK(out.find("完整输出(2 行)") != std::string::npos);
    CHECK(out.find("\n  line1\n") != std::string::npos);
    CHECK(out.find("\n  line2\n") != std::string::npos);
    CHECK(out.find("看全文") == std::string::npos);  // 收定后不截断,无需收口行
}

TEST_CASE("FormatTranscriptItem 思考条目展开(进行中,短):已到正文全铺,无收口行") {
    const auto theme = BuiltinTheme("plain");
    TranscriptItem item = MakeItem(TranscriptStatus::Running, TranscriptKind::Thinking);
    item.tool_name = "thinking";
    item.title = "思考中…";
    item.summary_lines.clear();
    item.full_output = "abc";
    const std::string out = FormatTranscriptItem(item, theme, 120, /*expanded=*/true);
    CHECK(out.find("思考中… · 3 字") != std::string::npos);
    CHECK(out.find("\n  abc\n") != std::string::npos);
    CHECK(out.find("看全文") == std::string::npos);
}

TEST_CASE("FormatTranscriptItem 思考条目展开(进行中,空):正文没到,占位行也不铺") {
    const auto theme = BuiltinTheme("plain");
    TranscriptItem item = MakeItem(TranscriptStatus::Running, TranscriptKind::Thinking);
    item.tool_name = "thinking";
    item.title = "思考中…";
    item.summary_lines.clear();
    const std::string out = FormatTranscriptItem(item, theme, 120, /*expanded=*/true);
    CHECK(out.find("(无完整输出)") == std::string::npos);
    CHECK(out.find("· ") == std::string::npos);
}

TEST_CASE("FormatTranscriptItem 思考条目展开(进行中,超长):截到一屏,补「共 N 行」收口") {
    const auto theme = BuiltinTheme("plain");
    TranscriptItem item = MakeItem(TranscriptStatus::Running, TranscriptKind::Thinking);
    item.tool_name = "thinking";
    item.title = "思考中…";
    item.summary_lines.clear();
    std::string body;
    for (int i = 1; i <= 40; ++i) {
        body += "L" + std::string(i < 10 ? "0" : "") + std::to_string(i) + "\n";
    }
    item.full_output = body;
    const std::string out = FormatTranscriptItem(item, theme, 120, /*expanded=*/true);
    CHECK(out.find("完整输出(40 行)") != std::string::npos);
    CHECK(out.find("\n  L01\n") != std::string::npos);
    CHECK(out.find("\n  L30\n") != std::string::npos);  // 帽是 30 行
    CHECK(out.find("L31") == std::string::npos);        // 第 31 行起不铺
    CHECK(out.find("共 40 行,思考结束后 Ctrl+O 看全文") != std::string::npos);
    // 同一条目收定后再展开:全文铺,没有帽。
    item.status = TranscriptStatus::Ok;
    item.title = "思考 9.9s";
    const std::string done = FormatTranscriptItem(item, theme, 120, /*expanded=*/true);
    CHECK(done.find("\n  L31\n") != std::string::npos);
    CHECK(done.find("\n  L40\n") != std::string::npos);
    CHECK(done.find("看全文") == std::string::npos);
}

// ---- ToolDisplay 思考快照通道:进行中 Ctrl+O 能看到已到正文 ------------------

TEST_CASE("ToolDisplay 思考进行中:已到正文走 transcript_snapshot_ 通道,Ctrl+O 文本可见") {
    std::vector<TranscriptItem> transcript;
    const auto theme = BuiltinTheme("plain");
    lubancode::cli::ToolDisplay display(transcript, theme, /*console=*/false,
                                         /*todo=*/nullptr, /*cancel=*/nullptr);
    display.OnThinkingDelta("思路第一段\n");
    display.OnThinkingDelta("思路第二段\n");
    REQUIRE(display.HasActiveThinking());
    std::string live;
    {
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        live = display.FormatSnapshotForToggleLocked(/*expanded=*/true);
    }
    CHECK(live.find("思路第一段") != std::string::npos);
    CHECK(live.find("思路第二段") != std::string::npos);

    // 收定:标题换「思考 Xs」,快照带全文;紧凑档一行,正文不露。
    display.OnThinkingDone();
    std::string done_expanded;
    std::string done_compact;
    {
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        done_expanded = display.FormatSnapshotForToggleLocked(/*expanded=*/true);
        done_compact = display.FormatSnapshotForToggleLocked(/*expanded=*/false);
    }
    CHECK(done_expanded.find("思考 ") != std::string::npos);
    CHECK(done_expanded.find("思路第二段") != std::string::npos);
    CHECK(done_compact.find("思路第二段") == std::string::npos);
}

TEST_CASE("ToolDisplay 思考快照:并发收 delta 与读快照,锁规约下不崩不串") {
    std::vector<TranscriptItem> transcript;
    const auto theme = BuiltinTheme("plain");
    lubancode::cli::ToolDisplay display(transcript, theme, /*console=*/false,
                                         /*todo=*/nullptr, /*cancel=*/nullptr);
    display.OnThinkingDelta("起头\n");  // 条目在主线程建好,后台只追加
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        while (!stop.load()) {
            display.OnThinkingDelta("流水行\n");
        }
    });
    for (int i = 0; i < 200; ++i) {
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        const std::string out = display.FormatSnapshotForToggleLocked(/*expanded=*/true);
        CHECK(!out.empty());
    }
    stop.store(true);
    writer.join();
    CHECK(display.thinking_buffer.empty() == false);  // 攒着的正文还在
    display.OnThinkingDone();
    CHECK(display.HasActiveThinking() == false);
}
