// PTC 受限 runner 的真机单测(P0):假工具 + 真 Python,跑通规格的
// 探针式场景——单调用/真依赖链/八路 fan-out/异常收口/编码穿透,外加
// 护栏、语法错、墙钟、调用数上限、取消链、asyncio.gather 兼容。
//
// 缺 Python 的环境整文件 SKIP(不硬失败——这套是"有 Python 才有意义"的
// 端到端,CI 三平台都有 python/python3)。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <set>
#include <thread>

#include "platform/process.hpp"
#include "ptc/runner.hpp"
#include "ptc/stub_generator.hpp"
#include "tools/registry.hpp"
#include "tools/schema_check.hpp"
#include "tools/tool.hpp"

using namespace lubancode;
using namespace lubancode::ptc;

namespace {

#ifdef _WIN32
constexpr const char* kPythonCmd = "python";
#else
constexpr const char* kPythonCmd = "python3";
#endif

// python 可用性探测:--version 退出码 0 且输出里有 Python。Windows 的
// Microsoft Store 假 shim 退出码非 0/输出是"未安装"提示,正好被挡掉。
bool PythonAvailable() {
    static const bool available = [] {
        const auto result = platform::RunProcess(std::vector<std::string>{kPythonCmd, "--version"}, 10000);
        return result.exit_code == 0 && result.output.find("Python") != std::string::npos;
    }();
    return available;
}

// 假工具:echo 原样回(message 前加 "echo:"),count 计数入参,fail 恒失败。
class EchoTool : public tools::Tool {
public:
    std::string name() const override { return "echo"; }
    std::string description() const override { return "回声测试工具。"; }
    nlohmann::json input_schema() const override {
        return nlohmann::json::parse(
            R"({"type":"object","properties":{"message":{"type":"string","minLength":1}},"required":["message"]})");
    }
    Result execute(const nlohmann::json& input) override {
        return Result{"echo:" + input.at("message").get<std::string>(), false};
    }
};

class FailTool : public tools::Tool {
public:
    std::string name() const override { return "always_fail"; }
    std::string description() const override { return "恒失败工具(异常收口探针)。"; }
    nlohmann::json input_schema() const override {
        return nlohmann::json::parse(R"({"type":"object","properties":{"n":{"type":"integer"}},"required":["n"]})");
    }
    Result execute(const nlohmann::json& input) override {
        (void)input;
        return Result{"boom: 这枚工具永远失败", true};
    }
};

// 假工具注册表 + P0 版执行链(schema 校验 + 执行;hooks/权限链在 P1 的
// PtcTool 测试里接)。
tools::ToolRegistry BuildFakeRegistry() {
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<EchoTool>());
    registry.Register(std::make_unique<FailTool>());
    return registry;
}

tools::Tool::Result ExecuteViaRegistry(tools::ToolRegistry& registry, const std::string& name,
                                       const nlohmann::json& input) {
    tools::Tool* tool = registry.Find(name);
    if (tool == nullptr) {
        return tools::Tool::Result{"未知工具: " + name, true};
    }
    const auto schema_error = tools::ValidateInputAgainstSchema(input, tool->input_schema());
    if (schema_error.has_value()) {
        return tools::Tool::Result{*schema_error, true};
    }
    return tool->execute(input);
}

// 测试脚手架:一份假表 + 生成好的 stub 模块 + 默认宽松限额。
struct Fixture {
    tools::ToolRegistry registry = BuildFakeRegistry();
    std::string stub_python;

    Fixture() {
        std::vector<StubToolInfo> infos;
        for (const auto& tool : registry.All()) {
            StubToolInfo info;
            info.definition.name = tool->name();
            info.definition.description = tool->description();
            info.definition.input_schema = tool->input_schema();
            info.needs_confirm = tool->needs_confirm();
            info.parallel_safe = true;
            infos.push_back(std::move(info));
        }
        stub_python = GenerateStubModule(infos, StubMode::Full).python_source;
    }

    PtcRunner::Options Options() {
        PtcRunner::Options options;
        options.python_cmd = kPythonCmd;
        options.executor = [this](const std::string& name, const nlohmann::json& input) {
            return ExecuteViaRegistry(registry, name, input);
        };
        options.limits.wall_clock_ms = 30000;
        options.limits.cpu_ms = 60000;
        options.limits.max_calls = 100;
        return options;
    }
};

}  // namespace

// ---- 探针五项(P0 离线版:直接喂手写脚本,验证 runner/协议/审计) ----

TEST_CASE("探针1 单调用: 参数与结果都对,审计账有 hash") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    Fixture fixture;
    const char* script = R"PY(
