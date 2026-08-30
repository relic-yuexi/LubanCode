// 自进化闭环阶段 6 收官:MCP server 草稿。钉六件事——
//   1. 选路判据:尺三同判据(簇 >=2 场同求、无人成功),同求而无人成功
//      的工具 >=2 件走 MCP server 路(缺一项服务),恰一件照旧 process
//      Plugin 路(缺一条命令);
//   2. 草稿形状:mcp.yaml(schema 1,stdio,${package_dir} 占位,network 恒
//      关)过它自己的严格解析器;server.py 是 newline JSON-RPC 的诚实
//      "未实现"脚手架;requirements.txt 零依赖;
//   3. 权限差异与工具 wire 名(mcp__…)全进演化账;tier 如实写
//      process-plugin-or-mcp,不冒充 content-only;
//   4. 零进程:评测计划没有 command 项,评测账 tool_calls 为 0;复杂度
//      栏 shape=code-draft、has_mcp 照实记;
//   5. 四类安全夹具对 MCP 组件同样生效:恶意脚本/依赖投毒/路径逃逸/
//      网络越权,发现即 error(夹具全是无害的"假装恶意");干净草稿
//      零发现;
//   6. approve 明拒指路 trust,store 一枚不落;diff 分档亮 MCP 摘要。

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "evolution/coordinator.hpp"
#include "evolution/drafter.hpp"
#include "evolution/eval.hpp"
#include "evolution/observation_store.hpp"
#include "package/component.hpp"
#include "platform/paths.hpp"
#include "skills/workflow_recorder.hpp"

namespace {

namespace fs = std::filesystem;
using namespace lubancode::evolution;

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_evolution_stage6mcp_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    const fs::path& Get() const { return dir_; }

private:
    fs::path dir_;
};

std::optional<std::string> ReadText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool WriteText(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file << content;
    return file.good();
}

struct ScriptedStep {
    std::string tool;
    std::string path;
    bool ok;
};

// 一场完整录制,可带多件"想用而不可得"的工具(每件一对 call/result,
// error_code=registry.unknown_tool)。
struct WantedCall {
    std::string tool;
    nlohmann::json input = nlohmann::json::object();
};

ClusterTaskMaterial MakeTask(const fs::path& recordings_root, const std::string& name,
                             const std::string& goal, const std::vector<ScriptedStep>& script,
                             const std::vector<WantedCall>& wanted = {}) {
    lubancode::skills::RecordingStartInfo info;
    info.name = name;
    info.goal = goal;
    info.acceptance = "产物可解析";
    info.cwd = "D:/nowhere";
    auto recorder = lubancode::skills::WorkflowRecorder::Start(recordings_root, info);
    REQUIRE(recorder.has_value());
    int item = 0;
    for (const ScriptedStep& step : script) {
        const std::string execution = "item-" + std::to_string(++item);
        recorder->RecordToolCall(step.tool, nlohmann::json{{"path", step.path}}, execution,
                                 execution);
        recorder->RecordToolResult(step.tool, !step.ok, step.ok ? "成了" : "没成",
                                   step.ok ? "" : "error", step.ok ? "" : "tool.failed", execution);
    }
    for (const WantedCall& call : wanted) {
        const std::string execution = "want-" + std::to_string(++item);
        recorder->RecordToolCall(call.tool, call.input, execution, execution);
        recorder->RecordToolResult(call.tool, true, "不认得这件工具", "error",
                                   kUnknownToolErrorCode, execution);
    }
    CHECK(recorder->Stop("yaml 可解析").has_value());
    ClusterTaskMaterial material;
    material.status.id = recorder->id();
    material.status.name = name;
    material.status.dir = recorder->dir();
    material.status.finished = true;
    material.events = lubancode::skills::ReadRecordingEvents(recorder->dir());
    return material;
}

// 造一只最小候选包(package.yaml + skill),给四类夹具测试当干净底座。
fs::path MakeCleanPackage(const fs::path& package_dir) {
    WriteText(package_dir / "package.yaml",
              "schema: 1\nid: evolve.stage6mcp-fixture\nversion: 0.1.0\nname: stage6mcp\n"
              "description: 夹具底座。\n");
    WriteText(package_dir / "skills" / "stage6mcp-fixture" / "SKILL.md",
              "---\nname: stage6mcp-fixture\ndescription: 夹具底座。\n---\n\n# 底座\n\n查一查。\n");
    return package_dir;
}

