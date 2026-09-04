// 自进化闭环阶段 3:评测与基线单测。钉七件事——
//   1. eval-plan 解析:replay/holdout/可执行验收(workspace/对象项)、
//      baseline.fixture、budget;坏计划给错误文本,不静默回落;
//   2. 密钥扫描与绝对路径扫描:键形态/裸 sk- key 命中,占位值
//      ([已打码]/{{…}}/<…>)不冤枉;盘符/UNC/POSIX 家目录命中,URL 不冤枉;
//   3. 确定性检查执行:file_exists/json_parses/file_contains/command(真起
//      进程,不经 shell)与 manual(如实 skipped);
//   4. 静态门:干净最小包 pass;塞密钥/绝对路径的包 fail 且发现即 error;
//   5. Coordinator::Test 全链:propose 出候选 -> 塞带验收命令的计划 ->
//      跑评测出账(行只追加,seq 递增)-> 状态 drafted->validated->evaluated;
//      基线对照、汇总、退出码;
//   6. 计划哈希过期(候选内容变过)拒评;workspace 缺失 = 没测(exit 2),
//      不是测砸;
//   7. 行序列化回读、坏行跳过、汇总与确定性判词(通过几项/没测什么/
//      比基线贵多少)、EvalExitCode 三档。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "evolution/coordinator.hpp"
#include "evolution/eval.hpp"
#include "evolution/observation_store.hpp"
#include "platform/paths.hpp"
#include "skills/workflow_recorder.hpp"