from luban_tools import echo
r = echo(message="你好 \\ \"引号\"")
emit({"got": r["content"]})
)PY";
    const auto result = PtcRunner::Run(script, fixture.stub_python, fixture.Options());
    REQUIRE(result.ok);
    REQUIRE(result.calls.size() == 1);
    CHECK(result.calls[0].tool == "echo");
    CHECK(result.calls[0].input.at("message") == "你好 \\ \"引号\"");
    CHECK(result.calls[0].ok);
    CHECK_FALSE(result.calls[0].is_error);
    CHECK_FALSE(result.calls[0].input_hash.empty());
    CHECK(result.emit_value.at("got") == "echo:你好 \\ \"引号\"");
    CHECK(result.python_version.find("3.") == 0);
}

TEST_CASE("探针2 真依赖链: 第一枚的真实返回值喂第二枚,不靠常识猜") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    Fixture fixture;
    const char* script = R"PY(
from luban_tools import echo
first = echo(message="seed")
second = echo(message=first["content"] + "-chained")
emit({"second": second["content"]})
)PY";
    const auto result = PtcRunner::Run(script, fixture.stub_python, fixture.Options());
    REQUIRE(result.ok);
    REQUIRE(result.calls.size() == 2);
    // 第二枚的入参必须来自第一枚的真实返回值(echo: 前缀只有真跑过才会有)。
    CHECK(result.calls[1].input.at("message") == "echo:seed-chained");
    CHECK(result.emit_value.at("second") == "echo:echo:seed-chained");
}

TEST_CASE("探针3 八路 fan-out: 不漏不重,id 唯一") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    Fixture fixture;
    const char* script = R"PY(
from luban_tools import echo
rs = [echo(message="f" + str(i)) for i in range(8)]
emit({"count": len(rs), "all": [r["content"] for r in rs]})
)PY";
    const auto result = PtcRunner::Run(script, fixture.stub_python, fixture.Options());
    REQUIRE(result.ok);
    REQUIRE(result.calls.size() == 8);
    std::set<std::string> messages;
    std::set<std::uint64_t> ids;
    for (const auto& call : result.calls) {
        messages.insert(call.input.at("message").get<std::string>());
        ids.insert(call.id);
    }
    CHECK(ids.size() == 8);
    REQUIRE(messages.size() == 8);
    for (int i = 0; i < 8; ++i) {
        CHECK(messages.count("f" + std::to_string(i)) == 1);
    }
    CHECK(result.emit_value.at("count") == 8);
}

TEST_CASE("探针4 异常收口: 一枚失败,其余与摘要仍有账") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    Fixture fixture;
    const char* script = R"PY(
from luban_tools import echo, always_fail, ToolCallError
errors = []
try:
    always_fail(n=1)
except ToolCallError as exc:
    errors.append(str(exc))
ok_calls = [echo(message="still-" + str(i)) for i in range(2)]
emit({"errors": errors, "ok_count": len(ok_calls)})
)PY";
    const auto result = PtcRunner::Run(script, fixture.stub_python, fixture.Options());
    REQUIRE(result.ok);
    REQUIRE(result.calls.size() == 3);
    CHECK(result.calls[0].tool == "always_fail");
    CHECK(result.calls[0].is_error);
    CHECK(result.calls[0].error == "工具层失败");
    CHECK(result.emit_value.at("ok_count") == 2);
    CHECK(result.emit_value.at("errors").size() == 1);
    // 失败信息带工具名与原文。
    const std::string error_text = result.emit_value.at("errors")[0].get<std::string>();
    CHECK(error_text.find("always_fail") != std::string::npos);
    CHECK(error_text.find("boom") != std::string::npos);
}

TEST_CASE("探针5 编码: 中文/emoji/反斜杠/引号/真实换行穿透脚本与 RPC") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    Fixture fixture;
    const char* script =
        R"PY(
from luban_tools import echo
tricky = "中文🎉 反斜杠\\ 双引号\" 单引号' 换行\n制表\t"
r = echo(message=tricky)
emit({"roundtrip": r["content"] == "echo:" + tricky, "len": len(r["content"])})
)PY";
    const auto result = PtcRunner::Run(script, fixture.stub_python, fixture.Options());
    REQUIRE(result.ok);
    CHECK(result.emit_value.at("roundtrip") == true);
}

// ---- 护栏与失败分型 ----

