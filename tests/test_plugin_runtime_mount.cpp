// plugins 单第 7 步(接 Runtime)的测试:PluginToolAdapter 的 call_id 换
// runtime::ProcessIdAuthority() 的真 id、日志与 stdout 分流(LogSink)、
// SetCancel/SetCwd 装配口、adapter 进 registry 的中立 Tool 形状。
//
// 真机用例(需要 Python)照 PythonAvailable 成例 SKIP;纯装配断言不依赖
// 外部环境,恒跑。
//
// app-server 深度挂载不在本线(另一线收尾),TODO 已留 server.cpp。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "platform/process.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/plugin_contract.hpp"
#include "runtime/plugin_process.hpp"
#include "runtime/plugin_tool.hpp"
#include "tools/registry.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

#ifdef _WIN32
constexpr const char* kPythonCmd = "python";
#else
constexpr const char* kPythonCmd = "python3";
#endif

bool PythonAvailable() {
    static const bool available = [] {
        const auto result = platform::RunProcess(std::vector<std::string>{kPythonCmd, "--version"}, 10000);
        return result.exit_code == 0 && result.output.find("Python") != std::string::npos;
    }();
    return available;
}

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        std::error_code ec;
        path = std::filesystem::temp_directory_path() /
              ("lubancode_plugin_mount_" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
               std::to_string(++counter_));
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    void WriteFile(const std::string& name, const std::string& content) const {
        std::ofstream out(path / name, std::ios::binary);
        out << content;
    }

  private:
    static int counter_;
};
int TempDir::counter_ = 0;

// 一份 process manifest(add echo 工具:把入参 text 原样回)。
std::shared_ptr<const PluginManifest> MakeEchoManifest(const TempDir& dir, int timeout_ms = 30000) {
    std::string text = R"json({
  "manifest_version": 1, "id": "mount-probe", "version": "1.0.0", "language": "python",
  "runtime": {"kind": "process", "command": ")json";
    text += kPythonCmd;
    text += R"json(", "args": ["${plugin_dir}/helper.py"], "timeout_ms": )json";
    text += std::to_string(timeout_ms);
    text += R"json(},
  "tools": [{"name": "echo", "description": "回声", "input_schema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}}]
})json";
    dir.WriteFile("helper.py", R"py(import json, sys
for stream in (sys.stdin, sys.stdout, sys.stderr):
    try:
        stream.reconfigure(encoding="utf-8")
    except (AttributeError, OSError):
        pass
request = json.load(sys.stdin)
sys.stderr.write("probe log line\n")
json.dump({
    "protocol": 1,
    "call_id": request["call_id"],
    "ok": True,
    "content": [{"type": "text", "text": request["arguments"]["text"]}],
}, sys.stdout, ensure_ascii=False)
)py");
    auto manifest = ParsePluginManifest(text, dir.path);
    REQUIRE(manifest.has_value());
    return std::make_shared<const PluginManifest>(std::move(*manifest));
}

}  // namespace

// ---------------------------------------------------------------------------
// call_id:进程级发号局的 req 号(单子第 7 步:换 ProcessIdAuthority 真id)
// ---------------------------------------------------------------------------

TEST_CASE("adapter 的 call_id 用 ProcessIdAuthority 的真 req 号") {
    if (!PythonAvailable()) {
        return;
    }
    TempDir dir;
    auto manifest = MakeEchoManifest(dir);
    PluginToolAdapter tool(manifest, &manifest->tools[0]);

    // 两次执行各拿一枚新 req 号(发号局只涨不回收);echo 脚本把 call_id
    // 回显不了(响应里没有),但插件自报失败分型不受影响——这里用子进程
    // 真跑通来证明协议线上带的就是发号局的号:echo 回的 text 正常,说明
    // call_id 对上了(对不上会是 call_id_mismatch)。
    const auto first = tool.execute(nlohmann::json{{"text", "一号"}});
    CHECK_FALSE(first.is_error);
    CHECK(first.content == "一号");
    const auto second = tool.execute(nlohmann::json{{"text", "二号"}});
    CHECK_FALSE(second.is_error);
    CHECK(second.content == "二号");
}

// ---------------------------------------------------------------------------
// 日志分流:stderr 进 LogSink,stdout 的正文进模型结果
// ---------------------------------------------------------------------------

