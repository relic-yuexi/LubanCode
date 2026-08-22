// plugins 运行时第 2 步(process v1)的真机单测:自带测试 helper(测试
// 自己写的小 Python 脚本),覆盖单子「验收」里的终态矩阵——
//   正常往返 / 嵌套 object+中文+多行不变形 / stdout 混日志 / 坏 JSON /
//   call_id 不合 / 超时 / ESC 取消 / 非零退出 / stderr 塞满 / 输出超限
// 每种死法各有唯一宿主错误码;宿主不死、不挂、不留孤儿进程。
//
// 缺 Python 的环境整文件 SKIP(照 test_ptc_runner.cpp 的 PythonAvailable()
// 成例:探测 --version 退出码 + 输出内容,Windows 的 Microsoft Store 假
// shim 正好被挡掉)。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "platform/process.hpp"
#include "runtime/plugin_contract.hpp"
#include "runtime/plugin_process.hpp"

using namespace lubancode;
using namespace lubancode::runtime;

namespace {

#ifdef _WIN32
constexpr const char* kPythonCmd = "python";
#else
constexpr const char* kPythonCmd = "python3";
#endif

// 照 test_ptc_runner.cpp 的成例:--version 退出码 0 且输出里有 Python。
bool PythonAvailable() {
    static const bool available = [] {
        const auto result = platform::RunProcess(std::vector<std::string>{kPythonCmd, "--version"}, 10000);
        return result.exit_code == 0 && result.output.find("Python") != std::string::npos;
    }();
    return available;
}

// 临时插件目录:落 helper 脚本 + plugin.json,收尾自删。Windows 文件柄的
// 规矩:写文件的流在析构里先关,remove_all 用 error_code 形态。
struct TempPluginDir {
    std::filesystem::path path;
    TempPluginDir() {
        std::error_code ec;
        path = std::filesystem::temp_directory_path() /
              ("lubancode_plugin_proc_" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
               std::to_string(++counter_));
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempPluginDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    void WriteFile(const std::string& name, const std::string& content) const {
        {
            std::ofstream out(path / name, std::ios::binary);
            out << content;
        }
    }

  private:
    static int counter_;
};
int TempPluginDir::counter_ = 0;

// 万能 helper:读 stdin 的协议请求,按 script 里的 Python 表达式产出响应。
// response_builder 是一段 Python,变量 request 在作用域里;把响应 dict 用
// json.dump 写到 stdout。日志(测试混日志场景)由脚本自己往 stdout/stderr
// 写——RunProcessToolCall 的解析层负责判协议错。
const char* kHelperScript = R"py(
import json, sys

sys.stdin.reconfigure(encoding="utf-8")
sys.stdout.reconfigure(encoding="utf-8")
sys.stderr.reconfigure(encoding="utf-8")

request = json.load(sys.stdin)
response = None
try:
    response = build_response(request)
except Exception as error:
    response = {
        "protocol": 1,
        "call_id": request.get("call_id", ""),
        "ok": False,
        "error": {"code": "execution_failed", "message": str(error)},
    }
json.dump(response, sys.stdout, ensure_ascii=False)
sys.stdout.flush()
)py";

// manifest 拼装:kPythonCmd 不是编译期常量,raw string 拼不了,统一走这。
std::string BuildManifestText(int timeout_ms) {
    std::string text = R"json({
      "manifest_version": 1, "id": "probe", "version": "1.0.0", "language": "python",
      "runtime": {"kind": "process", "command": ")json";
    text += kPythonCmd;
    text += R"json(", "args": ["${plugin_dir}/helper.py"], "timeout_ms": )json";
    text += std::to_string(timeout_ms);
    text += R"json(},
      "tools": [{"name": "echo", "description": "d", "input_schema": {"type": "object"}}]
    })json";
    return text;
}

