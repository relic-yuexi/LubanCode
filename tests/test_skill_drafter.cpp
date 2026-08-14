// 录一遍生成技能:起草归纳(变量提取、失败重试剔掉、验收节)、frontmatter
// 校验回炉、草稿落盘、从草稿目录原子安装、脱敏贯穿录制→草稿全链路。

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/skill_drafter.hpp"
#include "agent/workflow_recorder.hpp"
#include "config/skill_store.hpp"
#include "tools/skill_loader.hpp"

namespace {

namespace fs = std::filesystem;
using lubancode::agent::RecordEvent;
using lubancode::agent::RecordingStartInfo;

class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        root_ = fs::temp_directory_path() /
                ("lubancode_drafter_test_" + tag + "_" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    const fs::path& Root() const { return root_; }

private:
    fs::path root_;
};

RecordEvent Ev(const std::string& type, nlohmann::json data) {
    static std::int64_t seq = 0;
    RecordEvent event;
    event.seq = ++seq;
    event.ts = "2026-08-14 10:00:00";
    event.source = "model";
    event.type = type;
    event.data = std::move(data);
    return event;
}

// 一段"读文件→改文件→跑测试"的演示事件流。
std::vector<RecordEvent> DemoEvents() {
    std::vector<RecordEvent> events;
    RecordEvent start;
    start.seq = 1;
    start.ts = "2026-08-14 10:00:00";
    start.source = "user";
    start.type = lubancode::agent::kEventRecordStart;
    start.data = {{"name", "weekly-report"},
                  {"goal", "生成 D:/demo 仓库的周报"},
                  {"variables", nlohmann::json::array({"日期"})},
                  {"acceptance", "测试全绿,周报落盘"},
                  {"cwd", "D:/demo"}};
    events.push_back(std::move(start));

    events.push_back(Ev(lubancode::agent::kEventToolCall,
                        {{"tool", "read_file"}, {"input", nlohmann::json{{"path", "D:/demo/src/main.cpp"}}}}));
    events.push_back(Ev(lubancode::agent::kEventToolResult, {{"tool", "read_file"}, {"ok", true},
                                                             {"summary", "读出 120 行"}}));
    events.push_back(Ev(lubancode::agent::kEventToolCall,
                        {{"tool", "edit_file"},
                         {"input", nlohmann::json{{"path", "D:/demo/src/main.cpp"}, {"new_string", "fix"}}}}));
    events.push_back(Ev(lubancode::agent::kEventToolResult, {{"tool", "edit_file"}, {"ok", true},
                                                             {"summary", "替换 1 处"}}));
    events.push_back(Ev(lubancode::agent::kEventToolCall,
                        {{"tool", "run_command"},
                         {"input", nlohmann::json{{"command", "ctest --date 2026-08-14 D:/demo/out-2026-08-14"}}}}));
    events.push_back(Ev(lubancode::agent::kEventToolResult, {{"tool", "run_command"}, {"ok", true},
                                                             {"summary", "100% tests passed"}}));
    events.push_back(Ev(lubancode::agent::kEventUserNote, {{"text", "先跑测试再改文案"}}));
    events.push_back(Ev(lubancode::agent::kEventVerification, {{"text", "ctest 全绿"}, {"ok", true}}));
    return events;
}

std::string SectionBetween(const std::string& text, const std::string& begin, const std::string& end) {
    const std::size_t b = text.find(begin);
    if (b == std::string::npos) {
        return "";
    }
    const std::size_t e = text.find(end, b);
    return text.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

}  // namespace

TEST_CASE("起草:读文件/改文件/跑测试的演示产出合法 SKILL.md") {
    const std::string draft = lubancode::agent::ComposeSkillMarkdown(DemoEvents());

    // frontmatter 过现有解析器,name/description 齐全
    const auto validated = lubancode::agent::ValidateSkillMarkdownForInstall(draft);
    REQUIRE(validated.has_value());
    CHECK(*validated == "weekly-report");
    const auto parsed = lubancode::tools::ParseSkillMarkdown(draft);
    REQUIRE(parsed.has_value());
    CHECK(parsed->description->find("周报") != std::string::npos);

    // 具体日期提成输入:步骤节里没有死值,变量占位在
    const std::string steps = SectionBetween(draft, "## 步骤", "## 风险动作与确认点");
    CHECK(steps.find("{{date}}") != std::string::npos);
    CHECK(steps.find("2026-08-14") == std::string::npos);
    // 绝对路径剥成相对:步骤节里没有盘符路径,相对路径照写
    CHECK(steps.find("D:") == std::string::npos);
    CHECK(steps.find("src/main.cpp") != std::string::npos);
    CHECK(steps.find("out-{{date}}") != std::string::npos);

    // 输入节把日期列为要填的值
    const std::string inputs = SectionBetween(draft, "## 输入", "## 步骤");
    CHECK(inputs.find("{{date}}") != std::string::npos);
    CHECK(inputs.find("日期") != std::string::npos);

    // 风险动作与验收
    const std::string risks = SectionBetween(draft, "## 风险动作与确认点", "## 验收");
    CHECK(risks.find("run_command") != std::string::npos);
    CHECK(risks.find("edit_file") != std::string::npos);
    const std::string acceptance = SectionBetween(draft, "## 验收", "## 备注");
    CHECK(acceptance.find("测试全绿,周报落盘") != std::string::npos);
    CHECK(acceptance.find("ctest 全绿") != std::string::npos);
}

TEST_CASE("起草:模型长输出不进正文,工具结果只用短摘要") {
    const std::vector<RecordEvent> events = DemoEvents();
    // 上面事件流的 tool_result 本来就只有一行摘要;再验一把超长摘要被截
    std::vector<RecordEvent> fat = events;
    fat.push_back(Ev(lubancode::agent::kEventToolCall, {{"tool", "search"}, {"input", nlohmann::json{{"q", "x"}}}}));
    fat.push_back(Ev(lubancode::agent::kEventToolResult,
                     {{"tool", "search"}, {"ok", true}, {"summary", std::string(500, 'Z')}}));
    const std::string draft = lubancode::agent::ComposeSkillMarkdown(fat);
    std::size_t count = 0;
    for (std::size_t at = draft.find('Z'); at != std::string::npos; at = draft.find('Z', at + 1)) {
        ++count;
    }
    CHECK(count <= 100);  // 超长摘要截断,草稿不背大段输出
}

TEST_CASE("起草:偶然的失败重试剔掉,连败写成分支") {
    std::vector<RecordEvent> events = DemoEvents();
    const nlohmann::json retry_input = {{"command", "ctest --rerun"}};
    // 同工具同入参:失败→成功,折成一步
    events.push_back(Ev(lubancode::agent::kEventToolCall, {{"tool", "run_command"}, {"input", retry_input}}));
    events.push_back(
        Ev(lubancode::agent::kEventToolResult, {{"tool", "run_command"}, {"ok", false}, {"summary", "exit 1"}}));
    events.push_back(Ev(lubancode::agent::kEventToolCall, {{"tool", "run_command"}, {"input", retry_input}}));
    events.push_back(Ev(lubancode::agent::kEventToolResult,
                        {{"tool", "run_command"}, {"ok", true}, {"summary", "passed"}}));
    // 另一入参连败两次:留成分支
    const nlohmann::json fail_input = {{"command", "deploy --canary"}};
    events.push_back(Ev(lubancode::agent::kEventToolCall, {{"tool", "run_command"}, {"input", fail_input}}));
    events.push_back(Ev(lubancode::agent::kEventToolResult,
                        {{"tool", "run_command"}, {"ok", false}, {"summary", "canary 500"}}));
    events.push_back(Ev(lubancode::agent::kEventToolCall, {{"tool", "run_command"}, {"input", fail_input}}));
    events.push_back(Ev(lubancode::agent::kEventToolResult,
                        {{"tool", "run_command"}, {"ok", false}, {"summary", "canary 503"}}));

    const std::string draft = lubancode::agent::ComposeSkillMarkdown(events);
    const std::string steps = SectionBetween(draft, "## 步骤", "## 风险动作与确认点");
    // 重试折一步:整份步骤节里 rerun 只出现一次,且注明失败过
    CHECK(steps.find("ctest --rerun") != std::string::npos);
    std::size_t rerun_count = 0;
    for (std::size_t at = steps.find("ctest --rerun"); at != std::string::npos;
         at = steps.find("ctest --rerun", at + 1)) {
        ++rerun_count;
    }
    CHECK(rerun_count == 1);
    CHECK(steps.find("失败过 1 次") != std::string::npos);
    // 连败留分支
    CHECK(steps.find("若失败") != std::string::npos);
    CHECK(steps.find("canary 503") != std::string::npos);
}

TEST_CASE("校验:frontmatter 不合法不许装,回炉保正文") {
    const auto ok = lubancode::agent::ValidateSkillMarkdownForInstall(
        "---\nname: a\ndescription: b\n---\n## 验收\n- x\n");
    REQUIRE(ok.has_value());
    CHECK(*ok == "a");

    CHECK_FALSE(lubancode::agent::ValidateSkillMarkdownForInstall("---\nname: a\n---\n## 验收\n- x\n").has_value());
    CHECK_FALSE(lubancode::agent::ValidateSkillMarkdownForInstall(
                    "---\nname: a\ndescription: b\n---\n正文里没有成事标准\n")
                    .has_value());
    CHECK_FALSE(lubancode::agent::ValidateSkillMarkdownForInstall("---\nname: a\ndescription: b\n正文").has_value());

    // 回炉:损坏 frontmatter 推倒重建,正文保住
    const std::string repaired = lubancode::agent::RepairSkillFrontmatter(
        "---\nname: a\ndescription: b\n## 验收\n- 正文还在\n");
    CHECK(lubancode::agent::ValidateSkillMarkdownForInstall(repaired).has_value());
    CHECK(repaired.find("正文还在") != std::string::npos);
    CHECK(*lubancode::agent::ValidateSkillMarkdownForInstall(repaired) == "recorded-skill");
}

TEST_CASE("落草稿 + 从草稿目录原子安装") {
    TempDir temp("install");
    const fs::path recording_dir = temp.Root() / "20260814-000000-weekly";
    fs::create_directories(recording_dir);

    const auto draft = lubancode::agent::WriteSkillDraft(recording_dir, DemoEvents());
    REQUIRE(draft.has_value());
    CHECK(fs::is_regular_file(recording_dir / "draft" / "SKILL.md"));
    REQUIRE(draft->files.size() == 1);
    CHECK(draft->files[0] == "SKILL.md");
    CHECK(draft->draft_dir == recording_dir / "draft");

    // 空事件起不出草稿
    const fs::path empty_dir = temp.Root() / "20260814-000001-empty";
    fs::create_directories(empty_dir);
    CHECK_FALSE(lubancode::agent::WriteSkillDraft(empty_dir, {}).has_value());

    // 原子安装:装进 skills_root/<frontmatter name>
    const fs::path skills_root = temp.Root() / "skills";
    const auto installed = lubancode::config::InstallDraftSkill(
        skills_root, draft->draft_dir,
        [](const std::string& content) { return lubancode::agent::ValidateSkillMarkdownForInstall(content); });
    REQUIRE(installed.has_value());
    REQUIRE(installed->installed_names.size() == 1);
    CHECK(installed->installed_names[0] == "weekly-report");
    CHECK(fs::is_regular_file(skills_root / "weekly-report" / "SKILL.md"));

    // 再装同名:拒绝,原目录不动
    CHECK_FALSE(lubancode::config::InstallDraftSkill(
                    skills_root, draft->draft_dir, [](const std::string& content) {
                        return lubancode::agent::ValidateSkillMarkdownForInstall(content);
                    })
                    .has_value());
    CHECK(fs::is_regular_file(skills_root / "weekly-report" / "SKILL.md"));

    // 校验不过:一个字节都不写
    const fs::path bad_draft = temp.Root() / "bad-draft";
    fs::create_directories(bad_draft);
    { std::ofstream file(bad_draft / "SKILL.md", std::ios::binary); file << "---\nname: bad\n没有闭合\n"; }
    const fs::path skills_root_2 = temp.Root() / "skills2";
    CHECK_FALSE(lubancode::config::InstallDraftSkill(
                    skills_root_2, bad_draft, [](const std::string& content) {
                        return lubancode::agent::ValidateSkillMarkdownForInstall(content);
                    })
                    .has_value());
    CHECK_FALSE(fs::exists(skills_root_2 / "bad"));
    // 装完的技能过得了现有扫描器,本场可加载
    const auto metas = lubancode::tools::ScanSkillsDir(skills_root, "项目级");
    REQUIRE(metas.size() == 1);
    CHECK(metas[0].name == "weekly-report");
}

TEST_CASE("脱敏贯穿全链路:录制→事件→草稿,grep 不到假 token") {
    TempDir temp("pipeline");
    RecordingStartInfo info;
    info.name = "deploy-demo";
    info.goal = "部署演示";
    info.acceptance = "部署成功";
    info.cwd = "D:\\demo";
    auto recorder = lubancode::agent::WorkflowRecorder::Start(temp.Root(), info);
    REQUIRE(recorder.has_value());
    recorder->RecordToolCall("run_command",
                             nlohmann::json{{"command", "curl -H \"Authorization: Bearer test-token-123\" https://ci.example.test"},
                                            {"api_key", "test-token-123"}});
    recorder->RecordToolResult("run_command", false, "deployed with test-token-123");
    const fs::path dir = recorder->dir();
    REQUIRE(recorder->Stop("部署成功").has_value());

    const std::vector<RecordEvent> events = lubancode::agent::ReadRecordingEvents(dir);
    const auto draft = lubancode::agent::WriteSkillDraft(dir, events);
    REQUIRE(draft.has_value());

    std::ifstream file(draft->draft_dir / "SKILL.md", std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string draft_text = buffer.str();
    CHECK(draft_text.find("test-token-123") == std::string::npos);
    // 打码痕迹与网址抽象都在
    CHECK(draft_text.find("[已打码]") != std::string::npos);
    CHECK(draft_text.find("{{url}}") != std::string::npos);
}
