// 自进化闭环阶段 5 单测:Workflow 与 Agent 组合包。钉四件事——
//   1. 两把尺:同形多场(成功路折叠序列一致)出组合候选;全场工具面再
//      同形才添 Agent;单场或组合不稳照旧 Skill-only;
//   2. 组合包形状:workflows/<id>/workflow.yaml 过 workflow parser 与结构
//      校验,agents/<slug>-agent.yaml 过 Agent parser,整包过静态门
//      (AnalyzePackage:引用闭合、canonical 名、无越界);
//   3. 降档:组合草稿过不了静态门(这里用悬空的 plugin__ wire 名触发)
//      就地降回 Skill-only,诊断进账,不硬塞;
//   4. 评测分家与复杂度代价:组合候选的评测计划只带确定性检查器(不起
//      被测 workflow),评测账静态行记复杂度,批准页照实亮。

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "agent/agent_definition.hpp"
#include "evolution/coordinator.hpp"
#include "evolution/eval.hpp"
#include "evolution/observation_store.hpp"
#include "platform/paths.hpp"
#include "skills/skill_drafter.hpp"
#include "skills/workflow_recorder.hpp"
#include "workflow/parser.hpp"
#include "workflow/validator.hpp"

namespace {

namespace fs = std::filesystem;
using namespace lubancode::evolution;

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_evolution_stage5_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

// 造一场完整录制件的材料的帮手。tool 脚本形如:
//   { {"read_file", "cfg/a.yaml", true}, {"write_file", "out/a.json", true} }
// 每项 = (工具名、首枚入参 path、该步最终成败)。同一名连着给两次即折成
// 一段;首枚失败、次枚成功即"失败重试后成功"。
struct ScriptedStep {
    std::string tool;
    std::string path;
    bool ok;
};

ClusterTaskMaterial MakeTask(const fs::path& recordings_root, const std::string& name,
                             const std::string& goal, const std::vector<ScriptedStep>& script) {
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
        recorder->RecordToolResult(step.tool, !step.ok,
                                   step.ok ? "成了" : "没成", step.ok ? "" : "error",
                                   step.ok ? "" : "tool.failed", execution);
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

}  // namespace

// ---------------------------------------------------------------------------
// 两把尺(纯函数)
// ---------------------------------------------------------------------------

TEST_CASE("stage5:两把尺——同形两场过 Workflow 档,工具面同形才过 Agent 档") {
    TempDir temp;
    const fs::path recordings = temp.Get() / "recordings";
    const std::vector<ScriptedStep> script = {
        {"read_file", "cfg/a.yaml", true},
        {"write_file", "out/a.json", true},
    };

    SUBCASE("两场同形:Workflow 档与 Agent 档都过") {
        const ClusterTaskMaterial a = MakeTask(recordings, "provider 排查甲", "排查 provider 绑定", script);
        const ClusterTaskMaterial b = MakeTask(recordings, "provider 排查乙", "排查 provider 绑定", script);
        const ComboThreshold verdict = AssessComboThreshold({a, b});
        CHECK(verdict.cluster_size == 2);
        CHECK(verdict.sequences_stable);
        CHECK(verdict.faces_stable);
        CHECK(verdict.workflow_eligible);
        CHECK(verdict.agent_eligible);
        CHECK(verdict.workflow_steps == 2);
        CHECK(verdict.why_not.empty());
    }

    SUBCASE("单场:两把尺都不过,why_not 有话") {
        const ClusterTaskMaterial a = MakeTask(recordings, "provider 排查甲", "排查 provider 绑定", script);
        const ComboThreshold verdict = AssessComboThreshold({a});
        CHECK_FALSE(verdict.workflow_eligible);
        CHECK_FALSE(verdict.agent_eligible);
        REQUIRE_FALSE(verdict.why_not.empty());
        CHECK(verdict.why_not.front().find("Skill-only") != std::string::npos);
    }

    SUBCASE("组合不稳:一场的重试步子在另一场永远失败,序列不同形") {
        // 甲:read 成、write 重试后成;乙:read 成、write 连败(成功路只剩 read)。
        const ClusterTaskMaterial a = MakeTask(recordings, "provider 排查甲", "排查 provider 绑定",
                                               {{"read_file", "cfg/a.yaml", true},
                                                {"write_file", "out/a.json", true}});
        lubancode::skills::RecordingStartInfo info;
        info.name = "provider 排查乙";
        info.goal = "排查 provider 绑定";
        info.acceptance = "产物可解析";
        info.cwd = "D:/elsewhere";
        auto recorder = lubancode::skills::WorkflowRecorder::Start(recordings, info);
        REQUIRE(recorder.has_value());
        recorder->RecordToolCall("read_file", nlohmann::json{{"path", "cfg/b.yaml"}}, "e1", "e1");
        recorder->RecordToolResult("read_file", false, "读到绑定段", "", "", "e1");
        recorder->RecordToolCall("write_file", nlohmann::json{{"path", "out/b.json"}}, "e2", "e2");
        recorder->RecordToolResult("write_file", true, "没写成", "error", "fs.denied", "e2");
        CHECK(recorder->Stop("yaml 可解析").has_value());
        ClusterTaskMaterial b;
        b.status.id = recorder->id();
        b.status.name = info.name;
        b.status.dir = recorder->dir();
        b.status.finished = true;
        b.events = lubancode::skills::ReadRecordingEvents(recorder->dir());
        const ComboThreshold verdict = AssessComboThreshold({a, b});
        CHECK_FALSE(verdict.sequences_stable);
        CHECK_FALSE(verdict.workflow_eligible);
        REQUIRE_FALSE(verdict.why_not.empty());
        CHECK(verdict.why_not.front().find("不同形") != std::string::npos);
    }

    SUBCASE("工具面不同形:Workflow 档过、Agent 档不过") {
        // 甲的链路与乙一致,但乙多摸过一件连败的工具(不在任何一场的成功路上)。
        const ClusterTaskMaterial a = MakeTask(recordings, "provider 排查甲", "排查 provider 绑定", script);
        lubancode::skills::RecordingStartInfo info;
        info.name = "provider 排查乙";
        info.goal = "排查 provider 绑定";
        info.acceptance = "产物可解析";
        info.cwd = "D:/elsewhere";
        auto recorder = lubancode::skills::WorkflowRecorder::Start(recordings, info);
        REQUIRE(recorder.has_value());
        recorder->RecordToolCall("read_file", nlohmann::json{{"path", "cfg/b.yaml"}}, "e1", "e1");
        recorder->RecordToolResult("read_file", false, "读到绑定段", "", "", "e1");
        recorder->RecordToolCall("search", nlohmann::json{{"q", "legacy"}}, "e2", "e2");
        recorder->RecordToolResult("search", true, "索引不在", "error", "index.missing", "e2");
        recorder->RecordToolCall("write_file", nlohmann::json{{"path", "out/b.json"}}, "e3", "e3");
        recorder->RecordToolResult("write_file", false, "写成", "", "", "e3");
        CHECK(recorder->Stop("yaml 可解析").has_value());
        ClusterTaskMaterial b;
        b.status.id = recorder->id();
        b.status.name = info.name;
        b.status.dir = recorder->dir();
        b.status.finished = true;
        b.events = lubancode::skills::ReadRecordingEvents(recorder->dir());
        const ComboThreshold verdict = AssessComboThreshold({a, b});
        CHECK(verdict.sequences_stable);      // 成功路两场都是 read_file -> write_file
        CHECK(verdict.workflow_eligible);
        CHECK_FALSE(verdict.faces_stable);    // 乙的工具面多一件 search
        CHECK_FALSE(verdict.agent_eligible);
    }

    SUBCASE("单步不算编排:归 Skill") {
        const std::vector<ScriptedStep> single = {{"read_file", "cfg/a.yaml", true}};
        const ClusterTaskMaterial a = MakeTask(recordings, "单步甲", "看一眼配置", single);
        const ClusterTaskMaterial b = MakeTask(recordings, "单步乙", "看一眼配置", single);
        const ComboThreshold verdict = AssessComboThreshold({a, b});
        CHECK(verdict.sequences_stable);
        CHECK_FALSE(verdict.workflow_eligible);
        REQUIRE_FALSE(verdict.why_not.empty());
        CHECK(verdict.why_not.front().find("归 Skill") != std::string::npos);
    }
}

TEST_CASE("stage5:成功路与工具面的折叠口径") {
    TempDir temp;
    const ClusterTaskMaterial task = MakeTask(
        temp.Get() / "recordings", "折叠口径", "看折叠",
        {{"read_file", "cfg/a.yaml", true},
         {"read_file", "cfg/a.yaml", true},   // 连续同名折成一段
         {"probe", "x", false},               // 连败:不在成功路,进工具面
         {"write_file", "out/a.json", true}});
    const std::vector<SequencedToolStep> steps = SuccessPathSteps(task.events);
    REQUIRE(steps.size() == 2);
    CHECK(steps[0].tool == "read_file");
    CHECK(steps[1].tool == "write_file");
    const std::vector<std::string> face = ToolFace(task.events);
    REQUIRE(face.size() == 3);
    CHECK(face[0] == "read_file");
    CHECK(face[1] == "probe");
    CHECK(face[2] == "write_file");
}

// ---------------------------------------------------------------------------
// 组合候选的形状与静态门
// ---------------------------------------------------------------------------

TEST_CASE("stage5:同形两场 propose 出组合候选,静态门与原生 parser 全过") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const fs::path recordings = home / "recordings";
    const ClusterTaskMaterial a = MakeTask(recordings, "provider 绑定排查甲", "排查 provider 绑定误判",
                                           {{"read_file", "cfg/provider.yaml", true},
                                            {"write_file", "out/provider.json", true}});
    const ClusterTaskMaterial b = MakeTask(recordings, "provider 绑定排查乙", "排查 provider 绑定误判",
                                           {{"read_file", "cfg/provider-b.yaml", true},
                                            {"write_file", "out/provider-b.json", true}});
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);

    const auto result = coordinator.ProposeFromCluster({a, b});
    if (!result.has_value()) {
        REQUIRE_MESSAGE(false, result.error());
    }
    CHECK(result->shape == "combination");
    CHECK(result->cluster_size == 2);
    CHECK(result->agent_drafted);
    CHECK(result->downgrade_note.empty());
    REQUIRE(result->component_paths.size() == 3);

    // ---- 文件形状 ----
    const fs::path package = result->candidate_dir / "package";
    std::optional<std::string> workflow_rel;
    std::optional<std::string> agent_rel;
    for (const std::string& rel : result->component_paths) {
        if (rel.rfind("workflows/", 0) == 0) {
            workflow_rel = rel;
        } else if (rel.rfind("agents/", 0) == 0) {
            agent_rel = rel;
        }
    }
    REQUIRE(workflow_rel.has_value());
    REQUIRE(agent_rel.has_value());
    CHECK(fs::exists(package / lubancode::platform::Utf8ToPath(*workflow_rel)));
    CHECK(fs::exists(package / lubancode::platform::Utf8ToPath(*agent_rel)));

    // ---- workflow.yaml:过 parser,过结构校验(entry/可达/无环) ----
    const auto workflow_text = ReadText(package / lubancode::platform::Utf8ToPath(*workflow_rel));
    REQUIRE(workflow_text.has_value());
    const auto definition = lubancode::workflow::ParseWorkflowYaml(*workflow_text);
    REQUIRE(definition.has_value());
    CHECK(definition->nodes.size() == 3);  // step_1、step_2、done
    CHECK(definition->entry == "step_1");
    const auto validation = lubancode::workflow::ValidateDefinition(*definition, std::nullopt);
    CHECK(validation.ok());
    // 异值入参提成 workflow 输入,没把两场的具体路径焊死成节点入参
    // (各场示例只进 inputs 的 description,不进 input 值)。
    CHECK(workflow_text->find("${inputs.step1_path}") != std::string::npos);
    CHECK(workflow_text->find("${inputs.step2_path}") != std::string::npos);
    CHECK(workflow_text->find("\"path\": \"cfg/") == std::string::npos);
    CHECK(workflow_text->find("\"path\": \"out/") == std::string::npos);

    // ---- Agent:过 parser,预装包内 Skill,工具面照观察到的两件 ----
    const auto agent_text = ReadText(package / lubancode::platform::Utf8ToPath(*agent_rel));
    REQUIRE(agent_text.has_value());
    const auto agent = lubancode::agent::ParseAgentDefinitionYaml(*agent_text, "agent.yaml");
    REQUIRE(agent.definition.has_value());
    REQUIRE(agent.definition->skills_preload.size() == 1);
    CHECK(agent.definition->skills_preload.front() == result->skill_rel_path.substr(
          7, result->skill_rel_path.size() - 7 - 9));  // skills/<slug>/SKILL.md -> <slug>
    CHECK(agent.definition->tools.allow.size() == 2);

    // ---- 静态门:AnalyzePackage 引用闭合、canonical 名、无越界 ----
    const StaticGateResult gate = RunStaticGate(package);
    CHECK(gate.pass());
    CHECK(gate.doctor_valid);
    CHECK(gate.findings.empty());

    // ---- 演化账:来源两场,组件三件 ----
    const auto record_text = ReadText(result->candidate_dir / "evolution.json");
    REQUIRE(record_text.has_value());
    const auto record = ParseEvolutionRecord(*record_text);
    REQUIRE(record.has_value());
    CHECK(record->sources.recording_ids.size() == 2);
    CHECK(record->changes.components_added.size() == 3);
    CHECK(record->generator.model == "combo-drafter");

    // ---- 评测分家:验收只有确定性检查器 + 人工口述,没有"跑被测 workflow" ----
    const auto plan_text = ReadText(result->candidate_dir / "eval-plan.json");
    REQUIRE(plan_text.has_value());
    const auto plan = ParseEvalPlan(*plan_text);
    REQUIRE(plan.has_value());
    REQUIRE(plan->replay.size() == 1);
    CHECK(plan->replay.front().workspace == ".");
    bool has_executable = false;
    for (const AcceptanceCheck& check : plan->replay.front().acceptance) {
        if (check.kind == AcceptanceCheckKind::FileExists ||
            check.kind == AcceptanceCheckKind::FileContains) {
            has_executable = true;
            // 只查包形状,不引用被测 workflow 的执行(评测与被测分家)。
            CHECK(check.path.rfind("package/") == 0);
        }
    }
    CHECK(has_executable);

    // ---- 评测跑一遍:静态行带复杂度,组合比最小包贵 ----
    const auto test = coordinator.Test(result->candidate_id);
    REQUIRE(test.has_value());
    REQUIRE(test->run_summary.complexity.has_value());
    CHECK(test->run_summary.complexity->shape == "combination");
    CHECK(test->run_summary.complexity->has_workflow);
    CHECK(test->run_summary.complexity->has_agent);
    CHECK(test->run_summary.complexity->components == 3);
    CHECK(test->run_summary.complexity->extra_components == 2);
    CHECK(test->run_summary.complexity->extra_files >= 2);
    // replay 的可执行检查器真跑过(workspace=候选目录,包形状查得到)。
    CHECK(test->run_summary.checks_passed >= 3);
    CHECK(test->static_gate.pass());

    // ---- 批准页材料:复杂度照实亮 ----
    const auto brief = coordinator.BuildApprovalBrief(result->candidate_id);
    REQUIRE(brief.has_value());
    REQUIRE(brief->complexity.has_value());
    CHECK(brief->complexity->shape == "combination");
    CHECK(brief->complexity->extra_components == 2);
    CHECK(brief->complexity->SummaryLine().find("组合包") != std::string::npos);

    // ---- diff:分档展示 workflow 与 agent ----
    const auto diff = coordinator.Diff(result->candidate_id);
    REQUIRE(diff.has_value());
    CHECK(diff->shape == "combination");
    bool saw_workflow = false;
    bool saw_agent = false;
    for (const auto& file : diff->added) {
        if (file.kind == "workflow") saw_workflow = true;
        if (file.kind == "agent") saw_agent = true;
    }
    CHECK(saw_workflow);
    CHECK(saw_agent);
    CHECK(diff->workflow_summary.find("read_file") != std::string::npos);
    CHECK(diff->workflow_summary.find("write_file") != std::string::npos);
    CHECK(diff->agent_summary.find("工具面") != std::string::npos);
}