// 拼一份 manifest + helper 脚本。script_python 是 build_response 的完整
// 定义(def build_response(request): ...),放在骨架前面——Python 从上往下
// 执行,定义得先于调用。
PluginManifest MakeProcessManifest(const TempPluginDir& dir, const std::string& script_python, int timeout_ms) {
    dir.WriteFile("helper.py", script_python + "\n" + kHelperScript);
    auto manifest = ParsePluginManifest(BuildManifestText(timeout_ms), dir.path);
    if (!manifest.has_value()) {
        INFO(manifest.error());  // 解析失败时亮出原因(版本/标识符/路径圈禁)
        REQUIRE(manifest.has_value());
    }
    return std::move(*manifest);
}

plugin_protocol::ProcessRequest MakeRequest(const nlohmann::json& arguments) {
    plugin_protocol::ProcessRequest request;
    request.plugin = "probe";
    request.tool = "echo";
    request.entry = "echo";
    request.arguments = arguments;
    request.call_id = "call_test";
    request.context_cwd = "D:/not-a-real-dir";
    return request;
}

}  // namespace

// ---------------------------------------------------------------------------
// 终态矩阵(验收三样的第 2 样)
// ---------------------------------------------------------------------------

TEST_CASE("process v1:正常往返——嵌套 object/中文/多行字符串不变形") {
    if (!PythonAvailable()) {
        return;
    }
    TempPluginDir dir;
    // 回声:把 arguments 原样放 structured,文本给序列化串。
    const auto manifest = MakeProcessManifest(
        dir, R"py(def build_response(request):
    return {
        "protocol": 1,
        "call_id": request["call_id"],
        "ok": True,
        "content": [{"type": "text", "text": json.dumps(request["arguments"], ensure_ascii=False)}],
        "structured": request["arguments"],
    })py",
        /*timeout_ms=*/30000);
    const nlohmann::json arguments = nlohmann::json{
        {"text", "中文往返\n第二行\t第三列"},
        {"nested", {{"list", {1, 2, 3}}, {"flag", true}, {"nothing", nullptr}, {"quote", "a\"b\\c;d&f|g"}}},
        {"deep", {{{"k", "v"}, {"n", -1.5}}, {{"k", "w"}, {"n", 2}}}},
    };
    const auto outcome = RunProcessToolCall(manifest, MakeRequest(arguments), "D:/tmp", nullptr,
                                            ProcessCallLimits{});
    if (outcome.code != PluginErrorCode::Ok) {
        const std::string why = outcome.detail + " | stderr=" + outcome.stderr_tail.substr(0, 300);
        INFO(why);  // 失败时把宿主侧 detail 与 stderr 尾巴亮出来,排查不用猜
        REQUIRE(outcome.code == PluginErrorCode::Ok);
    }
    CHECK_FALSE(outcome.text.empty());
    // 文本往返解回 JSON,与原入参全等——证明 stdin/stdout 两个方向都没变形。
    CHECK(nlohmann::json::parse(outcome.text) == arguments);
    CHECK(outcome.structured == arguments);
}

TEST_CASE("process v1:插件自报失败——ok=false 的错误码原样透传") {
    if (!PythonAvailable()) {
        return;
    }
    TempPluginDir dir;
    const auto manifest = MakeProcessManifest(
        dir, R"py(def build_response(request):
    return {
        "protocol": 1,
        "call_id": request["call_id"],
        "ok": False,
        "error": {"code": "bad_input", "message": "a must be a number"},
    })py",
        30000);
    const auto outcome = RunProcessToolCall(manifest, MakeRequest(nlohmann::json{{"a", "x"}}), "D:/tmp", nullptr,
                                            ProcessCallLimits{});
    REQUIRE(outcome.code == PluginErrorCode::PluginReportedError);
    CHECK(outcome.plugin_error_code == "bad_input");
    CHECK(outcome.detail.find("a must be a number") != std::string::npos);
}