namespace {

namespace fs = std::filesystem;
using namespace lubancode::evolution;

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_evolution_eval_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

// 平台各挑一条无害命令:退 0 即过,不写盘不碰网(夹具规矩同 README)。
std::string OkCommand() {
#ifdef _WIN32
    return "cmd /d /c exit 0";
#else
    return "true";
#endif
}

std::string FailCommand() {
#ifdef _WIN32
    return "cmd /d /c exit 3";
#else
    return "false";
#endif
}

struct MadeRecording {
    lubancode::skills::RecordingStatus status;
    std::vector<lubancode::skills::RecordEvent> events;
};

MadeRecording MakeRecording(const fs::path& recordings_root) {
    lubancode::skills::RecordingStartInfo info;
    info.name = "provider 绑定排查";
    info.goal = "排查 provider 绑定误判";
    info.acceptance = "产物可解析";
    info.cwd = "D:/nowhere";
    auto recorder = lubancode::skills::WorkflowRecorder::Start(recordings_root, info);
    REQUIRE(recorder.has_value());
    if (!recorder.has_value()) {
        return {};
    }
    recorder->RecordToolCall("read_file", nlohmann::json{{"path", "cfg/provider.yaml"}}, "item-1",
                             "toolu-1");
    recorder->RecordToolResult("read_file", false, "读到绑定段", "", "", "item-1");
    CHECK(recorder->Stop("yaml 可解析").has_value());
    MadeRecording made;
    made.status.id = recorder->id();
    made.status.name = info.name;
    made.status.dir = recorder->dir();
    made.status.finished = true;
    made.events = lubancode::skills::ReadRecordingEvents(recorder->dir());
    return made;
}

// 塞一份带验收命令的评测计划(哈希用 propose 实算的,不带占位)。
std::string ComposeRichPlan(const std::string& candidate_id, const std::string& hash,
                            const std::string& replay_workspace, const std::string& holdout_workspace,
                            const std::string& baseline_fixture, const std::string& ok_command,
                            int max_tool_calls) {
    std::string plan;
    plan += "{\n";
    plan += "  \"schema\": 1,\n";
    plan += "  \"candidate_id\": \"" + candidate_id + "\",\n";
    plan += "  \"content_hash\": \"" + hash + "\",\n";
    plan += "  \"replay\": [{\n";
    plan += "    \"source_id\": \"replay-src-1\",\n";
    plan += "    \"task\": \"复核 report.json\",\n";
    plan += "    \"workspace\": \"" + replay_workspace + "\",\n";
    plan += "    \"acceptance\": [\n";
    plan += "      {\"kind\": \"file_exists\", \"path\": \"report.json\"},\n";
    plan += "      {\"kind\": \"json_parses\", \"path\": \"report.json\"},\n";
    plan += "      {\"kind\": \"file_contains\", \"path\": \"report.json\", \"text\": \"findings\"},\n";
    plan += "      {\"kind\": \"command\", \"command\": \"" + ok_command + "\"}\n";
    plan += "    ]\n";
    plan += "  }],\n";
    plan += "  \"holdout\": [{\n";
    plan += "    \"task_id\": \"holdout-1\",\n";
    plan += "    \"task\": \"换一处配置重核\",\n";
    plan += "    \"workspace\": \"" + holdout_workspace + "\",\n";
    plan += "    \"acceptance\": [\n";
    plan += "      {\"kind\": \"file_exists\", \"path\": \"report.json\"},\n";
    plan += "      \"每条疑点与账本一致(人工复核)\"\n";
    plan += "    ]\n";
    plan += "  }],\n";
    plan += "  \"baseline\": {\n";
    plan += "    \"kind\": \"bare-agent\",\n";
    plan += "    \"ref\": \"default-agent\",\n";
    plan += "    \"metrics\": [\"success_rate\", \"acceptance_rate\", \"tool_calls\", \"tokens\",\n";
    plan += "                 \"wall_clock_ms\", \"permission_prompts\", \"workspace_writes\"],\n";
    plan += "    \"fixture\": \"" + baseline_fixture + "\"\n";
    plan += "  },\n";
    plan += std::string("  \"budget\": {\"max_tool_calls\": ") + std::to_string(max_tool_calls) +
            ", \"max_tokens\": 200000, \"timeout_ms\": 60000}\n";
    plan += "}\n";
    return plan;
}

std::string ComposeBaselineFixture(double success, double acceptance, std::int64_t tool_calls) {
    std::string text;
    text += "{\n";
    text += "  \"schema\": 1,\n";
    text += "  \"kind\": \"bare-agent\",\n";
    text += "  \"ref\": \"default-agent\",\n";
    text += "  \"task_id\": \"replay-src-1\",\n";
    text += "  \"metrics\": {\"success_rate\": " + std::to_string(success) +
             ", \"acceptance_rate\": " + std::to_string(acceptance) +
             ", \"tool_calls\": " + std::to_string(tool_calls) +
             ", \"tokens\": 1200, \"wall_clock_ms\": 900, \"permission_prompts\": 1,"
             " \"workspace_writes\": 1},\n";
    text += "  \"unverified\": [\"real-service\"]\n";
    text += "}\n";
    return text;
}

}  // namespace

// ---------------------------------------------------------------------------
// 计划解析
// ---------------------------------------------------------------------------