TEST_CASE("护栏: import os 被拒,stage=guard(沙箱拒绝)") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    Fixture fixture;
    const char* script = "import os\nemit({'env': dict(os.environ)})\n";
    const auto result = PtcRunner::Run(script, fixture.stub_python, fixture.Options());
    REQUIRE_FALSE(result.ok);
    CHECK(result.failure == PtcFailure::Guard);
    CHECK(result.stage == "guard");
    CHECK(result.ZeroCallsHappened());  // 零调用:可自动回退 JSON 一次
}

TEST_CASE("护栏: socket 一样被拒(禁网络)") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    Fixture fixture;
    const char* script = "import socket\nemit({'s': 1})\n";
    const auto result = PtcRunner::Run(script, fixture.stub_python, fixture.Options());
    REQUIRE_FALSE(result.ok);
    CHECK(result.failure == PtcFailure::Guard);
}

TEST_CASE("语法错: stage=syntax,零调用,traceback 有行号") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    Fixture fixture;
    const char* script = "def broken(:\n    pass\n";
    const auto result = PtcRunner::Run(script, fixture.stub_python, fixture.Options());
    REQUIRE_FALSE(result.ok);
    CHECK(result.failure == PtcFailure::Syntax);
    CHECK(result.ZeroCallsHappened());
    CHECK(result.error.find("SyntaxError") != std::string::npos);
}

TEST_CASE("运行时错: stage=runtime,traceback 带异常名") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    Fixture fixture;
    const char* script = "x = 1 / 0\nemit({'x': x})\n";
    const auto result = PtcRunner::Run(script, fixture.stub_python, fixture.Options());
    REQUIRE_FALSE(result.ok);
    CHECK(result.failure == PtcFailure::Runtime);
    CHECK(result.traceback.find("ZeroDivisionError") != std::string::npos);
    CHECK(result.ZeroCallsHappened());
}

TEST_CASE("漏 emit: 明确报协议约定,不算成功") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    Fixture fixture;
    const char* script = "from luban_tools import echo\nr = echo(message='x')\n";
    const auto result = PtcRunner::Run(script, fixture.stub_python, fixture.Options());
    REQUIRE_FALSE(result.ok);
    CHECK(result.failure == PtcFailure::Runtime);
    CHECK(result.error.find("emit") != std::string::npos);
    // 只读调用已发生:不可盲重放,但可复用结果。
    CHECK(result.HasReadOnlyCalls());
}

TEST_CASE("emit 两次: 拒绝并按运行时错收场") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    Fixture fixture;
    const char* script = "from luban_tools import emit\nemit({'a': 1})\nemit({'b': 2})\n";
    const auto result = PtcRunner::Run(script, fixture.stub_python, fixture.Options());
    REQUIRE_FALSE(result.ok);
    CHECK(result.failure == PtcFailure::Runtime);
    CHECK(result.traceback.find("emit()") != std::string::npos);
}

TEST_CASE("未知工具: 宿主链回错,脚本可收口") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    Fixture fixture;
    const char* script = R"PY(
from luban_tools import echo, ToolCallError
try:
    echo(message="hi")
except ToolCallError:
    pass
emit({"a": 1})
)PY";
    // 正常工具先跑一枚,再用假名直接 call_tool(绕过 stub)触发未知工具。
    const char* script2 = R"PY(
from luban_tools import echo
from ptc_runtime import call_tool, ToolCallError
echo(message="ok")
try:
    call_tool("no_such_tool", {})
except ToolCallError as exc:
    emit({"unknown_error": str(exc)})
)PY";
    const auto ok_result = PtcRunner::Run(script, fixture.stub_python, fixture.Options());
    CHECK(ok_result.ok);
    const auto result = PtcRunner::Run(script2, fixture.stub_python, fixture.Options());
    REQUIRE(result.ok);
    REQUIRE(result.calls.size() == 2);
    CHECK(result.calls[1].tool == "no_such_tool");
    CHECK(result.calls[1].is_error);
    CHECK(result.emit_value.at("unknown_error").get<std::string>().find("未知工具") != std::string::npos);
}

// ---- 五道上限 ----

TEST_CASE("墙钟上限: 死循环脚本被杀,failure 指名哪道墙") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    Fixture fixture;
    auto options = fixture.Options();
    options.limits.wall_clock_ms = 2000;
    options.limits.cpu_ms = 60000;
    const char* script = "x = 0\nwhile True:\n    x += 1\n";
    const auto result = PtcRunner::Run(script, fixture.stub_python, options);
    REQUIRE_FALSE(result.ok);
    CHECK(result.failure == PtcFailure::LimitWallClock);
    CHECK(result.error.find("墙钟") != std::string::npos);
    CHECK(result.elapsed_ms < 15000);  // 收场及时,没吊死
}