// 干净的 mcp.yaml(与草稿同款形状)。
std::string CleanMcpYaml() {
    return "schema: 1\n"
           "id: probe-server\n"
           "description: \"夹具底座 server(stdio)\"\n"
           "transport: stdio\n"
           "runtime:\n"
           "  command: python\n"
           "  args:\n"
           "    - \"${package_dir}/mcp/probe-server/server.py\"\n"
           "  timeout_ms: 30000\n"
           "permissions:\n"
           "  network: false\n";
}

int CountFindings(const StaticGateResult& gate, const std::string& kind) {
    int count = 0;
    for (const ScanFinding& finding : gate.findings) {
        if (finding.kind == kind) {
            ++count;
        }
    }
    return count;
}

const std::vector<ScriptedStep> kScript = {
    {"read_file", "cfg/a.yaml", true},
    {"write_file", "out/a.json", true},
};

}  // namespace

// ---------------------------------------------------------------------------
// 1. 选路判据:同尺三,>=2 件同求走 MCP 路,恰一件照旧 Plugin 路
// ---------------------------------------------------------------------------
TEST_CASE("stage6mcp:选路——同求两件不存在的工具出 MCP 草稿;恰一件照旧 Plugin") {
    TempDir temp;
    const fs::path recordings = temp.Get() / "recordings";
    // 甲乙两场都求 jsonl_probe + jsonl_sample(两件都不存在,全簇无人成功)
    // ——同指纹同形(指纹含折叠工具序列,求的组合不同就聚不成簇),缺的是
    // 一项服务,封一只 server 合账。
    const ClusterTaskMaterial a = MakeTask(
        recordings, "账目抽样甲", "抽样核查大账", kScript,
        {{"jsonl_probe", nlohmann::json{{"path", "logs/a.jsonl"}}},
         {"jsonl_sample", nlohmann::json{{"path", "logs/a.jsonl"}, {"max_lines", 30}}}});
    const ClusterTaskMaterial b = MakeTask(
        recordings, "账目抽样乙", "抽样核查大账", kScript,
        {{"jsonl_probe", nlohmann::json{{"path", "logs/b.jsonl"}}},
         {"jsonl_sample", nlohmann::json{{"path", "logs/b.jsonl"}, {"max_lines", 50}}}});

    SUBCASE("尺三账:两件同求,wanted_tools 攒齐") {
        const CodeCapabilitySignal signal = AssessCodeCapability({a, b});
        CHECK(signal.eligible);
        CHECK(signal.wanted_tool == "jsonl_probe");  // 求的人最多那件当代表
        CHECK(signal.tasks_wanting == 2);
        REQUIRE(signal.wanted_tools.size() == 2);
        CHECK(signal.wanted_tools[0] == "jsonl_probe");
        CHECK(signal.wanted_tools[1] == "jsonl_sample");
    }

    ObservationStore observations(temp.Get() / ".lubancode" / "evolution" / "observations");
    EvolutionCoordinator coordinator(temp.Get() / ".lubancode" / "package-candidates", &observations);
    const auto result = coordinator.ProposeFromCluster({a, b});
    REQUIRE(result.has_value());
    REQUIRE(result->shape == "code-draft");
    CHECK(result->code_draft);
    CHECK(result->mcp_draft);  // 走的是 MCP server 路
    REQUIRE(result->wanted_tools.size() == 2);
    CHECK(result->wanted_tools[0] == "jsonl_probe");
    // 没有插件草稿:一只候选只带一种代码件。
    CHECK_FALSE(fs::exists(result->candidate_dir / "package" / "plugins"));

    SUBCASE("恰一件同求:照旧 process Plugin 路(回归)") {
        const ClusterTaskMaterial c = MakeTask(recordings, "账目抽样丙", "抽样核查大账", kScript,
                                               {{"jsonl_probe", nlohmann::json{{"path", "logs/c.jsonl"}}}});
        const ClusterTaskMaterial d = MakeTask(recordings, "账目抽样丁", "抽样核查大账", kScript,
                                               {{"jsonl_probe", nlohmann::json{{"path", "logs/d.jsonl"}}}});
        const auto single = coordinator.ProposeFromCluster({c, d});
        REQUIRE(single.has_value());
        REQUIRE(single->shape == "code-draft");
        CHECK_FALSE(single->mcp_draft);
        CHECK(fs::exists(single->candidate_dir / "package" / "plugins"));
        CHECK_FALSE(fs::exists(single->candidate_dir / "package" / "mcp"));
    }
}

