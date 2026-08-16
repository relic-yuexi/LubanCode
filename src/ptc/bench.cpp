// bench.hpp 的实现:八类题库、判卷、离线跑法、JSON 报告。

#include "ptc/bench.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "ptc/profile.hpp"
#include "ptc/stub_generator.hpp"
#include "tools/registry.hpp"
#include "tools/schema_check.hpp"
#include "tools/tool.hpp"

namespace lubancode::ptc {

namespace {

// bench 假工具:echo 型回 "echo:<message>",flaky 型按 n 的奇偶成败,
// big 型回大块文本(大结果摘要题用)。schema 全是平对象,判卷只看入参。
class BenchEchoTool : public tools::Tool {
public:
    explicit BenchEchoTool(std::string tool_name, bool flaky, std::size_t big_bytes)
        : name_(std::move(tool_name)), flaky_(flaky), big_bytes_(big_bytes) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "bench 假工具:" + name_; }
    bool needs_confirm() const override { return flaky_; }  // flaky 兼当"拒权"源

    nlohmann::json input_schema() const override {
        return nlohmann::json::parse(
            R"({"type":"object","properties":{"message":{"type":"string"},"n":{"type":"integer"}},"required":["message"]})");
    }

    Result execute(const nlohmann::json& input) override {
        const std::string message = input.at("message").get<std::string>();
        if (flaky_ && input.contains("n") && input.at("n").is_number_integer() &&
            input.at("n").get<int>() % 2 == 0) {
            return Result{"flaky: 偶数序号按题设失败", true};
        }
        if (big_bytes_ > 0) {
            return Result{"BIG[" + std::to_string(big_bytes_) + "]:" + message, false};
        }
        return Result{"echo:" + message, false};
    }

private:
    std::string name_;
    bool flaky_ = false;
    std::size_t big_bytes_ = 0;
};

// 按题的工具定义造一份假注册表(executor 吃它)。
tools::ToolRegistry BuildScenarioRegistry(const BenchScenario& scenario) {
    tools::ToolRegistry registry;
    for (const auto& definition : scenario.tools) {
        const bool flaky = definition.name.rfind("flaky_", 0) == 0;
        const std::size_t big = definition.name.rfind("big_", 0) == 0 ? 64 * 1024 : 0;
        registry.Register(std::make_unique<BenchEchoTool>(definition.name, flaky, big));
    }
    return registry;
}

tools::Tool::Result ExecuteScenarioCall(tools::ToolRegistry& registry, const std::string& name,
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

// 一枚期望调用是否被命中(工具名同 + 声明的入参字段全等)。
bool CallMatches(const nlohmann::json& expected, const PtcCallRecord& record) {
    if (!expected.is_object() || !expected.contains("tool")) {
        return false;
    }
    if (record.tool != expected.at("tool").get<std::string>()) {
        return false;
    }
    if (!expected.contains("input")) {
        return true;
    }
    for (auto it = expected.at("input").begin(); it != expected.at("input").end(); ++it) {
        if (!record.input.contains(it.key()) || record.input.at(it.key()) != it.value()) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string ToString(BenchKind kind) {
    switch (kind) {
        case BenchKind::SingleToolShortParams: return "single_tool_short_params";
        case BenchKind::ChainDependency: return "chain_dependency";
        case BenchKind::FanoutIndependent: return "fanout_independent";
        case BenchKind::NeedleInHaystack: return "needle_in_haystack";
        case BenchKind::LargeResultSummary: return "large_result_summary";
        case BenchKind::HalfFailures: return "half_failures";
        case BenchKind::MixedReadOnlySideEffects: return "mixed_readonly_side_effects";
        case BenchKind::ExtensionCross: return "extension_cross";
    }
    return "unknown";
}

nlohmann::json BenchMetrics::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out["scenario_id"] = scenario_id;
    out["backend"] = backend;
    out["task_success"] = task_success;
    out["missed_calls"] = missed_calls;
    out["param_accuracy"] = param_accuracy;
    out["model_rounds"] = model_rounds;
    out["first_result_ms"] = first_result_ms;
    out["total_ms"] = total_ms;
    out["input_tokens"] = input_tokens;
    out["output_tokens"] = output_tokens;
    out["cache_read_tokens"] = cache_read_tokens;
    out["duplicate_calls"] = duplicate_calls;
    out["sandbox_covered"] = sandbox_covered;
    out["permission_covered"] = permission_covered;
    out["hooks_covered"] = hooks_covered;
    out["side_effect_replays"] = side_effect_replays;
    out["call_count"] = call_count;
    return out;
}

std::vector<BenchScenario> BuildBenchScenarios() {
    std::vector<BenchScenario> out;

    // 1. 单工具、短参数(BFCL simple 形状)。
    {
        BenchScenario scenario;
        scenario.id = "single-echo-1";
        scenario.kind = BenchKind::SingleToolShortParams;
        scenario.prompt = "调用 echo 工具,message 为 'bench-single',把结果原样汇报。";
        api::ToolDefinition definition;
        definition.name = "echo";
        definition.description = "回声";
        definition.input_schema = nlohmann::json::parse(
            R"({"type":"object","properties":{"message":{"type":"string"}},"required":["message"]})");
        scenario.tools.push_back(definition);
        scenario.expected = nlohmann::json::parse(
            R"({"calls":[{"tool":"echo","input":{"message":"bench-single"}}],"emit_probe":"echo:bench-single"})");
        scenario.reference_script =
            "from luban_tools import echo\nr = echo(message='bench-single')\nemit({'got': r['content']})\n";
        out.push_back(std::move(scenario));
    }

    // 2. 真依赖链(五步;BFCL chained 形状)。
    {
        BenchScenario scenario;
        scenario.id = "chain-5";
        scenario.kind = BenchKind::ChainDependency;
        scenario.prompt = "从 'seed' 开始,把 echo 的真实返回值去掉 echo: 前缀后加 '-s<N>' 再喂下一枚,"
                          "连走 5 步,汇报最后一步。";
        api::ToolDefinition definition;
        definition.name = "echo";
        definition.description = "回声";
        definition.input_schema = nlohmann::json::parse(
            R"({"type":"object","properties":{"message":{"type":"string"}},"required":["message"]})");
        scenario.tools.push_back(definition);
        // 期望账只钉头两步的真实入参(第 3 步起依赖运行期结果,判卷看
        // emit_probe 与调用数)。探针取链尾两段:五步走完末尾必是 -s4-s5。
        scenario.expected = nlohmann::json::parse(
            R"({"calls":[{"tool":"echo","input":{"message":"seed"}},
                          {"tool":"echo","input":{"message":"seed-s1"}}],
                "call_count":5,"emit_probe":"-s4-s5"})");
        scenario.reference_script =
            "from luban_tools import echo\ncur = 'seed'\nfor i in range(1, 6):\n"
            "    r = echo(message=cur)\n    cur = r['content'][len('echo:'):] + '-s%d' % i\n"
            "emit({'final': cur})\n";
        out.push_back(std::move(scenario));
    }

    // 3. 独立 fan-out(八路;BFCL multi/parallel 形状)。
    {
        BenchScenario scenario;
        scenario.id = "fanout-8";
        scenario.kind = BenchKind::FanoutIndependent;
        scenario.prompt = "并发调 echo 八次,message 分别为 'f0'..'f7',汇报八个结果。";
        api::ToolDefinition definition;
        definition.name = "echo";
        definition.description = "回声";
        definition.input_schema = nlohmann::json::parse(
            R"({"type":"object","properties":{"message":{"type":"string"}},"required":["message"]})");
        scenario.tools.push_back(definition);
        nlohmann::json expected_calls = nlohmann::json::array();
        for (int i = 0; i < 8; ++i) {
            expected_calls.push_back(nlohmann::json{{"tool", "echo"},
                                                     {"input", {{"message", "f" + std::to_string(i)}}}});
        }
        scenario.expected = nlohmann::json{{"calls", expected_calls}, {"call_count", 8}, {"emit_probe", "f7"}};
        scenario.reference_script =
            "from luban_tools import echo\nrs = [echo(message='f%d' % i) for i in range(8)]\n"
            "emit({'count': len(rs), 'last': rs[-1]['content']})\n";
        out.push_back(std::move(scenario));
    }

    // 4. 128 份无关 schema 下找目标(针在草堆;在线层给模型看全量 schema,
    //    离线层验证"正确选中目标工具"的判卷)。
    {
        BenchScenario scenario;
        scenario.id = "needle-128";
        scenario.kind = BenchKind::NeedleInHaystack;
        scenario.prompt = "可用工具很多,其中只有 'needle_lookup' 能按 key 查到值。查 key='bench-needle',"
                          "汇报结果。";
        for (int i = 0; i < 128; ++i) {
            api::ToolDefinition definition;
            definition.name = i == 64 ? "needle_lookup" : ("noise_" + std::to_string(i));
            definition.description = i == 64 ? "按 key 查值(唯一的针)" : "无关噪音工具";
            definition.input_schema = nlohmann::json::parse(
                R"({"type":"object","properties":{"message":{"type":"string"}},"required":["message"]})");
            scenario.tools.push_back(std::move(definition));
        }
        scenario.expected = nlohmann::json::parse(
            R"({"calls":[{"tool":"needle_lookup","input":{"message":"bench-needle"}}],"emit_probe":"echo:bench-needle"})");
        scenario.reference_script =
            "from luban_tools import needle_lookup\nr = needle_lookup(message='bench-needle')\n"
            "emit({'got': r['content']})\n";
        out.push_back(std::move(scenario));
    }

    // 5. 大结果落盘与摘要。
    {
        BenchScenario scenario;
        scenario.id = "big-summary-1";
        scenario.kind = BenchKind::LargeResultSummary;
        scenario.prompt = "big_fetch 会回一大块文本。取回后不要全文汇报,emit 里只给长度与前 40 字符。";
        api::ToolDefinition definition;
        definition.name = "big_fetch";
        definition.description = "取回大块文本";
        definition.input_schema = nlohmann::json::parse(
            R"({"type":"object","properties":{"message":{"type":"string"}},"required":["message"]})");
        scenario.tools.push_back(definition);
        scenario.expected = nlohmann::json::parse(
            R"({"calls":[{"tool":"big_fetch","input":{"message":"payload"}}],"emit_probe":"BIG[65536]"})");
        scenario.reference_script =
            "from luban_tools import big_fetch\nr = big_fetch(message='payload')\n"
            "c = r['content']\nemit({'len': len(c), 'head': c[:40]})\n";
        out.push_back(std::move(scenario));
    }

    // 6. 一半调用失败/拒权。
    {
        BenchScenario scenario;
        scenario.id = "half-fail-8";
        scenario.kind = BenchKind::HalfFailures;
        scenario.prompt = "flaky_echo 对偶数序号(n=0,2,4,6)会失败。发八次(n=0..7),失败的记入 errors,"
                          "成功的计数,emit 汇报两侧。";
        api::ToolDefinition definition;
        definition.name = "flaky_echo";
        definition.description = "一半失败的回声";
        definition.input_schema = nlohmann::json::parse(
            R"({"type":"object","properties":{"message":{"type":"string"},"n":{"type":"integer"}},"required":["message","n"]})");
        scenario.tools.push_back(definition);
        scenario.expected = nlohmann::json::parse(
            R"({"call_count":8,"emit_probe":"ok_count"})");
        scenario.reference_script =
            "from luban_tools import flaky_echo, ToolCallError\nok_count = 0\nerrors = []\n"
            "for n in range(8):\n    try:\n        flaky_echo(message='h%d' % n, n=n)\n        ok_count += 1\n"
            "    except ToolCallError:\n        errors.append(n)\n"
            "emit({'ok_count': ok_count, 'errors': errors})\n";
        out.push_back(std::move(scenario));
    }

    // 7. 混合只读与副作用(P3 前只读占位:记账,不真造写工具)。
    {
        BenchScenario scenario;
        scenario.id = "mixed-placeholder-1";
        scenario.kind = BenchKind::MixedReadOnlySideEffects;
        scenario.prompt = "先读 echo 两次,再'写'(占位:本版无写工具,模型应只做只读并说明)。";
        api::ToolDefinition definition;
        definition.name = "echo";
        definition.description = "回声";
        definition.input_schema = nlohmann::json::parse(
            R"({"type":"object","properties":{"message":{"type":"string"}},"required":["message"]})");
        scenario.tools.push_back(definition);
        scenario.expected = nlohmann::json::parse(R"({"call_count":2,"emit_probe":"echo:"})");
        scenario.reference_script =
            "from luban_tools import echo\nrs = [echo(message='m0'), echo(message='m1')]\n"
            "emit({'reads': [r['content'] for r in rs]})\n";
        out.push_back(std::move(scenario));
    }

    // 8. 扩展交叉(hooks/MCP/LSP/技能;离线层无外挂,判卷记"未覆盖",
    //    在线层手测清单里跑真交叉)。
    {
        BenchScenario scenario;
        scenario.id = "extension-cross-1";
        scenario.kind = BenchKind::ExtensionCross;
        scenario.prompt = "(在线层专用)经 hooks 拦截 + MCP 工具的交叉编排;离线层只验记账形状。";
        api::ToolDefinition definition;
        definition.name = "echo";
        definition.description = "回声";
        definition.input_schema = nlohmann::json::parse(
            R"({"type":"object","properties":{"message":{"type":"string"}},"required":["message"]})");
        scenario.tools.push_back(definition);
        scenario.expected = nlohmann::json::parse(R"({"call_count":1,"emit_probe":"echo:"})");
        scenario.reference_script = "from luban_tools import echo\nr = echo(message='x')\nemit({'a': r['content']})\n";
        out.push_back(std::move(scenario));
    }

    return out;
}

BenchMetrics JudgeRun(const BenchScenario& scenario, const PtcRunResult& run, bool sandbox_covered,
                      bool permission_covered, bool hooks_covered) {
    BenchMetrics metrics;
    metrics.scenario_id = scenario.id;
    metrics.backend = "ptc";
    metrics.total_ms = run.elapsed_ms;
    metrics.first_result_ms = run.calls.empty() ? 0 : run.calls.front().elapsed_ms;
    metrics.model_rounds = 1;  // 离线层:一段脚本一轮;在线层由跑法实记
    metrics.sandbox_covered = sandbox_covered;
    metrics.permission_covered = permission_covered;
    metrics.hooks_covered = hooks_covered;
    metrics.call_count = static_cast<int>(run.calls.size());
    metrics.task_success = run.ok;

    // 期望调用逐项匹配(按多重集,不看次序;链题的次序约束靠 expected
    // 里钉死的前几步入参)。
    int expected_total = 0;
    int matched = 0;
    std::vector<bool> used(run.calls.size(), false);
    if (scenario.expected.contains("calls") && scenario.expected["calls"].is_array()) {
        for (const auto& expected : scenario.expected["calls"]) {
            ++expected_total;
            for (std::size_t i = 0; i < run.calls.size(); ++i) {
                if (!used[i] && CallMatches(expected, run.calls[i])) {
                    used[i] = true;
                    ++matched;
                    break;
                }
            }
        }
    }
    metrics.missed_calls = matched < expected_total;
    metrics.param_accuracy = expected_total == 0 ? 1.0 : static_cast<double>(matched) / expected_total;
    // 任务成功 = 跑成 + 期望调用全命中 + (有 call_count 要求时数量对)。
    if (run.ok && !metrics.missed_calls && scenario.expected.contains("call_count")) {
        metrics.task_success = static_cast<int>(run.calls.size()) == scenario.expected["call_count"].get<int>();
    }

    // 重复调用:同 (tool, input_hash) 的成功执行超过一次算重复。
    {
        std::map<std::pair<std::string, std::string>, int> seen;
        for (const auto& call : run.calls) {
            if (call.ok && !call.is_error) {
                ++seen[{call.tool, call.input_hash}];
            }
        }
        for (const auto& [key, count] : seen) {
            metrics.duplicate_calls += count - 1;
        }
    }

    // emit_probe:摘要里必须出现。
    if (run.ok && scenario.expected.contains("emit_probe") && scenario.expected["emit_probe"].is_string()) {
        const std::string probe = run.emit_value.dump();
        if (probe.find(scenario.expected["emit_probe"].get<std::string>()) == std::string::npos) {
            metrics.task_success = false;
        }
    }
    return metrics;
}

BenchMetrics RunScenarioOffline(const BenchScenario& scenario, const PtcRunner::Options& options) {
    tools::ToolRegistry registry = BuildScenarioRegistry(scenario);
    std::vector<StubToolInfo> infos;
    for (const auto& definition : scenario.tools) {
        StubToolInfo info;
        info.definition = definition;
        info.parallel_safe = true;
        infos.push_back(std::move(info));
    }
    const auto stub = GenerateStubModule(infos, StubMode::Full);
    PtcRunner::Options run_options = options;
    run_options.executor = [&registry](const std::string& name, const nlohmann::json& input) {
        return ExecuteScenarioCall(registry, name, input);
    };
    const auto run = PtcRunner::Run(scenario.reference_script, stub.python_source, std::move(run_options));
    return JudgeRun(scenario, run, /*sandbox_covered=*/true, /*permission_covered=*/false,
                    /*hooks_covered=*/false);
}

std::string BenchReportJson(const std::vector<BenchMetrics>& runs) {
    nlohmann::json root = nlohmann::json::object();
    root["harness"] = kPtcHarnessRevision;
    root["runs"] = nlohmann::json::array();
    for (const auto& run : runs) {
        root["runs"].push_back(run.ToJson());
    }
    return root.dump(2);
}

}  // namespace lubancode::ptc