TEST_CASE("process v1:stdout 混日志 = 协议错 bad_json,不从字堆里猜 JSON") {
    if (!PythonAvailable()) {
        return;
    }
    TempPluginDir dir;
    // 先打一行日志再写 JSON——协议线即坏。
    const auto manifest = MakeProcessManifest(
        dir, R"py(def build_response(request):
    print("starting up...")
    print("another log line", file=sys.stderr)
    return {
        "protocol": 1,
        "call_id": request["call_id"],
        "ok": True,
        "content": [{"type": "text", "text": "hi"}],
    })py",
        30000);
    std::string stderr_tail;
    const auto outcome = RunProcessToolCall(manifest, MakeRequest(nlohmann::json::object()), "D:/tmp", nullptr,
                                            ProcessCallLimits{}, &stderr_tail);
    if (outcome.code != PluginErrorCode::BadJson) {
        INFO(outcome.detail);  // 失败时亮出宿主侧 detail
        REQUIRE(outcome.code == PluginErrorCode::BadJson);
    }
    // stderr 的日志照攒(它是专线,不算协议错的一部分)
    CHECK(stderr_tail.find("another log line") != std::string::npos);
}

TEST_CASE("process v1:坏 JSON / call_id 不合各有唯一码") {
    if (!PythonAvailable()) {
        return;
    }
    TempPluginDir dir;
    // 手写 stdout,不走 helper 的 json.dump——直接吐坏 JSON。
    dir.WriteFile("helper.py", R"py(import sys
sys.stdin.read()
sys.stdout.write("this is not json")
)py");
    const std::string manifest_text = BuildManifestText(30000);
    auto manifest = ParsePluginManifest(manifest_text, dir.path);
    REQUIRE(manifest.has_value());
    auto outcome = RunProcessToolCall(*manifest, MakeRequest(nlohmann::json::object()), "D:/tmp", nullptr,
                                      ProcessCallLimits{});
    REQUIRE(outcome.code == PluginErrorCode::BadJson);

    // call_id 不合:吐一份合法 JSON 但回错 id。
    TempPluginDir dir2;
    dir2.WriteFile("helper.py", R"py(import json, sys
request = json.load(sys.stdin)
json.dump({
    "protocol": 1,
    "call_id": "call_somebody_else",
    "ok": True,
    "content": [{"type": "text", "text": "x"}],
}, sys.stdout)
)py");
    auto manifest2 = ParsePluginManifest(BuildManifestText(30000), dir2.path);
    REQUIRE(manifest2.has_value());
    outcome = RunProcessToolCall(*manifest2, MakeRequest(nlohmann::json::object()), "D:/tmp", nullptr,
                                 ProcessCallLimits{});
    REQUIRE(outcome.code == PluginErrorCode::CallIdMismatch);
}

TEST_CASE("process v1:非零退出 = tool_exit_non_zero,stderr 尾巴带在 detail 里") {
    if (!PythonAvailable()) {
        return;
    }
    TempPluginDir dir;
    dir.WriteFile("helper.py", R"py(import sys
sys.stderr.reconfigure(encoding="utf-8")  # 管道下 Python 默认走本地代码页,中文会变 '?'——钉死 UTF-8
sys.stdin.read()
sys.stderr.write("爆炸现场日志\n")
sys.exit(3)
)py");
    auto manifest = ParsePluginManifest(BuildManifestText(30000), dir.path);
    REQUIRE(manifest.has_value());
    const auto outcome = RunProcessToolCall(*manifest, MakeRequest(nlohmann::json::object()), "D:/tmp", nullptr,
                                            ProcessCallLimits{});
    REQUIRE(outcome.code == PluginErrorCode::ToolExitNonZero);
    CHECK(outcome.exit_code == 3);
    CHECK(outcome.detail.find("爆炸现场日志") != std::string::npos);
}