// ---------------------------------------------------------------------------
// 2/3. 草稿形状:三件落盘、各自过原生 parser;演化账如实
// ---------------------------------------------------------------------------
TEST_CASE("stage6mcp:propose 出 MCP 草稿——mcp.yaml 过原生 parser,账如实") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const fs::path recordings = home / "recordings";
    const ClusterTaskMaterial a = MakeTask(
        recordings, "账目抽样甲", "抽样核查大账", kScript,
        {{"jsonl_probe", nlohmann::json{{"path", "logs/a.jsonl"}}},
         {"jsonl_sample", nlohmann::json{{"path", "logs/a.jsonl"}, {"max_lines", 30}}}});
    const ClusterTaskMaterial b = MakeTask(
        recordings, "账目抽样乙", "抽样核查大账", kScript,
        {{"jsonl_probe", nlohmann::json{{"path", "logs/b.jsonl"}}},
         {"jsonl_sample", nlohmann::json{{"path", "logs/b.jsonl"}, {"max_lines", 50}}}});
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const auto result = coordinator.ProposeFromCluster({a, b});
    REQUIRE(result.has_value());
    REQUIRE(result->shape == "code-draft");
    CHECK(result->mcp_draft);

    // ---- mcp.yaml 落盘,过它自己的严格解析器 ----
    const fs::path pkg = result->candidate_dir / "package";
    const fs::path server_dir = pkg / "mcp" / "jsonl-probe";
    const auto mcp_yaml = ReadText(server_dir / "mcp.yaml");
    REQUIRE(mcp_yaml.has_value());
    const auto parsed = lubancode::package::ParseMcpComponentYaml(*mcp_yaml, pkg);
    REQUIRE(parsed.has_value());
    CHECK(parsed->id == "jsonl-probe");
    CHECK(parsed->transport == "stdio");
    CHECK(parsed->command == "python");
    REQUIRE(parsed->args.size() == 1);
    CHECK(parsed->args[0].find("${package_dir}/mcp/jsonl-probe/server.py") != std::string::npos);
    CHECK_FALSE(parsed->network_allowed);

    // ---- server.py:诚实脚手架 ----
    const auto server = ReadText(server_dir / "server.py");
    REQUIRE(server.has_value());
    CHECK(server->find("draft-not-implemented") != std::string::npos);
    CHECK(server->find("tools/list") != std::string::npos);   // 协议账如实亮
    CHECK(server->find("tools/call") != std::string::npos);
    CHECK(server->find("initialize") != std::string::npos);
    CHECK(server->find("stderr") != std::string::npos);       // 日志只走 stderr
    CHECK(server->find("native-library") == std::string::npos);
    // 依赖清单零依赖。
    const auto requirements = ReadText(server_dir / "requirements.txt");
    REQUIRE(requirements.has_value());
    CHECK(CountFindings(RunStaticGate(pkg), "dependency-poisoning") == 0);

    // ---- 演化账:权限差异与 mcp__ wire 名 ----
    const auto record = ParseEvolutionRecord(ReadText(result->candidate_dir / "evolution.json").value_or(""));
    REQUIRE(record.has_value());
    CHECK(record->generator.prompt_revision == "evolution-stage6-mcp");
    CHECK(record->generator.model == "mcp-drafter");
    REQUIRE(record->changes.permissions_added.size() >= 2);
    CHECK(std::find(record->changes.permissions_added.begin(),
                    record->changes.permissions_added.end(),
                    "process:python") != record->changes.permissions_added.end());
    CHECK(std::find(record->changes.permissions_added.begin(),
                    record->changes.permissions_added.end(),
                    "fs_read:workspace") != record->changes.permissions_added.end());
    REQUIRE(record->changes.tools_added.size() == 2);  // 一件工具一个 wire 名
    for (const std::string& tool : record->changes.tools_added) {
        CHECK(tool.rfind("mcp__", 0) == 0);
        CHECK(tool.find("%2Ejsonl-probe__") != std::string::npos);
        CHECK(tool.size() <= 64);
    }
    CHECK(record->changes.components_added.size() >= 2);  // skill + mcp.yaml

    // 批准账如实落档,不冒充 content-only。
    const auto approval = ParseApprovalRecord(ReadText(result->candidate_dir / "approval.json").value_or(""));
    REQUIRE(approval.has_value());
    CHECK(approval->tier == "process-plugin-or-mcp");
}