TEST_CASE("stage5:单场与不稳簇照旧 Skill-only;旧入口不动") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const fs::path recordings = home / "recordings";
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);

    SUBCASE("单场簇:最小包,无 workflow 无 agent") {
        const ClusterTaskMaterial a = MakeTask(recordings, "provider 排查", "排查 provider 绑定",
                                               {{"read_file", "cfg/a.yaml", true},
                                                {"write_file", "out/a.json", true}});
        const auto result = coordinator.ProposeFromCluster({a});
        REQUIRE(result.has_value());
        CHECK(result->shape == "skill-only");
        CHECK(result->cluster_size == 1);
        CHECK(result->component_paths.size() == 1);
        CHECK(fs::exists(result->candidate_dir / "package" /
                         lubancode::platform::Utf8ToPath(result->skill_rel_path)));
        CHECK(!fs::exists(result->candidate_dir / "package" / "workflows"));
        CHECK(!fs::exists(result->candidate_dir / "package" / "agents"));
    }

    SUBCASE("不稳簇(成功路不同形):Skill-only 带缘由") {
        const ClusterTaskMaterial a =
            MakeTask(recordings, "provider 排查甲", "排查 provider 绑定",
                     {{"read_file", "cfg/a.yaml", true}, {"write_file", "out/a.json", true}});
        lubancode::skills::RecordingStartInfo info;
        info.name = "provider 排查乙";
        info.goal = "排查 provider 绑定";
        info.acceptance = "产物可解析";
        info.cwd = "D:/elsewhere";
        auto recorder = lubancode::skills::WorkflowRecorder::Start(recordings, info);
        REQUIRE(recorder.has_value());
        recorder->RecordToolCall("read_file", nlohmann::json{{"path", "cfg/b.yaml"}}, "e1", "e1");
        recorder->RecordToolResult("read_file", false, "读到", "", "", "e1");
        recorder->RecordToolCall("write_file", nlohmann::json{{"path", "out/b.json"}}, "e2", "e2");
        recorder->RecordToolResult("write_file", true, "没写成", "error", "fs.denied", "e2");
        CHECK(recorder->Stop("yaml 可解析").has_value());
        ClusterTaskMaterial b;
        b.status.id = recorder->id();
        b.status.name = info.name;
        b.status.dir = recorder->dir();
        b.status.finished = true;
        b.events = lubancode::skills::ReadRecordingEvents(recorder->dir());
        const auto result = coordinator.ProposeFromCluster({a, b});
        REQUIRE(result.has_value());
        CHECK(result->shape == "skill-only");
        CHECK(result->component_paths.size() == 1);
        CHECK(!fs::exists(result->candidate_dir / "package" / "workflows"));
    }

    SUBCASE("旧入口 ProposeRecording:行为不动(单场 = 最小包)") {
        const ClusterTaskMaterial a = MakeTask(recordings, "旧入口单场", "排查 provider 绑定",
                                               {{"read_file", "cfg/a.yaml", true}});
        const auto result = coordinator.ProposeRecording(a.status, a.events);
        REQUIRE(result.has_value());
        CHECK(result->shape == "skill-only");
        CHECK(result->candidate_version == "0.1.0-candidate.1");
        const auto record = ParseEvolutionRecord(
            ReadText(result->candidate_dir / "evolution.json").value_or(""));
        REQUIRE(record.has_value());
        CHECK(record->generator.model == "skill-drafter");
        CHECK(record->sources.recording_ids == std::vector<std::string>{a.status.id});
        CHECK(record->changes.components_added.size() == 1);
    }
}