TEST_CASE("process v1:超时 = timed_out,进程树收干净(不吊死宿主)") {
    if (!PythonAvailable()) {
        return;
    }
    TempPluginDir dir;
    dir.WriteFile("helper.py", R"py(import sys, time
sys.stdin.read()
time.sleep(30)
)py");
    auto manifest = ParsePluginManifest(BuildManifestText(1500), dir.path);
    REQUIRE(manifest.has_value());
    const auto started = std::chrono::steady_clock::now();
    const auto outcome = RunProcessToolCall(*manifest, MakeRequest(nlohmann::json::object()), "D:/tmp", nullptr,
                                            ProcessCallLimits{});
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                started)
                             .count();
    REQUIRE(outcome.code == PluginErrorCode::TimedOut);
    // 落锤有界:超时 1.5s + grace 2s,总时长远小于脚本的 30s。
    CHECK(elapsed < 15000);
}

TEST_CASE("process v1:ESC 取消 = cancelled(与超时分型独立)") {
    if (!PythonAvailable()) {
        return;
    }
    TempPluginDir dir;
    dir.WriteFile("helper.py", R"py(import sys, time
sys.stdin.read()
time.sleep(30)
)py");
    auto manifest = ParsePluginManifest(BuildManifestText(60000), dir.path);
    REQUIRE(manifest.has_value());

    std::atomic<bool> cancel{false};
    std::thread canceller([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        cancel.store(true);
    });
    const auto outcome = RunProcessToolCall(*manifest, MakeRequest(nlohmann::json::object()), "D:/tmp", &cancel,
                                            ProcessCallLimits{});
    canceller.join();
    REQUIRE(outcome.code == PluginErrorCode::Cancelled);
}

TEST_CASE("process v1:stderr 塞满不致死锁——日志照攒、帽内截断、调用照常收场") {
    if (!PythonAvailable()) {
        return;
    }
    TempPluginDir dir;
    // 吐 5MB stderr(远超管道缓冲 64KB;若不排水,子进程会卡在写上,
    // 双方死锁——这正是三管异步排水要防的)。
    dir.WriteFile("helper.py", R"py(import json, sys
request = json.load(sys.stdin)
for i in range(5000):
    sys.stderr.write("x" * 1024 + "\n")
sys.stdout.flush()
json.dump({
    "protocol": 1,
    "call_id": request["call_id"],
    "ok": True,
    "content": [{"type": "text", "text": "done"}],
}, sys.stdout)
)py");
    auto manifest = ParsePluginManifest(BuildManifestText(60000), dir.path);
    REQUIRE(manifest.has_value());
    ProcessCallLimits limits;
    limits.stderr_cap_bytes = 64 * 1024;  // 帽 64KB,截断保留前段
    std::string stderr_tail;
    const auto outcome = RunProcessToolCall(*manifest, MakeRequest(nlohmann::json::object()), "D:/tmp", nullptr,
                                            limits, &stderr_tail);
    REQUIRE(outcome.code == PluginErrorCode::Ok);
    CHECK(outcome.text == "done");
    CHECK(stderr_tail.size() == limits.stderr_cap_bytes);  // 截到帽
}

TEST_CASE("process v1:stdout 输出超限 = output_too_large,停读杀进程") {
    if (!PythonAvailable()) {
        return;
    }
    TempPluginDir dir;
    // 吐 5MB stdout,帽设 64KB:读到帽即停读、杀树,不吃进几百 MiB。
    dir.WriteFile("helper.py", R"py(import sys
sys.stdin.read()
for i in range(5000):
    sys.stdout.write("y" * 1024 + "\n")
)py");
    auto manifest = ParsePluginManifest(BuildManifestText(60000), dir.path);
    REQUIRE(manifest.has_value());
    ProcessCallLimits limits;
    limits.stdout_cap_bytes = 64 * 1024;
    const auto started = std::chrono::steady_clock::now();
    const auto outcome = RunProcessToolCall(*manifest, MakeRequest(nlohmann::json::object()), "D:/tmp", nullptr,
                                            limits);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                started)
                             .count();
    REQUIRE(outcome.code == PluginErrorCode::OutputTooLarge);
    CHECK(elapsed < 15000);  // 有界落锤,不等脚本吐完 5MB
}