// ---------------------------------------------------------------------------
// 4. 零进程:计划无 command,评测账 tool_calls=0,复杂度照实记
// ---------------------------------------------------------------------------
TEST_CASE("stage6mcp:零进程——计划无 command,tool_calls 恒 0,复杂度记 has_mcp") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const fs::path recordings = home / "recordings";
    const ClusterTaskMaterial a = MakeTask(
        recordings, "账目抽样甲", "抽样核查大账", kScript,
        {{"jsonl_probe", nlohmann::json{{"path", "logs/a.jsonl"}}},
         {"jsonl_sample", nlohmann::json{{"path", "logs/a.jsonl"}}}});
    const ClusterTaskMaterial b = MakeTask(
        recordings, "账目抽样乙", "抽样核查大账", kScript,
        {{"jsonl_probe", nlohmann::json{{"path", "logs/b.jsonl"}}},
         {"jsonl_sample", nlohmann::json{{"path", "logs/b.jsonl"}}}});
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const auto result = coordinator.ProposeFromCluster({a, b});
    REQUIRE(result.has_value());
    REQUIRE(result->mcp_draft);

    const auto plan = ParseEvalPlan(
        ReadText(result->candidate_dir / "eval-plan.json").value_or(""));
    REQUIRE(plan.has_value());
    REQUIRE(plan->replay.size() == 1);
    bool any_command = false;
    bool has_mcp_static = false;
    for (const AcceptanceCheck& check : plan->replay.front().acceptance) {
        any_command = any_command || check.kind == AcceptanceCheckKind::Command;
        has_mcp_static = has_mcp_static ||
                         (check.kind == AcceptanceCheckKind::FileContains &&
                          check.text.find("transport: stdio") != std::string::npos);
    }
    CHECK_FALSE(any_command);  // 草稿永不执行
    CHECK(has_mcp_static);

    const auto tested = coordinator.Test(result->candidate_id);
    REQUIRE(tested.has_value());
    CHECK(tested->static_gate.pass());  // 干净草稿过静态门(含四类扫描零发现)
    CHECK(tested->static_gate.findings.empty());
    REQUIRE(tested->state_after == "evaluated");
    CHECK(tested->run_summary.tool_calls.candidate == 0.0);
    REQUIRE(tested->run_summary.complexity.has_value());
    CHECK(tested->run_summary.complexity->shape == "code-draft");
    CHECK(tested->run_summary.complexity->has_mcp);
    CHECK_FALSE(tested->run_summary.complexity->has_plugin);
    CHECK(tested->run_summary.complexity->components >= 2);

    // diff 分档亮 MCP 摘要。
    const auto diff = coordinator.Diff(result->candidate_id);
    REQUIRE(diff.has_value());
    CHECK(diff->shape == "code-draft");
    CHECK(diff->mcp_summary.find("stdio server") != std::string::npos);
    CHECK(diff->mcp_summary.find("网络 关") != std::string::npos);
    CHECK(diff->plugin_summary.empty());
    REQUIRE_FALSE(diff->permission_lines.empty());
}

