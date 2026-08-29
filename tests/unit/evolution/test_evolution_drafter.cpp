// 自进化闭环阶段 2:起草器单测。钉四件事——
//   1. package.yaml 过 src/package 的 manifest 严格解析(id 两段式 evolve.*,
//      版本 0.1.0);
//   2. SKILL.md 复用现有 skill drafter(偶然值抽象成 {{date}}/{{url}}/
//      {{path}} 占位,正文查无原始绝对路径)且过安装校验;
//   3. 稳定失败路进"排错"节,偶然的失败重试不进;
//   4. 半截录制(没有 record_stop)起不出候选。

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "evolution/drafter.hpp"
#include "package/manifest.hpp"
#include "skills/skill_drafter.hpp"
#include "skills/workflow_recorder.hpp"

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_evolution_drafter_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

// 造一场完整录制件(真落盘,再整读——与采集器测试同一路数)。REQUIRE
// 只留在 void 的 TEST_CASE 里,帮手用 CHECK + 空返回兜。
struct MadeRecording {
    lubancode::skills::RecordingStatus status;
    std::vector<lubancode::skills::RecordEvent> events;
};

MadeRecording MakeRecording(const fs::path& recordings_root, const std::string& name,
                            const std::string& goal, const std::string& acceptance,
                            const std::string& verification) {
    lubancode::skills::RecordingStartInfo info;
    info.name = name;
    info.goal = goal;
    info.variables = {"region"};
    info.acceptance = acceptance;
    info.cwd = "D:/nowhere";
    auto recorder = lubancode::skills::WorkflowRecorder::Start(recordings_root, info);
    CHECK(recorder.has_value());
    if (!recorder.has_value()) {
        return {};
    }
    CHECK(recorder->Stop(verification).has_value());
    MadeRecording made;
    made.status.id = recorder->id();
    made.status.name = name;
    made.status.dir = recorder->dir();
    made.status.finished = true;
    // Stop 之后整读(verification/record_stop 两条也在)。
    made.events = lubancode::skills::ReadRecordingEvents(recorder->dir());
    return made;
}

}  // namespace

TEST_CASE("起草:最小 content-only 包,过 manifest 严格解析与 SKILL 安装校验") {
    TempDir temp;
    const MadeRecording made = MakeRecording(
        temp.Get() / "recordings", "provider 绑定排查", "排查 provider 绑定,按 2026-08-28 的账对",
        "ctest 全绿", "跑 ctest -C Debug,225 册全过");

    const auto draft = lubancode::evolution::DraftSkillCandidate(made.status, made.events);
    REQUIRE(draft.has_value());

    // ---- package.yaml:严格解析(单测钉死) ----
    const auto manifest = lubancode::package::ParsePackageManifest(draft->package_yaml);
    REQUIRE(manifest.has_value());
    CHECK(manifest->id.rfind("evolve.", 0) == 0);
    CHECK(manifest->version.text == "0.1.0");
    CHECK(manifest->name.empty() == false);
    CHECK(manifest->description.empty() == false);

    // ---- SKILL.md:过安装校验,slug 与包 id 末段一致 ----
    const auto skill_name = lubancode::skills::ValidateSkillMarkdownForInstall(draft->skill_markdown);
    REQUIRE(skill_name.has_value());
    CHECK("evolve." + *skill_name == manifest->id);
    CHECK(draft->skill_slug == *skill_name);

    // ---- 清单里也不焊死演示现场的值 ----
    CHECK(draft->package_yaml.find("2026-08-28") == std::string::npos);
    CHECK(draft->package_yaml.find("{{date}}") != std::string::npos);

    // ---- 偶然值抽象:正文只留占位(输入节的"演示值"提示除外,那是变量
    //     说明书的一部分——0.25.x 起草器的既定设计) ----
    CHECK(draft->skill_markdown.find("按 {{date}} 的账对") != std::string::npos);
    CHECK(draft->skill_markdown.find("{{date}}") != std::string::npos);
    CHECK(draft->skill_markdown.find("D:/nowhere") == std::string::npos);
    // goal 进 objective(演化账用),验收口述进正文验收节。
    CHECK(draft->objective.find("provider 绑定") != std::string::npos);
    CHECK(draft->skill_markdown.find("ctest 全绿") != std::string::npos);
}

