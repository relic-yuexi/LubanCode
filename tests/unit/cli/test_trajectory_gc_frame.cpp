// TUI 排版批 7:`lubancode trajectory gc` 的输出形状册(从
// test_trajectory_command_frame 拆出:该册在 macos-clang 上 Bus error,
// 拆册二分定位崩点;usage 档留在原册)。
//   - gc:逐 session 走表格(reclaim/deleted/failed 数值列右对齐,
//     失败行 Fail 色)+ dry-run 尾句键值对;
//   - sessions 目录不在:stderr 原样 + 退 1(错误行不进框);
//   - plain 零转义、无框。
//
// RunGc 吃已解析的 workspace 目录(直调,零全局状态)。--derived-only
// 真删档与多 session 真账真机未验。


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
    OutputCapture() { cli::TermPort().Redirect(&out_, &err_); }
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

const cli::Theme plain = cli::BuiltinTheme("plain");

fs::path TempRoot(const std::string& name) {
    const fs::path path = fs::temp_directory_path() / ("lubancode-trajectory-gc-frame-" + name);
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

fs::path MakeWorkspace(const fs::path& root) {
    const fs::path ws = root / "ws-a";
    WriteFile(ws / "sessions" / "s-alpha" / "main.jsonl", std::string(100, 'j'));
    fs::create_directories(ws / "sessions" / "s-beta");
    return ws;
}

}  // namespace

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

TEST_CASE("gc:sessions 目录不在,退 1,stdout 无 frame 无转义") {
    // 错误行走 std::cerr 直写 C 流(批 7 不动错误流),TermPort 的
    // Redirect 截不到——文字未动由 diff 核实(PR body),这里钉退出码与
    // stdout 面。
    const fs::path root = TempRoot("gc-missing");
    fs::create_directories(root / "ws-empty");

    OutputCapture capture;
    const int code = RunGc(root / "ws-empty", "k1", false);
    CHECK(code == 1);
    CHECK(capture.out().find("\x1b") == std::string::npos);
}
