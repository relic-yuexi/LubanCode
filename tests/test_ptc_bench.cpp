// LubanBench-Tool 离线层单测:八类场景题库齐全、离线判卷指标正确、JSON
// 报告可读。在线层(同模型 JSON vs PTC 各五轮)须真 API,见手测清单。

#include <doctest/doctest.h>

#include <algorithm>
#include <set>
#include <vector>

#include "platform/process.hpp"
#include "ptc/bench.hpp"
#include "ptc/profile.hpp"
#include "ptc/runner.hpp"
#include "ptc/stub_generator.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;
using namespace lubancode::ptc;

namespace {

#ifdef _WIN32
constexpr const char* kPythonCmd = "python";
#else
constexpr const char* kPythonCmd = "python3";
#endif

// 诊断用本地回声假工具(bench.cpp 里那份在匿名命名空间)。
class LocalEchoTool : public lubancode::tools::Tool {
public:
    std::string name() const override { return "echo"; }
    std::string description() const override { return "回声"; }
    nlohmann::json input_schema() const override {
        return nlohmann::json::parse(
            R"({"type":"object","properties":{"message":{"type":"string"}},"required":["message"]})");
    }
    Result execute(const nlohmann::json& input) override {
        return Result{"echo:" + input.at("message").get<std::string>(), false};
    }
};

bool PythonAvailable() {
    static const bool available = [] {
        const auto result =
            lubancode::platform::RunProcess(std::vector<std::string>{kPythonCmd, "--version"}, 10000);
        return result.exit_code == 0 && result.output.find("Python") != std::string::npos;
    }();
    return available;
}

PtcRunner::Options BenchOptions() {
    PtcRunner::Options options;
    options.python_cmd = kPythonCmd;
    options.limits.wall_clock_ms = 30000;
    options.limits.cpu_ms = 60000;
    return options;
}

}  // namespace

TEST_CASE("题库: 八类场景齐全,每类至少一道") {
    const auto scenarios = BuildBenchScenarios();
    REQUIRE_FALSE(scenarios.empty());
    std::set<std::string> kinds;  // doctest 环境没有 <set> 依赖问题——pch 里有
    for (const auto& scenario : scenarios) {
        REQUIRE_FALSE(scenario.id.empty());
        REQUIRE_FALSE(scenario.prompt.empty());
        kinds.insert(ToString(scenario.kind));
    }
    CHECK(kinds.count("single_tool_short_params") == 1);
    CHECK(kinds.count("chain_dependency") == 1);
    CHECK(kinds.count("fanout_independent") == 1);
    CHECK(kinds.count("needle_in_haystack") == 1);
    CHECK(kinds.count("large_result_summary") == 1);
    CHECK(kinds.count("half_failures") == 1);
    CHECK(kinds.count("mixed_readonly_side_effects") == 1);
    CHECK(kinds.count("extension_cross") == 1);
    CHECK(kinds.size() == 8);
    // 编号唯一。
    std::set<std::string> ids;
    for (const auto& scenario : scenarios) {
        ids.insert(scenario.id);
    }
    CHECK(ids.size() == scenarios.size());
}

TEST_CASE("离线判卷: 单工具/链/fan-out/针在草堆/大结果/一半失败 全过线") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:跳过
    }
    // 判卷器直连版:同一条 RunScenarioOffline 的链,但把 run 明细也带出来
    // 方便诊断。复刻 RunScenarioOffline 的装配。
    const auto scenarios = BuildBenchScenarios();
    std::vector<BenchMetrics> all_metrics;
    for (const auto& scenario : scenarios) {
        const auto metrics = RunScenarioOffline(scenario, BenchOptions());
        all_metrics.push_back(metrics);
        if (scenario.kind == BenchKind::ExtensionCross || scenario.kind == BenchKind::MixedReadOnlySideEffects) {
            continue;  // 这两类在线层才有肉,离线只验形状(能跑能判)。
        }
        INFO("scenario: ", scenario.id, " task_success=", metrics.task_success,
             " missed=", metrics.missed_calls, " param_acc=", metrics.param_accuracy);
        CHECK(metrics.task_success);
        CHECK_FALSE(metrics.missed_calls);
        CHECK(metrics.param_accuracy == 1.0);
    }
    // 报告 JSON 可解析、字段齐。
    const std::string report = BenchReportJson(all_metrics);
    const auto parsed = nlohmann::json::parse(report, nullptr, false);
    REQUIRE_FALSE(parsed.is_discarded());
    CHECK(parsed.at("harness") == kPtcHarnessRevision);
    CHECK(parsed.at("runs").size() == all_metrics.size());
    CHECK(parsed.at("runs")[0].at("scenario_id") == all_metrics[0].scenario_id);
}