TEST_CASE("评测.计划解析:夹具形状与可执行验收") {
    const std::string fixture_plan_path =
        std::string(LUBANCODE_TEST_FIXTURES_DIR) +
        "/evolution/candidate-eval-smoke/evolve.eval-smoke/cand-20260828-003/eval-plan.json";
    const auto text = ReadText(lubancode::platform::Utf8ToPath(fixture_plan_path));
    REQUIRE(text.has_value());
    const auto plan = ParseEvalPlan(*text);
    REQUIRE(plan.has_value());
    CHECK(plan->candidate_id == "cand-20260828-003");
    CHECK(plan->content_hash.rfind("sha256:", 0) == 0);
    REQUIRE(plan->replay.size() == 1);
    CHECK(plan->replay[0].task_id == "rec-placeholder-203");  // source_id 归一进 task_id
    CHECK(plan->replay[0].workspace == "fixtures/replay-workspace");
    REQUIRE(plan->replay[0].acceptance.size() == 3);
    CHECK(plan->replay[0].acceptance[0].kind == AcceptanceCheckKind::FileExists);
    CHECK(plan->replay[0].acceptance[1].kind == AcceptanceCheckKind::JsonParses);
    CHECK(plan->replay[0].acceptance[2].kind == AcceptanceCheckKind::FileContains);
    CHECK(plan->replay[0].acceptance[2].text == "findings");
    REQUIRE(plan->holdout.size() == 1);
    REQUIRE(plan->holdout[0].acceptance.size() == 2);
    CHECK(plan->holdout[0].acceptance[0].kind == AcceptanceCheckKind::FileExists);
    CHECK(plan->holdout[0].acceptance[1].kind == AcceptanceCheckKind::Manual);
    CHECK(plan->baseline_kind == "bare-agent");
    CHECK(plan->baseline_fixture == "fixtures/baseline-bare.json");
    CHECK(plan->budget_max_tool_calls == 10);
    CHECK(plan->budget_timeout_ms == 60000);
}