// ---------------------------------------------------------------------------
// 5. 四类安全夹具对 MCP 组件同样生效(夹具全是无害的"假装恶意")
// ---------------------------------------------------------------------------
TEST_CASE("stage6mcp:四类夹具对 mcp/ 组件同样全拦") {
    TempDir temp;

    SUBCASE("恶意脚本:mcp server 里的毁盘注释也拦") {
        const fs::path pkg = MakeCleanPackage(temp.Get() / "evil-mcp" / "package");
        WriteText(pkg / "mcp" / "probe-server" / "mcp.yaml", CleanMcpYaml());
        WriteText(pkg / "mcp" / "probe-server" / "server.py",
                  "# 夹具:假装恶意,只写注释\n"
                  "# 演示形状: rm -rf / 一把梭\n"
                  "def main():\n    return 0\n");
        const StaticGateResult gate = RunStaticGate(pkg);
        CHECK_FALSE(gate.pass());
        CHECK(CountFindings(gate, "malicious-script") >= 1);
    }

    SUBCASE("依赖投毒:MCP 依赖清单里的直链也拦;注释行不冤枉") {
        const fs::path pkg = MakeCleanPackage(temp.Get() / "evil-mcp-deps" / "package");
        WriteText(pkg / "mcp" / "probe-server" / "mcp.yaml", CleanMcpYaml());
        WriteText(pkg / "mcp" / "probe-server" / "server.py",
                  "import json\nimport sys\n");
        WriteText(pkg / "mcp" / "probe-server" / "requirements.txt",
                  "# 夹具:假装投毒\n"
                  "requests @ git+https://example.invalid/req.git\n");
        const StaticGateResult gate = RunStaticGate(pkg);
        CHECK_FALSE(gate.pass());
        CHECK(CountFindings(gate, "dependency-poisoning") >= 1);
        WriteText(pkg / "mcp" / "probe-server" / "requirements.txt",
                  "# 说明: 直链不许写(注释行跳过)\n");
        const StaticGateResult clean = RunStaticGate(pkg);
        CHECK(CountFindings(clean, "dependency-poisoning") == 0);
    }

    SUBCASE("路径逃逸:mcp.yaml 的 args 带 .. 出包根即拦") {
        const fs::path pkg = MakeCleanPackage(temp.Get() / "evil-mcp-path" / "package");
        std::string yaml = CleanMcpYaml();
        const std::string from = "\"${package_dir}/mcp/probe-server/server.py\"";
        const std::size_t hit = yaml.find(from);
        REQUIRE(hit != std::string::npos);
        yaml.replace(hit, from.size(), "\"${package_dir}/../evil/server.py\"");
        WriteText(pkg / "mcp" / "probe-server" / "mcp.yaml", yaml);
        WriteText(pkg / "mcp" / "probe-server" / "server.py", "import json\n");
        const StaticGateResult gate = RunStaticGate(pkg);
        CHECK_FALSE(gate.pass());
        CHECK(CountFindings(gate, "path-escape") >= 1);
    }

    SUBCASE("网络越权:server 用网而 mcp.yaml 未许;布尔放行也算宽授权") {
        const fs::path pkg = MakeCleanPackage(temp.Get() / "evil-mcp-net" / "package");
        WriteText(pkg / "mcp" / "probe-server" / "mcp.yaml", CleanMcpYaml());
        WriteText(pkg / "mcp" / "probe-server" / "server.py",
                  "# 夹具:代码想用网,清单却写着关\n"
                  "import urllib.request\n");
        const StaticGateResult gate = RunStaticGate(pkg);
        CHECK_FALSE(gate.pass());
        CHECK(CountFindings(gate, "network-overreach") >= 1);

        std::string broad_yaml = CleanMcpYaml();
        const std::size_t at = broad_yaml.find("network: false");
        REQUIRE(at != std::string::npos);
        broad_yaml.replace(at, std::strlen("network: false"), "network: true");
        const fs::path broad = MakeCleanPackage(temp.Get() / "broad-mcp-net" / "package");
        WriteText(broad / "mcp" / "probe-server" / "mcp.yaml", broad_yaml);
        WriteText(broad / "mcp" / "probe-server" / "server.py", "import json\n");
        const StaticGateResult broad_gate = RunStaticGate(broad);
        CHECK_FALSE(broad_gate.pass());
        CHECK(CountFindings(broad_gate, "network-overreach") >= 1);
    }

    SUBCASE("干净 MCP 草稿(夹具形状)四类零发现") {
        const fs::path pkg = MakeCleanPackage(temp.Get() / "clean-mcp" / "package");
        WriteText(pkg / "mcp" / "probe-server" / "mcp.yaml", CleanMcpYaml());
        WriteText(pkg / "mcp" / "probe-server" / "server.py",
                  "# 草稿脚手架:读 newline JSON-RPC,答一句未实现\n"
                  "import json\nimport sys\n");
        WriteText(pkg / "mcp" / "probe-server" / "requirements.txt", "# 零依赖\n");
        const StaticGateResult gate = RunStaticGate(pkg);
        CHECK(gate.pass());
        CHECK(gate.findings.empty());
    }
}