TEST_CASE("起草:稳定失败路进排错节;偶然失败重试不进") {
    TempDir temp;
    const fs::path recordings_root = temp.Get() / "recordings";
    lubancode::skills::RecordingStartInfo info;
    info.name = "失败路示范";
    info.goal = "示范失败分支";
    info.acceptance = "有排错节";
    info.cwd = "D:/nowhere";
    auto recorder = lubancode::skills::WorkflowRecorder::Start(recordings_root, info);
    REQUIRE(recorder.has_value());  // TEST_CASE 是 void,REQUIRE 放心用

    // 1) 偶然重试:同工具同入参,先败后成——折成一步,不进排错。
    //    (RecordToolResult 的第二参是 is_error:true = 失败。)
    const nlohmann::json input{{"path", "a.cpp"}};
    recorder->RecordToolCall("read_file", input, "item-1", "toolu-1");
    recorder->RecordToolResult("read_file", true, "文件不存在", "error", "fs.not_found", "item-1");
    recorder->RecordToolCall("read_file", input, "item-2", "toolu-2");
    recorder->RecordToolResult("read_file", false, "读到 12 行", "", "", "item-2");
    // 2) 稳定失败:换入参的连败(最后仍失败)——进排错。
    recorder->RecordToolCall("run_command", nlohmann::json{{"command", "ping legacy-host"}}, "item-3",
                             "toolu-3");
    recorder->RecordToolResult("run_command", true, "解析不了主机名", "error", "net.dns", "item-3");
    REQUIRE(recorder->Stop("排错节有了").has_value());

    lubancode::skills::RecordingStatus status;
    status.id = recorder->id();
    status.name = info.name;
    status.dir = recorder->dir();
    const auto events = lubancode::skills::ReadRecordingEvents(recorder->dir());
    const auto draft = lubancode::evolution::DraftSkillCandidate(status, events);
    REQUIRE(draft.has_value());

    // 排错节在,记的是 run_command 的连败;偶然重试(read_file)不进。
    const std::size_t section = draft->skill_markdown.find("## 排错");
    CHECK(section != std::string::npos);
    const std::string tail = draft->skill_markdown.substr(section);
    CHECK(tail.find("run_command") != std::string::npos);
    CHECK(tail.find("read_file") == std::string::npos);
    // 补节之后仍过安装校验(正文完整、验收节在)。
    CHECK(lubancode::skills::ValidateSkillMarkdownForInstall(draft->skill_markdown).has_value());

    // 纯失败路抽取的直查:一条 run_command,没有 read_file。
    const auto modes = lubancode::skills::CollectStableFailureModes(events);
    REQUIRE(modes.size() == 1);
    CHECK(modes[0].tool == "run_command");
}

TEST_CASE("起草:半截录制起不出候选") {
    TempDir temp;
    const fs::path recordings_root = temp.Get() / "recordings";
    lubancode::skills::RecordingStartInfo info;
    info.name = "半截";
    info.goal = "g";
    info.acceptance = "a";
    info.cwd = "D:/nowhere";
    auto recorder = lubancode::skills::WorkflowRecorder::Start(recordings_root, info);
    REQUIRE(recorder.has_value());
    // 不 Stop,直接读事件——没有 record_stop。
    lubancode::skills::RecordingStatus status;
    status.id = recorder->id();
    status.name = info.name;
    status.dir = recorder->dir();
    const auto events = lubancode::skills::ReadRecordingEvents(recorder->dir());
    const auto draft = lubancode::evolution::DraftSkillCandidate(status, events);
    REQUIRE(!draft.has_value());
    CHECK(draft.error().find("record_stop") != std::string::npos);
}
