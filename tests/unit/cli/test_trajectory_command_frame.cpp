// TUI 排版批 7:`lubancode trajectory` 人看分支的输出形状册。
//   - usage:头句键值对框(标题 usage)+ 逐 session 表格(字节/文件数列
//     右对齐)+ 尾注键值对;
//   - gc:逐 session 表格 + dry-run 尾句键值对;
//   - verify:头句键值对(标题 verify,未过档 Error 语义色);
//   - 错误路径(缺 key/找不到 session)走 stderr 原样,一字不动;
//   - plain 零转义、无框;dark 有框角。
//
// RunUsageReport/RunGc 吃已解析的 workspace 目录(直调,零全局状态);
// RunTrajectoryCommand 走 trajectories_root 注入临时树。verify 绿档/
// replay/export 要真 Journal 账,本册不造(真机未验),只钉可控形状。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "cli/trajectory_command.hpp"
#include "platform/paths.hpp"

using namespace lubancode;
using namespace lubancode::cli;

namespace {

namespace fs = std::filesystem;

class OutputCapture {
public:
    // 双流都截:stdout 钉 frame 形状,stderr 钉错误行原样。
    OutputCapture() {
        cli::TermPort().Redirect(&out_, &err_);
    }
    ~OutputCapture() { cli::TermPort().Reset(); }
    const std::string& out() const { return out_.str(); }
    const std::string& err() const { return err_.str(); }

private:
    std::ostringstream out_;
    std::ostringstream err_;
};

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

const cli::Theme dark = cli::BuiltinTheme("dark");
const cli::Theme plain = cli::BuiltinTheme("plain");

constexpr const char* kBoxTopLeft = "\xe2\x94\x8c";  // ┌

fs::path TempRoot(const std::string& name) {
    const fs::path path = fs::temp_directory_path() / ("lubancode-trajectory-frame-" + name);
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path, ec);
    return path;
}

void WriteFile(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
}

// 造一棵 workspace:sessions/s-alpha 带一份 100B 的 main.jsonl,s-beta 空账。
fs::path MakeWorkspace(const fs::path& root) {
    const fs::path ws = root / "ws-a";
    WriteFile(ws / "sessions" / "s-alpha" / "main.jsonl", std::string(100, 'j'));
    fs::create_directories(ws / "sessions" / "s-beta");
    return ws;
}

}  // namespace

TEST_CASE("usage:头句键值对 + session 表格 + 尾注,plain 零转义无框") {
    const fs::path root = TempRoot("usage");
    const fs::path ws = MakeWorkspace(root);

    OutputCapture capture;
    const int code = RunUsageReport(ws, "k1");
    CHECK(code == 0);
    const std::string out = capture.out();

    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(out.find(kBoxTopLeft) == std::string::npos);
    CHECK(Contains(out, "usage"));            // 标题行
    CHECK(Contains(out, "workspace k1"));     // 头句(SentenceField 整句进 value)
    CHECK(Contains(out, "2 场 session"));     // 头句计数
    // 表头独立成行 + plain 下垫 "-" 横线。
    bool saw_header = false;
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        if (Contains(line, "session") && Contains(line, "journal") && Contains(line, "blobs") &&
            Contains(line, "files")) {
            saw_header = true;
        }
    }
    CHECK(saw_header);
    CHECK(Contains(out, "s-alpha"));
    CHECK(Contains(out, "s-beta"));
    CHECK(Contains(out, "100B"));  // journal 列值
    // 尾注键值对。
    CHECK(Contains(out, "session delete"));
}

TEST_CASE("usage:dark 有框,表格标题 workspace <key>") {
    const fs::path root = TempRoot("usage-dark");
    const fs::path ws = MakeWorkspace(root);

    // dark 形状由 frame 助手保(同输入换主题只换色/框);这里钉一个框角
    // 与表标题存在即可,不重复钉对齐(test_frame_helpers 已钉)。
    OutputCapture capture;
    (void)RunUsageReport(ws, "k1");
    const std::string out = capture.out();
    CHECK(Contains(out, kBoxTopLeft));
    CHECK(Contains(out, "workspace k1"));
}

TEST_CASE("gc:dry-run 表格 + 尾句键值对,退出码 0") {
    const fs::path root = TempRoot("gc");
    const fs::path ws = MakeWorkspace(root);

    OutputCapture capture;
    const int code = RunGc(ws, "k1", /*derived_only=*/false);
    CHECK(code == 0);
    const std::string out = capture.out();

    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(Contains(out, "gc"));                 // 表标题
    CHECK(Contains(out, "s-alpha"));            // 表行
    CHECK(Contains(out, "KiB"));                // reclaim 列值
    CHECK(Contains(out, "dry-run 只报账;真清加 --derived-only。"));  // 尾句
}

TEST_CASE("gc:sessions 目录不在,stderr 原样 + 退 1(错误行不进框)") {
    const fs::path root = TempRoot("gc-missing");
    fs::create_directories(root / "ws-empty");

    OutputCapture capture;
    const int code = RunGc(root / "ws-empty", "k1", false);
    CHECK(code == 1);
    CHECK(Contains(capture.err(), "没有 sessions 目录"));
    CHECK(capture.out().find("\x1b") == std::string::npos);
}

TEST_CASE("verify:空 session 目录如实未过,退 2,键值对形状") {
    const fs::path root = TempRoot("verify");
    fs::create_directories(root / "ws-a" / "sessions" / "s-none");

    TrajectoryCommandArgs args;
    args.verb = "verify";
    args.session_id = "s-none";
    args.trajectories_root = platform::PathToUtf8(root);
    OutputCapture capture;
    const int code = RunTrajectoryCommand(args);
    CHECK(code == 2);
    const std::string out = capture.out();
    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(Contains(out, "verify"));       // 标题
    CHECK(Contains(out, "s-none"));       // key
    CHECK(Contains(out, "verify 未过"));  // 值
}

TEST_CASE("缺 workspace key:stderr 原样 + 退 1") {
    TrajectoryCommandArgs args;
    args.verb = "usage";
    args.trajectories_root = platform::PathToUtf8(TempRoot("noid"));
    OutputCapture capture;
    const int code = RunTrajectoryCommand(args);
    CHECK(code == 1);
    CHECK(Contains(capture.err(), "缺 workspace key"));
}