TEST_CASE("process v1:起不来(spawn_failed)与请求超 stdin 帽(output_too_large)") {
    // 命令不存在:spawn_failed。
    {
        TempPluginDir dir;
        const std::string manifest_text = R"json({
          "manifest_version": 1, "id": "probe", "version": "1",
          "runtime": {"kind": "process", "command": "no_such_interp_for_lubancode_test"},
          "tools": [{"name": "echo", "description": "d", "input_schema": {"type": "object"}}]
        })json";
        auto manifest = ParsePluginManifest(manifest_text, dir.path);
        REQUIRE(manifest.has_value());
        const auto outcome = RunProcessToolCall(*manifest, MakeRequest(nlohmann::json::object()), "D:/tmp", nullptr,
                                                ProcessCallLimits{});
        REQUIRE(outcome.code == PluginErrorCode::SpawnFailed);
    }
    // 请求帧超 stdin 帽:发都不发。
    {
        TempPluginDir dir;
        const auto manifest = MakeProcessManifest(
            dir, R"py(def build_response(request):
    return {"protocol": 1, "call_id": request["call_id"], "ok": True, "content": []})py", 30000);
        ProcessCallLimits limits;
        limits.stdin_cap_bytes = 16;  // 请求帧必超
        const auto outcome =
            RunProcessToolCall(manifest, MakeRequest(nlohmann::json{{"payload", std::string(512, 'z')}}), "D:/tmp",
                               nullptr, limits);
        REQUIRE(outcome.code == PluginErrorCode::OutputTooLarge);
    }
}

TEST_CASE("process v1:带空格/中文/引号的路径与参数不经 shell,原样抵达") {
    if (!PythonAvailable()) {
        return;
    }
    TempPluginDir dir;
    // 参数里塞齐 shell 元字符;回声脚本把它们原样带回来,证明没有一条
    // shell 路把它们当命令跑了。
    const auto manifest = MakeProcessManifest(
        dir, R"py(def build_response(request):
    text = request["arguments"]["payload"]
    return {
        "protocol": 1,
        "call_id": request["call_id"],
        "ok": True,
        "content": [{"type": "text", "text": text}],
    })py",
        30000);
    const std::string nasty = "a b&c;d|e<f>g\"h'i`j$(k)l*m?n[o]p{q}r\\s;t~u^v!w#x%y$z 中文 空格";
    const auto outcome = RunProcessToolCall(manifest, MakeRequest(nlohmann::json{{"payload", nasty}}), "D:/tmp",
                                            nullptr, ProcessCallLimits{});
    REQUIRE(outcome.code == PluginErrorCode::Ok);
    CHECK(outcome.text == nasty);
}

TEST_CASE("process v1:退出后不留孤儿——同一目录连续多轮调用,每轮进程都收干净") {
    if (!PythonAvailable()) {
        return;
    }
    TempPluginDir dir;
    const auto manifest = MakeProcessManifest(
        dir, R"py(def build_response(request):
    return {
        "protocol": 1,
        "call_id": request["call_id"],
        "ok": True,
        "content": [{"type": "text", "text": "ok"}],
    })py",
        30000);
    for (int i = 0; i < 5; ++i) {
        const auto outcome = RunProcessToolCall(manifest, MakeRequest(nlohmann::json::object()), "D:/tmp", nullptr,
                                                ProcessCallLimits{});
        REQUIRE(outcome.code == PluginErrorCode::Ok);
    }
    // 没有可移植的"数孤儿"办法;连续五轮都快速收场本身就是"进程都收干净"
    // 的行为证明(挂了一轮,后续调用会在管道句柄上出问题或超时)。
}