// ---------------------------------------------------------------------------
// 降档:组合草稿过不了静态门,就地降回 Skill-only
// ---------------------------------------------------------------------------

TEST_CASE("stage5:悬空引用的组合草稿降档 Skill-only,诊断进账不硬塞") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const fs::path recordings = home / "recordings";
    // 工具名是 plugin__<带点包段>__<工具> 的 wire 名:静态门解不出包段
    // (ref index 为空),引用悬空,AnalyzePackage 判 invalid。
    const ClusterTaskMaterial a = MakeTask(recordings, "插件面排查甲", "查插件工具",
                                           {{"read_file", "cfg/a.yaml", true},
                                            {"plugin__ghost.pkg__probe", "x", true}});
    const ClusterTaskMaterial b = MakeTask(recordings, "插件面排查乙", "查插件工具",
                                           {{"read_file", "cfg/b.yaml", true},
                                            {"plugin__ghost.pkg__probe", "y", true}});
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);

    const auto result = coordinator.ProposeFromCluster({a, b});
    REQUIRE(result.has_value());
    CHECK(result->shape == "skill-only");  // 降了档
    CHECK_FALSE(result->agent_drafted);
    REQUIRE(result->component_paths.size() == 1);
    CHECK(!fs::exists(result->candidate_dir / "package" / "workflows"));
    CHECK(!fs::exists(result->candidate_dir / "package" / "agents"));
    REQUIRE_FALSE(result->downgrade_note.empty());
    CHECK(result->downgrade_note.find("降回") != std::string::npos);

    // 降档后的包必须真过静态门(不是带病落盘)。
    const StaticGateResult gate = RunStaticGate(result->candidate_dir / "package");
    CHECK(gate.pass());

    // 诊断进状态账(只追加账里看得见降档缘由)。
    const auto state_text = ReadText(result->candidate_dir / "state.jsonl");
    REQUIRE(state_text.has_value());
    CHECK(state_text->find("降档") != std::string::npos);

    // 演化账按降档后的形状记:一件组件,生成器注明降档。
    const auto record = ParseEvolutionRecord(
        ReadText(result->candidate_dir / "evolution.json").value_or(""));
    REQUIRE(record.has_value());
    CHECK(record->changes.components_added.size() == 1);
    CHECK(record->generator.prompt_revision == "evolution-stage5-downgraded");

    // 评测照常走得通,复杂度按 Skill-only 记(不冒充组合)。
    const auto test = coordinator.Test(result->candidate_id);
    REQUIRE(test.has_value());
    REQUIRE(test->run_summary.complexity.has_value());
    CHECK(test->run_summary.complexity->shape == "skill-only");
    CHECK(test->run_summary.complexity->extra_components == 0);
}

