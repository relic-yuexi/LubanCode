// TUI 排版批 7:`lubancode gateway` 人看分支的输出形状册。
//   - status:probe 行逐句拆列进键值对框(标题 gateway status);三栏
//     sections 行(FormatSectionLines,有册钉格式)原样逐行,一字不动;
//   - job list:tab 平铺行升表格(列头 job_id/state/schedule/rev/prompt,
//     rev 右对齐);job read:头部键值对 + occurrences 表(空态句原样);
//   - stop:停机回执键值对;doctor:check 清单走表格(result 列
//     [ok]/[warn]/[FAIL] 记号 + Pass/Skip/Fail 三态语义色)+ 退出码键值对;
//   - --json 分支(status/job/doctor)printf 原样,本册不碰(字节级不变
//     属代码核实,见 PR body);
//   - plain 零转义、无框。
//
// GatewayCommandArgs.gateway_root 注入临时树(test_gateway_process 同款);
// automation 账手写 job.created 行(ReadAutomationProjection 的公开格式)。
// install/uninstall/start/restart 要真服务管理器,真机未验。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "cli/gateway_command.hpp"
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "gateway/profile.hpp"

using namespace lubancode;

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

const cli::Theme plain = cli::BuiltinTheme("plain");

fs::path TempRoot(const std::string& name) {
    const fs::path path = fs::temp_directory_path() / ("lubancode-gateway-frame-" + name);
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

// 一份最小 automation 账:一枚 once 任务(公开 job.created 行格式)。
void SeedJobs(const fs::path& root, const std::string& job_id, const std::string& prompt) {
    const gateway::GatewayProfilePaths paths =
        gateway::ResolveGatewayProfilePaths(root, "default");
    WriteFile(paths.automation_log,
              "{\"type\":\"job.created\",\"jobId\":\"" + job_id + "\",\"revision\":1," +
                  "\"prompt\":\"" + prompt + "\"}\n");
}

int RunGateway(const std::string& verb, const fs::path& root, std::string* out,
               const std::string& job_verb = std::string(),
               const std::string& job_id = std::string()) {
    GatewayCommandArgs args;
    args.verb = verb;
    args.gateway_root = root;
    args.job_verb = job_verb;
    args.job_id = job_id;
    OutputCapture capture;
    const int code = RunGatewayCommand(args);
    *out = capture.text();
    return code;
}

}  // namespace

TEST_CASE("gateway status:空树 probe 键值对框,sections 行原样,退 1") {
    const fs::path root = TempRoot("status");
    std::string out;
    const int code = RunGateway("status", root, &out);

    CHECK(code == 1);  // 没跑起来的 Gateway:非 0 如实
    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(Contains(out, "gateway status"));  // 标题行
    CHECK(Contains(out, "gateway.not_running"));
    // 三栏 sections 的行形状归 FormatSectionLines(不动):栏头原样在。
    CHECK(Contains(out, "[work]"));
    CHECK(Contains(out, "[delivery]"));
}

TEST_CASE("gateway job list:空账原句,退 0") {
    const fs::path root = TempRoot("jobs-empty");
    std::string out;
    const int code = RunGateway("job", root, &out, "list");

    CHECK(code == 0);
    CHECK(Contains(out, "没有持久任务"));  // 空态文案照旧平铺
    CHECK(out.find("\x1b") == std::string::npos);
}

TEST_CASE("gateway job list:有账走表格,列头 schema 名,rev 右对齐列") {
    const fs::path root = TempRoot("jobs-one");
    SeedJobs(root, "job-1", "每天早八问好");
    std::string out;
    const int code = RunGateway("job", root, &out, "list");

    CHECK(code == 0);
    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(Contains(out, "jobs"));        // 表标题
    CHECK(Contains(out, "job-1"));       // 行
    CHECK(Contains(out, "active"));      // state 列
    CHECK(Contains(out, "once due"));    // schedule 列
    CHECK(Contains(out, "每天早八问好"));
    // 列头独立成行。
    bool saw_header = false;
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        if (Contains(line, "job_id") && Contains(line, "state") && Contains(line, "schedule") &&
            Contains(line, "rev") && Contains(line, "prompt")) {
            saw_header = true;
        }
    }
    CHECK(saw_header);
}

TEST_CASE("gateway job read:头部键值对 + 正文句,occurrence 空态原句") {
    const fs::path root = TempRoot("read");
    SeedJobs(root, "job-9", "盯一下构建");
    std::string out;
    const int code = RunGateway("job", root, &out, "read", "job-9");

    CHECK(code == 0);
    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(Contains(out, "job read"));    // 标题
    CHECK(Contains(out, "job-9"));
    CHECK(Contains(out, "正文"));        // 正文句拆列
    CHECK(Contains(out, "盯一下构建"));
    CHECK(Contains(out, "(尚无 occurrence)"));  // 空态句照旧平铺
}

TEST_CASE("gateway job read:任务不存在,stdout 句原样 + 退 1") {
    const fs::path root = TempRoot("read-missing");
    std::string out;
    const int code = RunGateway("job", root, &out, "read", "ghost");

    CHECK(code == 1);
    CHECK(Contains(out, "任务不存在"));
    CHECK(Contains(out, "ghost"));
}

TEST_CASE("gateway stop:空树停机回执键值对,退 0") {
    const fs::path root = TempRoot("stop");
    std::string out;
    const int code = RunGateway("stop", root, &out);

    CHECK(code == 0);
    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(!out.empty());  // NotRunning 的 detail 句如实进框
}

TEST_CASE("gateway doctor:check 清单表格 + 退出码键值对") {
    const fs::path root = TempRoot("doctor");
    std::string out;
    const int code = RunGateway("doctor", root, &out);

    CHECK(code >= 0);
    CHECK(code <= 2);  // 0 全绿/1 有警/2 有病,空树如实
    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(Contains(out, "gateway doctor"));  // 表标题
    CHECK(Contains(out, "退出码"));
    // 列头独立成行;result 列是 [ok]/[warn]/[FAIL]/[info] 记号。
    bool saw_header = false;
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        if (Contains(line, "result") && Contains(line, "code") && Contains(line, "detail")) {
            saw_header = true;
        }
    }
    CHECK(saw_header);
    const bool saw_check_row =
        Contains(out, "[ok]") || Contains(out, "[warn]") || Contains(out, "[FAIL]") ||
        Contains(out, "[info]");
    CHECK(saw_check_row);
}