TEST_CASE("调用数上限: 超限调用被点名拒绝,脚本收口后整轮仍算完成") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    Fixture fixture;
    auto options = fixture.Options();
    options.limits.max_calls = 2;
    const char* script = R"PY(
from luban_tools import echo, ToolCallError
done = 0
rejected = 0
for i in range(5):
    try:
        echo(message="c" + str(i))
        done += 1
    except ToolCallError:
        rejected += 1
emit({"done": done, "rejected": rejected})
)PY";
    const auto result = PtcRunner::Run(script, fixture.stub_python, options);
    REQUIRE(result.ok);  // done 收口:撞墙但收了尾
    REQUIRE(result.calls.size() == 5);
    int executed = 0;
    int rejected = 0;
    for (const auto& call : result.calls) {
        if (call.ok) {
            ++executed;
        } else {
            ++rejected;
            CHECK(call.error.find("调用数上限") != std::string::npos);
        }
    }
    CHECK(executed == 2);
    CHECK(rejected == 3);
    CHECK(result.emit_value.at("rejected") == 3);
}

TEST_CASE("取消链(Esc): 未开始的调用回'已取消',不再进执行链") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    Fixture fixture;
    auto options = fixture.Options();
    std::atomic<bool> cancel{true};  // 预先置位:第一枚调用就该被拒
    options.cancel = &cancel;
    const char* script = R"PY(
from luban_tools import echo, ToolCallError
try:
    echo(message="never")
except ToolCallError:
    pass
emit({"after": "cancel"})
)PY";
    const auto result = PtcRunner::Run(script, fixture.stub_python, options);
    REQUIRE_FALSE(result.ok);
    CHECK(result.failure == PtcFailure::Cancelled);
    REQUIRE(result.calls.size() == 1);
    CHECK_FALSE(result.calls[0].ok);
    CHECK(result.calls[0].error == "取消(未开始)");
}

// ---- 兼容与杂项 ----

TEST_CASE("asyncio.gather 兼容: ToolResult 可 await,fan-out 结果齐") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    Fixture fixture;
    const char* script = R"PY(
import asyncio
from luban_tools import echo

async def main():
    rs = await asyncio.gather(*[echo(message="a" + str(i)) for i in range(4)])
    emit({"n": len(rs), "first": rs[0]["content"]})
)PY";
    const auto result = PtcRunner::Run(script, fixture.stub_python, fixture.Options());
    REQUIRE(result.ok);
    REQUIRE(result.calls.size() == 4);
    CHECK(result.emit_value.at("n") == 4);
    CHECK(result.emit_value.at("first") == "echo:a0");
}

TEST_CASE("print 被捕获进 done 帧,不污染 RPC 通道") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:整条跳过(doctest 2.4.12 无 SKIP 宏)
    }
    Fixture fixture;
    const char* script = "print('hello print 中文')\nfrom luban_tools import emit\nemit({'a': 1})\n";
    const auto result = PtcRunner::Run(script, fixture.stub_python, fixture.Options());
    REQUIRE(result.ok);
    CHECK(result.captured_stdout.find("hello print 中文") != std::string::npos);
}

TEST_CASE("起不来: 解释器不存在报 Spawn,错误带命令名") {
    Fixture fixture;
    auto options = fixture.Options();
    options.python_cmd = "definitely-not-a-python-interpreter";
    const char* script = "from luban_tools import emit\nemit({'a': 1})\n";
    const auto result = PtcRunner::Run(script, fixture.stub_python, options);
    REQUIRE_FALSE(result.ok);
    CHECK(result.failure == PtcFailure::Spawn);
    CHECK(result.error.find("definitely-not-a-python-interpreter") != std::string::npos);
}

TEST_CASE("StableHash/NewRunId/文本化: 基本形状") {
    CHECK(PtcRunner::NewRunId().substr(0, 4) == "ptc-");
    CHECK(PtcRunner::NewRunId().size() == 12);
    CHECK(PtcRunner::NewRunId() != PtcRunner::NewRunId());
    // 规范化:键序不同不影响 hash。
    CHECK(PtcRunner::StableHash(nlohmann::json::parse(R"({"a":1,"b":2})")) ==
          PtcRunner::StableHash(nlohmann::json::parse(R"({"b":2,"a":1})")));
    CHECK(PtcRunner::StableHash(nlohmann::json::parse(R"({"a":1,"b":2})")) !=
          PtcRunner::StableHash(nlohmann::json::parse(R"({"a":1,"b":3})")));
    CHECK(PtcRunner::FailureText(PtcFailure::LimitWallClock).find("墙钟") != std::string::npos);
}