// ---------------------------------------------------------------------------
// 冒烟样张:同形两场起出的组合件长什么样(-s 时整份打印,给人工过目)
// ---------------------------------------------------------------------------

TEST_CASE("stage5:冒烟——同形两场的组合件样张") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const fs::path recordings = home / "recordings";
    const ClusterTaskMaterial a = MakeTask(recordings, "provider 绑定排查甲", "排查 provider 绑定误判",
                                           {{"read_file", "cfg/provider.yaml", true},
                                            {"write_file", "out/provider.json", true}});
    const ClusterTaskMaterial b = MakeTask(recordings, "provider 绑定排查乙", "排查 provider 绑定误判",
                                           {{"read_file", "cfg/provider-b.yaml", true},
                                            {"write_file", "out/provider-b.json", true}});
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const auto result = coordinator.ProposeFromCluster({a, b});
    if (!result.has_value()) {
        REQUIRE_MESSAGE(false, result.error());
    }
    REQUIRE(result->shape == "combination");

    std::cout << "==== 冒烟:组合候选 " << result->candidate_id << " ====\n";
    std::cout << "形状: " << result->shape << "(簇 " << result->cluster_size << " 场;agent "
              << (result->agent_drafted ? "带" : "不带") << ")\n";
    std::cout << "组件: ";
    for (const std::string& rel : result->component_paths) {
        std::cout << rel << " ";
    }
    std::cout << "\n";
    for (const std::string& rel : result->component_paths) {
        if (rel.compare(rel.size() - 13, 13, "/workflow.yaml") == 0 ||
            rel.compare(rel.size() - 5, 5, ".yaml") == 0) {
            const auto text = ReadText(result->candidate_dir / "package" /
                                       lubancode::platform::Utf8ToPath(rel));
            if (text.has_value()) {
                std::cout << "---- " << rel << " ----\n" << *text;
            }
        }
    }
    const auto diff = coordinator.Diff(result->candidate_id);
    REQUIRE(diff.has_value());
    std::cout << "---- diff 摘要 ----\n";
    std::cout << "workflow: " << diff->workflow_summary << "\n";
    std::cout << "agent: " << diff->agent_summary << "\n";
    const auto test = coordinator.Test(result->candidate_id);
    if (!test.has_value()) {
        REQUIRE_MESSAGE(false, test.error());
    }
    std::cout << "评测: 静态门 " << (test->static_gate.pass() ? "pass" : "fail") << ";复杂度 "
              << (test->run_summary.complexity.has_value()
                      ? test->run_summary.complexity->SummaryLine()
                      : std::string("(未记)"))
              << "\n";
}