TEST_CASE("评测.计划解析:坏计划给错误文本") {
    CHECK_FALSE(ParseEvalPlan("not json").has_value());
    CHECK_FALSE(ParseEvalPlan("{\"schema\": 2}").has_value());
    const auto no_hash = ParseEvalPlan("{\"schema\": 1, \"candidate_id\": \"cand-1\"}");
    REQUIRE_FALSE(no_hash.has_value());
    CHECK(no_hash.error().find("content_hash") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 密钥扫描与绝对路径扫描
// ---------------------------------------------------------------------------

TEST_CASE("评测.密钥扫描:键形态命中,占位值不冤枉") {
    const std::vector<ScanFinding> hits = ScanTextForSecrets(
        "token: abc123realmadness\n"
        "api_key = \"sk-live-abcdef123456\"\n"
        "password: [已打码]\n"
        "client_secret: {{secret}}\n"
        "Authorization: Bearer xx-token-placeholder\n");
    // 三处:token 键形态、api_key 键形态、值里的裸 sk- key(同一行各记一笔);
    // Authorization 的值是 placeholder 样例,不冤枉。
    CHECK(hits.size() == 3);
    bool saw_token = false;
    bool saw_api_key = false;
    bool saw_bare_sk = false;
    for (const ScanFinding& finding : hits) {
        CHECK(finding.kind == "secret");
        CHECK(finding.detail.find("abc") == std::string::npos);  // 不回显值
        if (finding.detail.find("token") != std::string::npos) {
            saw_token = true;
        }
        if (finding.detail.find("api_key") != std::string::npos) {
            saw_api_key = true;
        }
        if (finding.detail.find("sk-") != std::string::npos) {
            saw_bare_sk = true;
        }
    }
    CHECK(saw_token);
    CHECK(saw_api_key);
    CHECK(saw_bare_sk);

    // 裸 sk- key:整段长 Token,不带键名也要抓。
    const auto bare = ScanTextForSecrets("key = sk-abcdef0123456789abcdef");
    REQUIRE(bare.size() == 1);
    CHECK(bare[0].detail.find("sk-") != std::string::npos);

    // 占位与打码不算密钥;Bearer 占位也不算。
    CHECK(ScanTextForSecrets("token: [已打码]\napi_key: <your-key>\nsecret: {{token}}\n"
                             "password: ${ENV_PWD}\ncookie: null\nauth: xxxxxx")
              .empty());
}

TEST_CASE("评测.绝对路径扫描:盘符/UNC/家目录命中,URL 不冤枉") {
    const std::vector<ScanFinding> hits =
        ScanTextForAbsolutePaths("读 C:\\Users\\someone\\cfg.yaml\n"
                                 " UNC \\\\fileshare\\cfg\n"
                                 " 家目录 ~/.lubancode/packages\n"
                                 " POSIX /home/dev/work\n"
                                 " macOS /Users/dev/work\n"
                                 "正常网址 https://example.com/a 与 http://x.y 都不算\n"
                                 "相对路径 cfg/provider.yaml 也不算\n");
    CHECK(hits.size() == 5);
    int drive = 0;
    int unc = 0;
    int home = 0;
    int posix_home = 0;
    int users = 0;
    for (const ScanFinding& finding : hits) {
        CHECK(finding.kind == "absolute-path");
        if (finding.detail.find("盘符") != std::string::npos) {
            ++drive;
        }
        if (finding.detail.find("UNC") != std::string::npos) {
            ++unc;
        }
        if (finding.detail.find("家目录") != std::string::npos) {
            ++home;
        }
        if (finding.detail.find("/home/") != std::string::npos) {
            ++posix_home;
        }
        if (finding.detail.find("/Users/") != std::string::npos) {
            ++users;  // /Users/ 同走 POSIX 分支,detail 里带样例
        }
    }
    CHECK(drive == 1);
    CHECK(unc == 1);
    CHECK(home == 1);
    CHECK(posix_home == 1);
    CHECK(users == 1);
}

// ---------------------------------------------------------------------------
// 确定性检查执行
// ---------------------------------------------------------------------------

TEST_CASE("评测.确定性检查:file/json/contains/command 与 manual") {
    TempDir temp;
    const fs::path workspace = temp.Get() / "ws";
    REQUIRE(WriteText(workspace / "report.json", "{\"findings\": [{\"id\": 1}]}"));
    REQUIRE(WriteText(workspace / "broken.json", "{oops"));

    EvalPlan plan;  // budget 不设帽
    EvalTask task;
    task.task_id = "t1";
    task.workspace = "ws";
    task.acceptance = {
        AcceptanceCheck{AcceptanceCheckKind::FileExists, "report.json", "report.json", "", ""},
        AcceptanceCheck{AcceptanceCheckKind::FileExists, "missing.json", "missing.json", "", ""},
        AcceptanceCheck{AcceptanceCheckKind::JsonParses, "report.json", "report.json", "", ""},
        AcceptanceCheck{AcceptanceCheckKind::JsonParses, "broken.json", "broken.json", "", ""},
        AcceptanceCheck{AcceptanceCheckKind::FileContains, "report.json", "report.json", "findings",
                        ""},
        AcceptanceCheck{AcceptanceCheckKind::Command, OkCommand(), "", "", OkCommand()},
        AcceptanceCheck{AcceptanceCheckKind::Command, FailCommand(), "", "", FailCommand()},
        AcceptanceCheck{AcceptanceCheckKind::Manual, "人工看看", "", "", ""},
    };
    const TaskRunResult run = RunEvalTask("replay", task, "cand-x", "sha256:aa",
                                          temp.Get(), plan);
    const EvalResultLine& line = run.line;
    REQUIRE(line.checks.size() == 8);
    CHECK(line.checks[0].pass);
    CHECK_FALSE(line.checks[1].pass);
    CHECK(line.checks[2].pass);
    CHECK_FALSE(line.checks[3].pass);
    CHECK(line.checks[4].pass);
    CHECK(line.checks[5].pass);  // exit 0
    CHECK_FALSE(line.checks[6].pass);  // exit 3
    CHECK(line.checks[7].skipped);     // 人工:没测,不冒充
    CHECK(line.outcome == "fail");     // 7 可执行,过 4
    CHECK(line.metrics.acceptance_rate == doctest::Approx(4.0 / 7.0));
    CHECK(line.metrics.tool_calls == 2);  // 两条 command 各算一次
    CHECK(line.metrics.tokens == 0);      // 不起模型,tokens 恒 0
    CHECK(line.metrics.workspace_writes == 0);
    // unverified:没测到的写明
    bool saw_model = false;
    bool saw_agent = false;
    bool saw_manual = false;
    for (const std::string& item : line.unverified) {
        saw_model = saw_model || item == "model-in-the-loop";
        saw_agent = saw_agent || item == "agent-metrics";
        saw_manual = saw_manual || item == "manual-acceptance";
    }
    CHECK(saw_model);
    CHECK(saw_agent);
    CHECK(saw_manual);
    CHECK_FALSE(run.fixture_missing);
}

TEST_CASE("评测.确定性检查:workspace 缺失是没测,不是测砸") {
    TempDir temp;
    EvalPlan plan;
    EvalTask task;
    task.task_id = "t1";
    task.workspace = "no-such-dir";
    task.acceptance = {
        AcceptanceCheck{AcceptanceCheckKind::FileExists, "report.json", "report.json", "", ""},
    };
    const TaskRunResult run = RunEvalTask("replay", task, "cand-x", "sha256:aa", temp.Get(), plan);
    CHECK(run.fixture_missing);
    CHECK(run.line.outcome == "skipped");
    CHECK(run.line.checks[0].skipped);
    bool saw = false;
    for (const std::string& item : run.line.unverified) {
        saw = saw || item == "fixture-missing";
    }
    CHECK(saw);
}

TEST_CASE("评测.预算:tool calls 越帽即 fail") {
    TempDir temp;
    const fs::path workspace = temp.Get() / "ws";
    REQUIRE(WriteText(workspace / "report.json", "{}"));
    EvalPlan plan;
    plan.budget_max_tool_calls = 1;
    plan.budget_timeout_ms = 60000;
    EvalTask task;
    task.task_id = "t1";
    task.workspace = "ws";
    task.acceptance = {
        AcceptanceCheck{AcceptanceCheckKind::Command, OkCommand(), "", "", OkCommand()},
        AcceptanceCheck{AcceptanceCheckKind::Command, OkCommand(), "", "", OkCommand()},
    };
    const TaskRunResult run = RunEvalTask("replay", task, "cand-x", "sha256:aa", temp.Get(), plan);
    CHECK(run.line.metrics.tool_calls == 2);
    CHECK(run.line.outcome == "fail");  // 检查全过,预算越帽仍 fail
    bool saw = false;
    for (const std::string& note : run.line.notes) {
        saw = saw || note.find("越帽") != std::string::npos;
    }
    CHECK(saw);
}

// ---------------------------------------------------------------------------
// 静态门
// ---------------------------------------------------------------------------

TEST_CASE("评测.静态门:干净包过,密钥/绝对路径即 fail") {
    TempDir temp;
    const fs::path package_dir = temp.Get() / "package";
    REQUIRE(WriteText(package_dir / "package.yaml",
                      "schema: 1\nid: evolve.clean-skill\nversion: 0.1.0\nname: clean\n"
                      "description: 干净包\n"));
    REQUIRE(WriteText(package_dir / "skills" / "clean" / "SKILL.md",
                      "---\nname: clean\ndescription: 干净技能,只用相对路径 cfg/a.yaml。\n---\n"
                      "\n# 干净\n\n读 {{path}} 再说。\n"));

    const StaticGateResult clean = RunStaticGate(package_dir);
    CHECK(clean.pass());
    CHECK(clean.doctor_valid);
    CHECK(clean.findings.empty());
    CHECK(clean.components_total == 1);  // 一份 Skill
    CHECK(clean.components_ok == 1);

    // 塞一行密钥与一行绝对路径:发现即 error。
    REQUIRE(WriteText(package_dir / "skills" / "clean" / "SKILL.md",
                      "---\nname: clean\ndescription: 又脏了。\n---\n\napi_key: real-key-123\n"
                      "先把 D:/work/cfg 读一遍。\n"));
    const StaticGateResult dirty = RunStaticGate(package_dir);
    CHECK_FALSE(dirty.pass());
    REQUIRE(dirty.findings.size() == 2);
    int secrets = 0;
    int paths = 0;
    for (const ScanFinding& finding : dirty.findings) {
        if (finding.kind == "secret") {
            ++secrets;
        }
        if (finding.kind == "absolute-path") {
            ++paths;
        }
    }
    CHECK(secrets == 1);
    CHECK(paths == 1);
}

// ---------------------------------------------------------------------------
// Coordinator::Test 全链(评测与状态迁移的唯一写口)
// ---------------------------------------------------------------------------

TEST_CASE("评测.全链:propose -> 塞带验收命令的计划 -> test 出账 -> evaluated") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const MadeRecording made = MakeRecording(home / "recordings");
    ObservationStore observations(home / "evolution" / "observations");
    EvolutionCoordinator coordinator(home / "package-candidates", &observations);
    const auto proposed = coordinator.ProposeRecording(made.status, made.events);
    REQUIRE(proposed.has_value());

    // propose 落的最小计划:replay 指回来源录制,验收是口述(人工)。
    {
        const auto plan_text = ReadText(proposed->candidate_dir / "eval-plan.json");
        REQUIRE(plan_text.has_value());
        const auto plan = ParseEvalPlan(*plan_text);
        REQUIRE(plan.has_value());
        REQUIRE(plan->replay.size() == 1);
        CHECK(plan->replay[0].task_id == made.status.id);
        CHECK(plan->replay[0].acceptance.size() == 1);
        CHECK(plan->replay[0].acceptance[0].kind == AcceptanceCheckKind::Manual);
        CHECK(plan->holdout.empty());
        CHECK(plan->baseline_kind == "bare-agent");
    }

    // ---- 塞夹具:workspace + 验收命令 + 基线指标账 ----
    const fs::path candidate_dir = proposed->candidate_dir;
    REQUIRE(WriteText(candidate_dir / "fixtures" / "replay-workspace" / "report.json",
                      "{\"findings\": [{\"id\": 1, \"source\": \"cfg/provider.yaml\"}]}"));
    REQUIRE(WriteText(candidate_dir / "fixtures" / "baseline-bare.json",
                      ComposeBaselineFixture(0.750000, 0.500000, 2)));
    REQUIRE(WriteText(candidate_dir / "eval-plan.json",
                      ComposeRichPlan(proposed->candidate_id, proposed->content_hash,
                                      "fixtures/replay-workspace", "fixtures/replay-workspace",
                                      "fixtures/baseline-bare.json", OkCommand(), 10)));

    // ---- test:五道门跑完 ----
    const auto report = coordinator.Test(proposed->candidate_id);
    REQUIRE(report.has_value());
    CHECK(report->static_gate.pass());
    CHECK(report->plan_loaded);
    CHECK(report->state_before == "drafted");
    CHECK(report->state_after == "evaluated");
    CHECK(report->transitioned_validated);
    CHECK(report->transitioned_evaluated);
    CHECK(report->fixture_missing_any == false);
    REQUIRE(report->appended.size() == 4);  // static + replay + holdout + baseline

    const EvalResultLine& replay_line =
        report->appended[1];  // 追加次序:static, replay, holdout, baseline
    CHECK(replay_line.gate == "replay");
    CHECK(replay_line.outcome == "pass");
    CHECK(replay_line.metrics.tool_calls == 1);  // 一条验收命令
    CHECK(replay_line.metrics.acceptance_rate == doctest::Approx(1.0));
    const EvalResultLine& holdout_line = report->appended[2];
    CHECK(holdout_line.gate == "holdout");
    CHECK(holdout_line.outcome == "pass");  // 机检过 + 人工一条跳过不挡
    CHECK(holdout_line.metrics.acceptance_rate == doctest::Approx(1.0));
    const EvalResultLine& baseline_line = report->appended[3];
    CHECK(baseline_line.gate == "baseline");
    CHECK(baseline_line.outcome == "pass");  // 候选 1.0/1.0 >= 基线 0.75/0.5
    CHECK(baseline_line.baseline_ref == "default-agent");
    CHECK(baseline_line.metrics.tool_calls == 2);  // 基线指标账原样入行

    // ---- 账本:只追加,seq 递增,行可回读 ----
    const std::vector<EvalResultLine> ledger = LoadEvalResults(candidate_dir / "eval-results.jsonl");
    REQUIRE(ledger.size() == 4);
    CHECK(ledger[0].seq == 1);
    CHECK(ledger[3].seq == 4);
    CHECK(ledger[0].gate == "static");
    CHECK(ledger[3].metrics.tokens == 1200);  // 基线指标回读不丢
    CHECK(coordinator.store().Find(proposed->candidate_id)->state == CandidateState::Evaluated);

    // ---- 汇总:通过几项、没测什么、比基线贵多少 ----
    const EvalSummary& summary = report->ledger_summary;
    CHECK(summary.static_gate.pass == 1);
    CHECK(summary.replay.pass == 1);
    CHECK(summary.holdout.pass == 1);
    CHECK(summary.baseline.pass == 1);
    CHECK(summary.checks_passed == 6);  // static 1 + replay 4 + holdout 1 + baseline 0
    CHECK(summary.checks_failed == 0);
    CHECK(summary.checks_skipped == 1);  // holdout 的人工一条
    CHECK(summary.has_holdout);
    CHECK(summary.has_baseline_metrics);
    CHECK(summary.tool_calls.has_baseline);
    CHECK(summary.tool_calls.candidate == 1.0);
    CHECK(summary.tool_calls.baseline == 2.0);
    CHECK(summary.tool_calls.delta == -1.0);
    CHECK(summary.tokens.candidate == 0.0);  // 确定性代跑零 token
    CHECK(summary.tokens.baseline == 1200.0);
    CHECK(report->exit_code == 0);

    const std::string verdict = BuildDeterministicVerdict(summary);
    CHECK(verdict.find("通过 6 项检查") != std::string::npos);
    CHECK(verdict.find("没测到") != std::string::npos);
    CHECK(verdict.find("model-in-the-loop") != std::string::npos);
    CHECK(verdict.find("real-service") != std::string::npos);  // 基线侧的 unverified 并进账面
    CHECK(verdict.find("对照基线") != std::string::npos);
    CHECK(verdict.find("tool calls 1 对 2") != std::string::npos);
    CHECK(verdict.find("未接评判模型") != std::string::npos);  // 判词不冒充模型

    // ---- 重跑:账照追加,状态不非法迁移 ----
    const auto again = coordinator.Test(proposed->candidate_id);
    REQUIRE(again.has_value());
    CHECK(again->state_before == "evaluated");
    CHECK(again->state_after == "evaluated");
    CHECK_FALSE(again->transitioned_evaluated);
    CHECK(LoadEvalResults(candidate_dir / "eval-results.jsonl").size() == 8);
}

TEST_CASE("评测.计划过期:候选内容变过,hash 对不上拒评") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const MadeRecording made = MakeRecording(home / "recordings");
    EvolutionCoordinator coordinator(home / "package-candidates", nullptr);
    const auto proposed = coordinator.ProposeRecording(made.status, made.events);
    REQUIRE(proposed.has_value());

    // 改候选一字:哈希变,旧计划作废。
    const fs::path skill_path =
        proposed->candidate_dir / "package" /
        lubancode::platform::Utf8ToPath(proposed->skill_rel_path);
    const auto skill_text = ReadText(skill_path);
    REQUIRE(skill_text.has_value());
    REQUIRE(WriteText(skill_path, *skill_text + "\n改了一行。\n"));
    CHECK_NE(ComputeCandidateContentHash(proposed->candidate_dir / "package"),
             proposed->content_hash);

    const auto report = coordinator.Test(proposed->candidate_id);
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().find("哈希") != std::string::npos);
}