TEST_CASE("离线判卷: 一半失败题的成功/失败账对") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:跳过
    }
    const auto scenarios = BuildBenchScenarios();
    const auto it = std::find_if(scenarios.begin(), scenarios.end(), [](const BenchScenario& scenario) {
        return scenario.id == "half-fail-8";
    });
    REQUIRE(it != scenarios.end());
    const auto metrics = RunScenarioOffline(*it, BenchOptions());
    CHECK(metrics.task_success);      // 脚本收口:异常收口算任务成功
    CHECK(metrics.call_count == 8);   // 八次都发起了(失败的也入账)
    CHECK(metrics.missed_calls == false);
}

TEST_CASE("离线判卷: chain-5 依赖链的原始 run 与判卷一致过线") {
    if (!PythonAvailable()) {
        return;  // 缺 Python:跳过
    }
    const auto scenarios = BuildBenchScenarios();
    const auto it = std::find_if(scenarios.begin(), scenarios.end(), [](const BenchScenario& scenario) {
        return scenario.id == "chain-5";
    });
    REQUIRE(it != scenarios.end());
    // 复刻 RunScenarioOffline 的装配,直接拿原始 run 钉住:五枚调用、后一枚
    // 入参来自前一枚真实返回、emit 链尾正确。
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<LocalEchoTool>());
    std::vector<StubToolInfo> infos;
    for (const auto& definition : it->tools) {
        StubToolInfo info;
        info.definition = definition;
        info.parallel_safe = true;
        infos.push_back(info);
    }
    const auto stub = GenerateStubModule(infos, StubMode::Full);
    auto options = BenchOptions();
    options.executor = [&registry](const std::string& name, const nlohmann::json& input) {
        tools::Tool* tool = registry.Find(name);
        if (tool == nullptr) {
            return tools::Tool::Result{"未知工具: " + name, true};
        }
        return tool->execute(input);
    };
    const auto run = PtcRunner::Run(it->reference_script, stub.python_source, std::move(options));
    REQUIRE(run.ok);
    REQUIRE(run.calls.size() == 5);
    // 第二枚的入参是第一枚真实返回剥前缀后的值(真依赖,不是常识猜)。
    CHECK(run.calls[0].input.at("message") == "seed");
    CHECK(run.calls[1].input.at("message") == "seed-s1");
    CHECK(run.calls[2].input.at("message") == "seed-s1-s2");
    CHECK(run.emit_value.at("final") == "seed-s1-s2-s3-s4-s5");
    CHECK(JudgeRun(*it, run, true, false, false).task_success);
    CHECK(RunScenarioOffline(*it, BenchOptions()).task_success);
}

TEST_CASE("判卷的负样本: 漏调用/漏摘要探针判失败") {
    const auto scenarios = BuildBenchScenarios();
    const auto it = std::find_if(scenarios.begin(), scenarios.end(), [](const BenchScenario& scenario) {
        return scenario.id == "single-echo-1";
    });
    REQUIRE(it != scenarios.end());
    // 构造一份"漏了调用且摘要是错"的账,直接喂判卷器。
    PtcRunResult run;
    run.ok = true;
    run.emit_value = nlohmann::json{{"got", "完全不相干的摘要"}};
    PtcCallRecord record;
    record.tool = "wrong_tool";
    record.input = {{"message", "wrong"}};
    record.ok = true;
    run.calls.push_back(record);
    const auto metrics = JudgeRun(*it, run, true, true, true);
    CHECK_FALSE(metrics.task_success);
    CHECK(metrics.missed_calls);
    CHECK(metrics.param_accuracy == 0.0);
}