TEST_CASE("stage5:复杂度代价栏——组合包贵出的组件与文件如实记") {
    TempDir temp;
    const fs::path combo_dir = temp.Get() / "combo" / "package";
    const fs::path minimal_dir = temp.Get() / "minimal" / "package";
    std::error_code ec;
    fs::create_directories(combo_dir / "skills" / "s" , ec);
    fs::create_directories(combo_dir / "workflows" / "s-flow", ec);
    fs::create_directories(combo_dir / "agents", ec);
    fs::create_directories(minimal_dir / "skills" / "s", ec);
    std::ofstream(combo_dir / "package.yaml") << "schema: 1\n";
    std::ofstream(combo_dir / "skills" / "s" / "SKILL.md") << "x\n";
    std::ofstream(combo_dir / "workflows" / "s-flow" / "workflow.yaml") << "x\n";
    std::ofstream(combo_dir / "agents" / "s-agent.yaml") << "x\n";
    std::ofstream(minimal_dir / "package.yaml") << "schema: 1\n";
    std::ofstream(minimal_dir / "skills" / "s" / "SKILL.md") << "x\n";

    const ComplexityCost combo = ComputeComplexityCost(combo_dir);
    CHECK(combo.shape == "combination");
    CHECK(combo.components == 3);
    CHECK(combo.extra_components == 2);
    CHECK(combo.files == 4);
    CHECK(combo.extra_files == 2);
    CHECK(combo.SummaryLine().find("多 2 件组件") != std::string::npos);

    const ComplexityCost minimal = ComputeComplexityCost(minimal_dir);
    CHECK(minimal.shape == "skill-only");
    CHECK(minimal.components == 1);
    CHECK(minimal.extra_components == 0);
    CHECK(minimal.extra_files == 0);

    // 序列化往返(评测账行的扩展字段)。
    const nlohmann::json json = combo.ToJson();
    const auto back = ComplexityCost::FromJson(json);
    REQUIRE(back.has_value());
    CHECK(back->shape == combo.shape);
    CHECK(back->extra_components == combo.extra_components);
    CHECK(back->extra_files == combo.extra_files);
}