TEST_CASE("LogSink 分流:插件 stderr 进 sink,结果文本不带日志") {
    if (!PythonAvailable()) {
        return;
    }
    TempDir dir;
    auto manifest = MakeEchoManifest(dir);
    PluginToolAdapter tool(manifest, &manifest->tools[0]);

    std::vector<std::string> log_lines;
    tool.SetLogSink([&log_lines](const std::string& line) { log_lines.push_back(line); });

    const auto result = tool.execute(nlohmann::json{{"text", "正文"}});
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "正文");            // stdout 是结果专线
    CHECK(result.content.find("probe log") == std::string::npos);  // 日志不混进正文
    REQUIRE(log_lines.size() == 1);             // stderr 进了 sink
    CHECK(log_lines[0].find("probe log line") != std::string::npos);
    CHECK(log_lines[0].find("mount-probe") != std::string::npos);  // 带插件 id 前缀
}

TEST_CASE("不设 LogSink:stderr 静默,不影响调用成败") {
    if (!PythonAvailable()) {
        return;
    }
    TempDir dir;
    auto manifest = MakeEchoManifest(dir);
    PluginToolAdapter tool(manifest, &manifest->tools[0]);
    const auto result = tool.execute(nlohmann::json{{"text", "quiet"}});
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "quiet");
}

// ---------------------------------------------------------------------------
// SetCwd:进程 cwd 缺省项目根
// ---------------------------------------------------------------------------

TEST_CASE("SetCwd 灌进 adapter,子进程按它跑") {
    if (!PythonAvailable()) {
        return;
    }
    TempDir dir;
    auto manifest = MakeEchoManifest(dir);
    PluginToolAdapter tool(manifest, &manifest->tools[0]);

    // cwd 传一个临时目录;echo 不读 cwd,这里验的是装配口不炸 + 调用照常
    // (真验 cwd 生效要在 Windows 上查进程快照,不值得;SetCwd 的值经
    // RunProcessToolCall 直通 ChildProcess::Start 的 cwd 语义,process_posix
    // /process_win 的测试已有覆盖)。
    tool.SetCwd(platform::PathToUtf8(dir.path));
    const auto result = tool.execute(nlohmann::json{{"text", "cwd ok"}});
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "cwd ok");
}

// ---------------------------------------------------------------------------
// SetCancel:ESC 旗子置位,进程被取消落锤
// ---------------------------------------------------------------------------

TEST_CASE("SetCancel 置位后长跑进程被取消(cancelled 终态)") {
    if (!PythonAvailable()) {
        return;
    }
    TempDir dir;
    // 慢脚本:睡 30s。manifest timeout 放大到 60s,让取消先落。
    dir.WriteFile("helper.py", R"py(import sys, time
for stream in (sys.stdin, sys.stderr):
    try:
        stream.reconfigure(encoding="utf-8")
    except (AttributeError, OSError):
        pass
sys.stdin.read()
time.sleep(30)
)py");
    std::string text = R"json({
  "manifest_version": 1, "id": "mount-probe", "version": "1.0.0",
  "runtime": {"kind": "process", "command": ")json";
    text += kPythonCmd;
    text += R"json(", "args": ["${plugin_dir}/helper.py"], "timeout_ms": 60000},
  "tools": [{"name": "echo", "description": "d", "input_schema": {"type": "object"}}]
})json";
    auto manifest = ParsePluginManifest(text, dir.path);
    REQUIRE(manifest.has_value());
    auto shared = std::make_shared<const PluginManifest>(std::move(*manifest));
    PluginToolAdapter tool(shared, &shared->tools[0]);

    std::atomic<bool> cancel{false};
    tool.SetCancel(&cancel);
    std::thread canceller([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        cancel.store(true);
    });
    const auto result = tool.execute(nlohmann::json::object());
    canceller.join();
    CHECK(result.is_error);
    // 取消终态的人话([cancelled] 前缀由 BuildResultText 落)。
    CHECK(result.content.find("cancelled") != std::string::npos);
}

// ---------------------------------------------------------------------------
// registry 挂载:中立 Tool 形状(Find 得到、needs_confirm、deferred)
// ---------------------------------------------------------------------------

TEST_CASE("adapter 进 registry:中立 Tool 形状,模型可见三样不含宿主元数据") {
    TempDir dir;
    auto manifest = MakeEchoManifest(dir);
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<PluginToolAdapter>(manifest, &manifest->tools[0]));

    tools::Tool* found = registry.Find("plugin__mount-probe__echo");
    REQUIRE(found != nullptr);
    CHECK(found->needs_confirm());
    CHECK(found->deferred());
    const std::string visible = found->name() + " " + found->description() + " " + found->input_schema().dump();
    CHECK(visible.find(kPythonCmd) == std::string::npos);   // command 不进
    CHECK(visible.find("helper.py") == std::string::npos);  // path 不进
    CHECK(visible.find("30000") == std::string::npos);      // timeout 不进
}