TEST_CASE("评测.退码:静态门有 fail 给 1;夹具缺失给 2") {
    TempDir temp;
    const fs::path home = temp.Get() / ".lubancode";
    const MadeRecording made = MakeRecording(home / "recordings");
    EvolutionCoordinator coordinator(home / "package-candidates", nullptr);
    const auto proposed = coordinator.ProposeRecording(made.status, made.events);
    REQUIRE(proposed.has_value());

    // workspace 指向不存在的目录:没测,exit 2。
    REQUIRE(WriteText(proposed->candidate_dir / "eval-plan.json",
                      ComposeRichPlan(proposed->candidate_id, proposed->content_hash,
                                      "fixtures/no-such", "fixtures/no-such", "", OkCommand(),
                                      10)));
    const auto missing = coordinator.Test(proposed->candidate_id);
    REQUIRE(missing.has_value());
    CHECK(missing->fixture_missing_any);
    CHECK(missing->exit_code == 2);
    CHECK(missing->state_after == "evaluated");  // 门跑完了(结果是被跳过)
    // 候侧行全 skipped,基线无夹具:对照无从判。
    bool any_fail = false;
    for (const EvalResultLine& line : missing->appended) {
        any_fail = any_fail || line.outcome == "fail";
    }
    CHECK_FALSE(any_fail);

    // 静态门 fail(塞密钥):exit 1,状态停在 drafted。
    const MadeRecording made2 = MakeRecording(temp.Get() / ".lubancode2" / "recordings");
    const auto second = coordinator.ProposeRecording(made2.status, made2.events);
    REQUIRE(second.has_value());
    const fs::path skill2 =
        second->candidate_dir / "package" /
        lubancode::platform::Utf8ToPath(second->skill_rel_path);
    REQUIRE(WriteText(skill2, "---\nname: dirty\ndescription: d\n---\n\napi_key: real-key-9\n"));
    const std::string dirty_hash = ComputeCandidateContentHash(second->candidate_dir / "package");
    REQUIRE(WriteText(second->candidate_dir / "eval-plan.json",
                      ComposeRichPlan(second->candidate_id, dirty_hash, "", "", "", OkCommand(),
                                      10)));
    const auto dirty_report = coordinator.Test(second->candidate_id);
    REQUIRE(dirty_report.has_value());
    CHECK_FALSE(dirty_report->static_gate.pass());
    CHECK(dirty_report->static_gate.findings.size() == 1);
    CHECK(dirty_report->state_after == "drafted");  // 静态门不过,不迁 validated
    CHECK(dirty_report->exit_code == 1);
}

TEST_CASE("评测.退码函数与坏行恢复") {
    EvalSummary summary;
    CHECK(EvalExitCode(summary, true, false) == 0);
    summary.replay.fail = 1;
    CHECK(EvalExitCode(summary, true, false) == 1);
    CHECK(EvalExitCode(summary, false, false) == 2);   // 计划读不出
    CHECK(EvalExitCode(summary, true, true) == 2);     // 夹具缺失
    summary.unverified.push_back("fixture-missing");
    CHECK(EvalExitCode(summary, true, false) == 2);    // 账面里的缺夹具也算

    // 行序列化回读;半截行/坏行跳过,不废整账。
    EvalResultLine line;
    line.seq = 7;
    line.gate = "replay";
    line.task_id = "t";
    line.candidate_id = "c";
    line.content_hash = "sha256:bb";
    line.outcome = "pass";
    line.recorded_at = "2026-08-28T10:00:00Z";
    const std::string serialized = SerializeEvalResultLine(line);
    const auto back = ParseEvalResultLine(serialized);
    REQUIRE(back.has_value());
    CHECK(back->seq == 7);
    CHECK(back->gate == "replay");
    CHECK(back->outcome == "pass");
    CHECK_FALSE(ParseEvalResultLine("{half").has_value());
}
