// TUI 排版批 7:/record(slash 命令,record_command.cpp)的输出形状册。
//   - list 走表格:标题=record.list.header 剥尾冒号,列头 id/name/started/
//     state/draft(schema 名);state 列值是既有 i18n 文案一字不改,语义色
//     未完成=Fail(error 色);
//   - 单句回执(status/note/cancel 一族)进键值对框,SentenceField 按句内
//     冒号拆列;
//   - plain 主题零转义、无框;dark 主题有框角;
//   - 主题由调用方递(会话主题),这里钉 plain 与 dark 两档;测试进程
//     SetLanguage("zh-CN") 钉死语言,断言中文文案不随平台漂。
//
// 录制件用真 WorkflowRecorder::Start 落账(纯文件,不发网),list 读回。

#include <doctest/doctest.h>

#include <filesystem>
#include <optional>
#include <sstream>
#include <string>

#include "cli/i18n.hpp"
#include "cli/record_command.hpp"
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "skills/skill_drafter.hpp"

using namespace lubancode;
using namespace lubancode::cli;

namespace {

namespace fs = std::filesystem;

class OutputCapture {
public:
    OutputCapture() { cli::TermPort().Redirect(&stream_, nullptr); }
    ~OutputCapture() { cli::TermPort().Reset(); }
    std::string text() const { return stream_.str(); }

private:
    std::ostringstream stream_;
};

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

const cli::Theme dark = cli::BuiltinTheme("dark");
const cli::Theme plain = cli::BuiltinTheme("plain");

constexpr const char* kBoxTopLeft = "\xe2\x94\x8c";  // ┌

fs::path TempRoot(const std::string& name) {
    const fs::path path = fs::temp_directory_path() / ("lubancode-record-frame-" + name);
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
    return path;
}

std::string RunRecord(const std::string& args, const fs::path& recordings_root,
                      const cli::Theme& theme) {
    std::optional<skills::WorkflowRecorder> recorder;
    RecordCommandContext ctx{recorder, recordings_root, recordings_root / "project-skills",
                             recordings_root / "home-skills", /*refresh_skills=*/nullptr,
                             /*selection=*/nullptr};
    OutputCapture capture;
    HandleRecordCommand(args, ctx, theme);
    return capture.text();
}

}  // namespace

TEST_CASE("record list:空账回原句(空态不进框),plain 零转义") {
    cli::SetLanguage("zh-CN");
    const fs::path root = TempRoot("empty");
    const std::string out = RunRecord("list", root, plain);

    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(Contains(out, "还没有录制件"));  // 空态文案照旧平铺
    CHECK(out.find(kBoxTopLeft) == std::string::npos);
}

TEST_CASE("record list:真账走表格,列头 schema 名,state 列语义色") {
    cli::SetLanguage("zh-CN");
    const fs::path root = TempRoot("one");
    skills::RecordingStartInfo info;
    info.name = "deploy-check";
    info.goal = "部署完看一眼日志";
    const auto started = skills::WorkflowRecorder::Start(root, info);
    REQUIRE(started.has_value());

    const std::string out = RunRecord("list", root, dark);
    CHECK(Contains(out, kBoxTopLeft));       // 表格有框
    CHECK(Contains(out, "录制件(倒序)"));   // 标题=header 剥尾冒号
    CHECK(Contains(out, "deploy-check"));    // name 列
    CHECK(Contains(out, "未完成"));          // state 列值(i18n 既有文案)
    // 列头独立成行:五枚 schema 名同行。
    bool saw_header = false;
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        if (Contains(line, "id") && Contains(line, "name") && Contains(line, "started") &&
            Contains(line, "state") && Contains(line, "draft")) {
            saw_header = true;
        }
    }
    CHECK(saw_header);
    // 未完成 = Fail 档 error 色(语义色,批 7 合同)。
    CHECK(Contains(out, dark.error));
}

TEST_CASE("record list:plain 表头下垫 '-' 横线,零转义无框") {
    cli::SetLanguage("zh-CN");
    const fs::path root = TempRoot("plain-table");
    skills::RecordingStartInfo info;
    info.name = "probe";
    REQUIRE(skills::WorkflowRecorder::Start(root, info).has_value());

    const std::string out = RunRecord("list", root, plain);
    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(out.find(kBoxTopLeft) == std::string::npos);
    CHECK(Contains(out, "录制件(倒序)"));
    CHECK(Contains(out, "deploy-check") == false);  // 这个目录里没有这笔账
    CHECK(Contains(out, "probe"));
    // 表头行下垫一条 "-" 横线(plain 的颜色分隔顶替)。
    bool saw_rule = false;
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.find_first_not_of('-') == std::string::npos &&
            line.size() >= 10) {
            saw_rule = true;
        }
    }
    CHECK(saw_rule);
}

TEST_CASE("record status:单句回执进键值对框,句内冒号拆列") {
    cli::SetLanguage("zh-CN");
    const fs::path root = TempRoot("status");

    const std::string plain_out = RunRecord("status", root, plain);
    CHECK(plain_out.find("\x1b") == std::string::npos);
    CHECK(Contains(plain_out, "当前没有在录"));  // idle 句整句进 value

    const std::string dark_out = RunRecord("status", root, dark);
    CHECK(Contains(dark_out, kBoxTopLeft));
    CHECK(Contains(dark_out, "当前没有在录"));
}