// ---------------------------------------------------------------------------
// 6. approve 明拒(阶段 4 语义不动,MCP 草稿同门);store 一枚不落
// ---------------------------------------------------------------------------
TEST_CASE("stage6mcp:approve 对 MCP 草稿明拒指路 trust;store 一枚不落") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const fs::path recordings = home / "recordings";
    const ClusterTaskMaterial a = MakeTask(
        recordings, "账目抽样甲", "抽样核查大账", kScript,
        {{"jsonl_probe", nlohmann::json{{"path", "logs/a.jsonl"}}},
         {"jsonl_sample", nlohmann::json{{"path", "logs/a.jsonl"}}}});
    const ClusterTaskMaterial b = MakeTask(
        recordings, "账目抽样乙", "抽样核查大账", kScript,
        {{"jsonl_probe", nlohmann::json{{"path", "logs/b.jsonl"}}},
         {"jsonl_sample", nlohmann::json{{"path", "logs/b.jsonl"}}}});
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations,
                                     home / "package-store");
    const auto proposed = coordinator.ProposeFromCluster({a, b});
    REQUIRE(proposed.has_value());
    REQUIRE(proposed->mcp_draft);
    const auto tested = coordinator.Test(proposed->candidate_id);
    REQUIRE(tested.has_value());
    REQUIRE(tested->state_after == "evaluated");

    const auto approved = coordinator.Approve(proposed->candidate_id);
    REQUIRE_FALSE(approved.has_value());
    CHECK(approved.error().find("code-bearing") != std::string::npos);
    CHECK(approved.error().find("trust") != std::string::npos);
    CHECK(approved.error().find("人工审查") != std::string::npos);
    CHECK(coordinator.store().Find(proposed->candidate_id)->state == CandidateState::Evaluated);
    CHECK_FALSE(fs::exists(home / "package-store"));
}

// ---------------------------------------------------------------------------
// 冒烟样张:两件同求起出的 MCP 草稿长什么样(-s 时整份打印,给人工过目)
// ---------------------------------------------------------------------------
TEST_CASE("stage6mcp:冒烟——MCP 草稿样张与零进程口径") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const fs::path recordings = home / "recordings";
    const ClusterTaskMaterial a = MakeTask(
        recordings, "账目抽样甲", "抽样核查大账", kScript,
        {{"jsonl_probe", nlohmann::json{{"path", "logs/a.jsonl"}}},
         {"jsonl_sample", nlohmann::json{{"path", "logs/a.jsonl"}, {"max_lines", 30}}}});
    const ClusterTaskMaterial b = MakeTask(
        recordings, "账目抽样乙", "抽样核查大账", kScript,
        {{"jsonl_probe", nlohmann::json{{"path", "logs/b.jsonl"}}},
         {"jsonl_sample", nlohmann::json{{"path", "logs/b.jsonl"}, {"max_lines", 50}}}});
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const auto result = coordinator.ProposeFromCluster({a, b});
    REQUIRE(result.has_value());
    REQUIRE(result->mcp_draft);

    std::cout << "==== 冒烟:MCP server 草稿 " << result->candidate_id << " ====\n";
    std::cout << "想要的工具: " << result->wanted_tool << " 等 " << result->wanted_tools.size()
              << " 件(簇 " << result->cluster_size << " 场同求)\n";
    std::cout << "---- mcp/jsonl-probe/mcp.yaml ----\n"
              << ReadText(result->candidate_dir / "package" / "mcp" / "jsonl-probe" / "mcp.yaml")
                     .value_or("(读不出)");
    std::cout << "---- mcp/jsonl-probe/requirements.txt ----\n"
              << ReadText(result->candidate_dir / "package" / "mcp" / "jsonl-probe" /
                          "requirements.txt")
                     .value_or("(读不出)");
    const auto diff = coordinator.Diff(result->candidate_id);
    REQUIRE(diff.has_value());
    std::cout << "---- diff 摘要 ----\n";
    std::cout << "mcp: " << diff->mcp_summary << "\n";
    for (const std::string& line : diff->permission_lines) {
        std::cout << "权限差异: " << line << "\n";
    }
    const auto test = coordinator.Test(result->candidate_id);
    REQUIRE(test.has_value());
    std::cout << "评测: 静态门 " << (test->static_gate.pass() ? "pass" : "fail") << ";tool calls "
              << test->run_summary.tool_calls.candidate << "(零进程;复杂度 "
              << (test->run_summary.complexity.has_value()
                      ? test->run_summary.complexity->SummaryLine()
                      : std::string("(未记)"))
              << ")\n";
    const auto approved = coordinator.Approve(result->candidate_id);
    REQUIRE_FALSE(approved.has_value());
    std::cout << "approve: " << approved.error() << "\n";
}
