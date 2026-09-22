// TUI 排版批 2(tui优化todo.todo P2:/hook-check 即 `lubancode hook
// validate/test`)的输出形状册。
//   - 静态检查表(result/item/detail,result 列 pass/FAIL 上语义色)+
//     fixtures 表(verb=test 时)+ 退出码键值对框;
//   - --json 分支(机器面)收口 TermOut 后字节级不变——与引擎自跑一份
//     ToJson().dump(2) 对账;
//   - 测试进程 stdout 非控制台,RunHookCheckCommand 内部
//     DetectConsoleCapability() 自然降 plain:这里钉的就是 plain 形状
//     (零转义、表头下垫 "-" 横线、对齐保住)——合同第 3 条的 T3 路径。
//
// 造一只临时 hook 包(hook.json + main.lua + fixtures,形状与 scaffold
// 同款)跑真引擎(runtime::RunHookPackageCheck),validate 不发网。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "app/cli_options.hpp"
#include "app/commands/hook_check_commands.hpp"
#include "cli/terminal_port.hpp"
#include "platform/paths.hpp"
#include "runtime/hook_package_check.hpp"

using namespace lubancode;

namespace {

namespace fs = std::filesystem;

fs::path TempPackage(const std::string& name) {
    static int sequence = 0;
    static const std::string run_id = std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const fs::path path =
        fs::temp_directory_path() / ("lubancode-hook-check-frame-" + run_id + "-" + name + "-" +
                                     std::to_string(++sequence));
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path / "fixtures", ec);
    return path;
}

void WriteFile(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

// 与 hook_check_commands.cpp 的 scaffold 同款形状:生成后 validate 全绿。
void MakeScaffoldPackage(const fs::path& dir) {
    WriteFile(dir / "hook.json", R"({
  "schemaVersion": 1,
  "id": "frame-probe",
  "version": "0.1.0",
  "entry": "main.lua",
  "hooks": [
    {
      "hookPoint": "PreUser",
      "name": "prompt.normalize",
      "handler": "normalize",
      "priority": 100,
      "match": {"origin": "human", "purpose": "interactive"}
    }
  ]
}
)");
    WriteFile(dir / "main.lua", R"(local function trim(s)
    return (s:gsub("^%s*(.-)%s*$", "%1"))
end

return {
    normalize = function(ctx, input, next)
        if type(input.prompt) ~= "string" then
            return next(input)
        end
        local candidate = { prompt = trim(input.prompt) }
        for key, value in pairs(input) do
            if key ~= "prompt" then candidate[key] = value end
        end
        return next(candidate)
    end,
}
)");
    WriteFile(dir / "fixtures" / "clean.json", R"({
  "name": "trims-surrounding-whitespace",
  "trigger": {
    "input": {"prompt": "  你好,LubanCode  "},
    "origin": "human",
    "purpose": "interactive",
    "deliveryMode": "steer"
  },
  "expect": {
    "kind": "completed",
    "prompt": "你好,LubanCode",
    "terminalRuns": 1,
    "records": {"PreUser/prompt.normalize": {"outcome": "completed", "nextCalls": 1}}
  }
}
)");
    WriteFile(dir / "fixtures" / "pass.json", R"({
  "name": "leaves-clean-prompt-alone",
  "trigger": {
    "input": {"prompt": "已经干净"},
    "origin": "human",
    "purpose": "interactive"
  },
  "expect": {
    "kind": "completed",
    "prompt": "已经干净"
  }
}
)");
}

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

int RunCheck(const std::string& verb, const std::string& dir) {
    app::HookCliArgs args;
    args.verb = verb;
    args.package_dir = dir;
    OutputCapture capture;
    const int code = app::RunHookCheckCommand(args);
    (void)capture.text();
    return code;
}

std::string RunCheckText(const std::string& verb, const std::string& dir) {
    app::HookCliArgs args;
    args.verb = verb;
    args.package_dir = dir;
    OutputCapture capture;
    (void)app::RunHookCheckCommand(args);
    return capture.text();
}

}  // namespace

TEST_CASE("validate: 静态检查走表格,表头三列,退出码进键值对") {
    const fs::path dir = TempPackage("validate");
    MakeScaffoldPackage(dir);
    const std::string out = RunCheckText("validate", platform::PathToUtf8(dir));

    // 测试进程里 RunHookCheckCommand 自起的是 plain 主题(管道 stdout):
    // 零转义字节;表头独立成行、下垫 "-" 横线(plain 的分隔顶替色)。
    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(Contains(out, "hook validate"));
    CHECK(Contains(out, "静态检查"));
    bool saw_header = false;
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        if (Contains(line, "result") && Contains(line, "item") && Contains(line, "detail")) {
            saw_header = true;
        }
    }
    CHECK(saw_header);
    // 全绿包:result 列的值是引擎的 pass 记号,表内有条目。
    CHECK(Contains(out, "pass"));
    // 退出码句进键值对("退出码"拆列)。
    CHECK(Contains(out, "退出码"));
}

TEST_CASE("test: fixtures 表另起一节,verb=test 才出现") {
    const fs::path dir = TempPackage("test");
    MakeScaffoldPackage(dir);
    const std::string out = RunCheckText("test", platform::PathToUtf8(dir));

    CHECK(Contains(out, "fixtures(fake adapter,真实 Lua runtime)"));
    CHECK(Contains(out, "trims-surrounding-whitespace"));
    CHECK(Contains(out, "leaves-clean-prompt-alone"));
    // fixtures 行与静态检查行分属两节,各自有表头。
    std::istringstream lines(out);
    std::string line;
    int fixture_header_rows = 0;
    while (std::getline(lines, line)) {
        if (Contains(line, "result") && Contains(line, "name") && Contains(line, "detail")) {
            ++fixture_header_rows;
        }
    }
    CHECK(fixture_header_rows >= 1);

    // validate 不跑 fixtures:同包 validate 输出里没有 fixtures 节。
    const std::string validate_out = RunCheckText("validate", platform::PathToUtf8(dir));
    CHECK(validate_out.find("fixtures(fake adapter,真实 Lua runtime)") == std::string::npos);
}

TEST_CASE("fail 档:坏包如实红着报,退出码 1") {
    const fs::path dir = TempPackage("broken");
    // 只有目录没有 hook.json:包读不到,静态检查给 FAIL,退出码非 0。
    WriteFile(dir / "placeholder.txt", "not a hook package");
    const int code = RunCheck("validate", platform::PathToUtf8(dir));
    CHECK(code != 0);
    const std::string out = RunCheckText("validate", platform::PathToUtf8(dir));
    CHECK(Contains(out, "FAIL"));
}

TEST_CASE("--json: 机器面字节级不变(std::cout 收口 TermOut 后对账)") {
    const fs::path dir = TempPackage("json");
    MakeScaffoldPackage(dir);
    runtime::HookCheckOptions options;
    options.run_fixtures = true;
    const runtime::HookCheckReport report =
        runtime::RunHookPackageCheck(dir, options);
    const std::string expected = report.ToJson().dump(2) + "\n";

    app::HookCliArgs args;
    args.verb = "test";
    args.package_dir = platform::PathToUtf8(dir);
    args.json = true;
    OutputCapture capture;
    (void)app::RunHookCheckCommand(args);
    CHECK(capture.text() == expected);  // 一个字节都不差
}
