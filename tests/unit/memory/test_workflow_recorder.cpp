// 录一遍生成技能:录制状态机、events.jsonl 读写回放、崩溃恢复、脱敏、
// /record 命令解析。全部走临时目录,不碰真主目录。

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "skills/skill_drafter.hpp"
#include "skills/workflow_recorder.hpp"
#include "cli/slash_commands.hpp"
#include "config/skill_store.hpp"

namespace {

namespace fs = std::filesystem;
using lubancode::agent::RecordEvent;
using lubancode::agent::RecorderAction;
using lubancode::agent::RecorderState;
using lubancode::agent::RecordingStartInfo;
using lubancode::agent::WorkflowRecorder;

class TempRecordings {
public:
    TempRecordings() {
        root_ = fs::temp_directory_path() /
                ("lubancode_recorder_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);
    }
    ~TempRecordings() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    const fs::path& Root() const { return root_; }

private:
    fs::path root_;
};

std::string ReadFileBytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void WriteFileBytes(const fs::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
}

RecordingStartInfo DemoInfo() {
    RecordingStartInfo info;
    info.name = "release-demo";
    info.goal = "把本周的版本发出去";
    info.variables = {"日期", "版本号"};
    info.acceptance = "测试全绿,tag 打上";
    info.cwd = "D:\\work\\demo";
    return info;
}

}  // namespace

TEST_CASE("状态机:开录/暂停/续录/停止/取消的合法与非法迁移") {
    using A = RecorderAction;
    using S = RecorderState;
    CHECK(lubancode::agent::IsValidRecorderTransition(S::Inactive, A::Start));
    CHECK_FALSE(lubancode::agent::IsValidRecorderTransition(S::Inactive, A::Pause));
    CHECK_FALSE(lubancode::agent::IsValidRecorderTransition(S::Inactive, A::Resume));
    CHECK_FALSE(lubancode::agent::IsValidRecorderTransition(S::Inactive, A::Stop));
    CHECK_FALSE(lubancode::agent::IsValidRecorderTransition(S::Inactive, A::Cancel));

    CHECK(lubancode::agent::IsValidRecorderTransition(S::Recording, A::Pause));
    CHECK(lubancode::agent::IsValidRecorderTransition(S::Recording, A::Stop));
    CHECK(lubancode::agent::IsValidRecorderTransition(S::Recording, A::Cancel));
    CHECK_FALSE(lubancode::agent::IsValidRecorderTransition(S::Recording, A::Start));
    CHECK_FALSE(lubancode::agent::IsValidRecorderTransition(S::Recording, A::Resume));

    CHECK(lubancode::agent::IsValidRecorderTransition(S::Paused, A::Resume));
    CHECK(lubancode::agent::IsValidRecorderTransition(S::Paused, A::Stop));
    CHECK(lubancode::agent::IsValidRecorderTransition(S::Paused, A::Cancel));
    CHECK_FALSE(lubancode::agent::IsValidRecorderTransition(S::Paused, A::Start));
    CHECK_FALSE(lubancode::agent::IsValidRecorderTransition(S::Paused, A::Pause));
}

TEST_CASE("录制器:开录→工具事件→暂停静默→续录→停止走通,非法操作被拒") {
    TempRecordings temp;
    auto recorder = WorkflowRecorder::Start(temp.Root(), DemoInfo());
    REQUIRE(recorder.has_value());
    CHECK(recorder->state() == RecorderState::Recording);
    CHECK(fs::exists(recorder->dir() / "manifest.json"));
    CHECK(fs::exists(recorder->dir() / "events.jsonl"));

    // 开录三问落在 record_start 里
    std::vector<RecordEvent> events = lubancode::agent::ReadRecordingEvents(recorder->dir());
    REQUIRE_FALSE(events.empty());
    CHECK(events.front().type == lubancode::agent::kEventRecordStart);
    CHECK(events.front().data.at("goal") == "把本周的版本发出去");
    CHECK(events.front().data.at("acceptance") == "测试全绿,tag 打上");

    // 工具事件成对入盘
    const std::size_t count_after_start = events.size();
    recorder->RecordToolCall("read_file", nlohmann::json{{"path", "src/main.cpp"}});
    recorder->RecordToolResult("read_file", false, "读出 120 行\n后面还有");
    events = lubancode::agent::ReadRecordingEvents(recorder->dir());
    REQUIRE(events.size() == count_after_start + 2);
    CHECK(events[events.size() - 2].type == lubancode::agent::kEventToolCall);
    CHECK(events[events.size() - 2].source == "model");
    CHECK(events[events.size() - 2].data.at("tool") == "read_file");
    CHECK(events.back().type == lubancode::agent::kEventToolResult);
    CHECK(events.back().data.at("ok") == true);
    CHECK(events.back().data.at("summary") == "读出 120 行");  // 只留首行短摘要

    // 暂停:事件照旧在盘,但暂停期间的动作不录
    REQUIRE(recorder->Pause().has_value());
    CHECK(recorder->state() == RecorderState::Paused);
    const std::size_t count_when_paused = lubancode::agent::ReadRecordingEvents(recorder->dir()).size();
    recorder->RecordToolCall("run_command", nlohmann::json{{"command", "echo hi"}});
    CHECK(lubancode::agent::ReadRecordingEvents(recorder->dir()).size() == count_when_paused);
    CHECK_FALSE(recorder->Pause().has_value());   // 暂停中不能再暂停
    REQUIRE(recorder->Resume().has_value());
    CHECK_FALSE(recorder->Resume().has_value());  // 录制中不能续录
    CHECK(recorder->state() == RecorderState::Recording);

    // 备注(脱敏在这里也过一遍)
    REQUIRE(recorder->Note("这一步先跑测试,token=abc 备用").has_value());
    events = lubancode::agent::ReadRecordingEvents(recorder->dir());
    bool has_note = false;
    for (const RecordEvent& event : events) {
        if (event.type == lubancode::agent::kEventUserNote) {
            has_note = true;
            CHECK(event.data.at("text").get<std::string>().find("token=abc") == std::string::npos);
        }
    }
    CHECK(has_note);

    // 停止:record_stop 落盘,状态回 Inactive
    const fs::path dir = recorder->dir();
    const auto stopped = recorder->Stop("测试全绿");
    REQUIRE(stopped.has_value());
    CHECK(*stopped == dir);
    CHECK(recorder->state() == RecorderState::Inactive);
    CHECK_FALSE(recorder->Stop("").has_value());         // 停过了不能再停
    CHECK_FALSE(recorder->Note("再补一句").has_value());  // 停了就记不进
    events = lubancode::agent::ReadRecordingEvents(dir);
    CHECK(events.back().type == lubancode::agent::kEventRecordStop);

    // 盘点:这一场 finished,还没有草稿
    const auto listed = lubancode::agent::ListRecordings(temp.Root());
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].finished);
    CHECK_FALSE(listed[0].has_draft);
    CHECK(listed[0].name == "release-demo");
}

TEST_CASE("取消:整目录删除,已装好的技能不归它管") {
    TempRecordings temp;
    auto recorder = WorkflowRecorder::Start(temp.Root(), DemoInfo());
    REQUIRE(recorder.has_value());
    const fs::path dir = recorder->dir();
    REQUIRE(fs::exists(dir));
    REQUIRE(recorder->Cancel().has_value());
    CHECK_FALSE(fs::exists(dir));
    CHECK(lubancode::agent::ListRecordings(temp.Root()).empty());
}

TEST_CASE("events.jsonl:序列化/解析回放,坏行与半截行跳过") {
    RecordEvent event;
    event.seq = 7;
    event.ts = "2026-08-14 10:00:00";
    event.source = "model";
    event.type = lubancode::agent::kEventToolCall;
    event.data = {{"tool", "read_file"}, {"input", nlohmann::json{{"path", "a.txt"}}}};
    const std::string line = lubancode::agent::SerializeRecordEvent(event);
    const auto parsed = lubancode::agent::ParseRecordEvent(line);
    REQUIRE(parsed.has_value());
    CHECK(parsed->seq == 7);
    CHECK(parsed->ts == "2026-08-14 10:00:00");
    CHECK(parsed->source == "model");
    CHECK(parsed->type == lubancode::agent::kEventToolCall);
    CHECK(parsed->data.at("tool") == "read_file");

    CHECK_FALSE(lubancode::agent::ParseRecordEvent("not json").has_value());
    CHECK_FALSE(lubancode::agent::ParseRecordEvent("{\"seq\":\"x\"}").has_value());
    CHECK_FALSE(lubancode::agent::ParseRecordEvent("{}").has_value());

    // 崩溃截断的半截行:读回放时跳过,不废整场
    TempRecordings temp;
    const fs::path dir = temp.Root() / "20260814-000000-crash";
    fs::create_directories(dir);
    WriteFileBytes(dir / "events.jsonl", line + "\n{\"seq\":8,\"ts\":\"2026-08");  // 第二行写到一半崩了
    const std::vector<RecordEvent> events = lubancode::agent::ReadRecordingEvents(dir);
    CHECK(events.size() == 1);
}

TEST_CASE("崩溃恢复:半截录制件可列出、可丢弃,装不进 skills") {
    TempRecordings temp;
    const fs::path dir = temp.Root() / "20260814-120000-crashleft";
    fs::create_directories(dir);
    WriteFileBytes(dir / "manifest.json",
                   nlohmann::json{{"version", 1},
                                  {"id", "20260814-120000-crashleft"},
                                  {"name", "crashleft"},
                                  {"started_at", "2026-08-14 12:00:00"}}
                       .dump());
    WriteFileBytes(dir / "events.jsonl",
                   lubancode::agent::SerializeRecordEvent(RecordEvent{1, "2026-08-14 12:00:01", "user",
                                                                       lubancode::agent::kEventRecordStart,
                                                                       nlohmann::json::object()}) +
                       "\n");

    const auto listed = lubancode::agent::ListRecordings(temp.Root());
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].id == "20260814-120000-crashleft");
    CHECK_FALSE(listed[0].finished);  // 没有 record_stop:崩溃/没停
    CHECK_FALSE(listed[0].has_draft);

    // 半截录制件没有 draft/SKILL.md,原子安装接口直接拒收
    const fs::path skills_root = temp.Root() / "skills";
    const auto installed = lubancode::config::InstallDraftSkill(
        skills_root, dir, [](const std::string& content) {
            return lubancode::agent::ValidateSkillMarkdownForInstall(content);
        });
    CHECK_FALSE(installed.has_value());
    CHECK_FALSE(fs::exists(skills_root / "crashleft"));

    // 丢弃:整目录删掉;路径花招拒绝
    REQUIRE(lubancode::agent::DiscardRecording(temp.Root(), "20260814-120000-crashleft").has_value());
    CHECK_FALSE(fs::exists(dir));
    CHECK_FALSE(lubancode::agent::DiscardRecording(temp.Root(), "../evil").has_value());
    CHECK_FALSE(lubancode::agent::DiscardRecording(temp.Root(), "a/b").has_value());
    CHECK_FALSE(lubancode::agent::DiscardRecording(temp.Root(), "no-such-id").has_value());
}

TEST_CASE("脱敏:假 token 在录制件全文里 grep 不到") {
    const std::string token = "test-token-123";

    SUBCASE("值形态打码") {
        const std::string a = lubancode::agent::RedactSecrets("Authorization: Bearer " + token);
        CHECK(a.find(token) == std::string::npos);
        const std::string b = lubancode::agent::RedactSecrets("curl -H \"authorization: " + token + "\" url");
        CHECK(b.find(token) == std::string::npos);
        const std::string c = lubancode::agent::RedactSecrets("token=" + token + " env");
        CHECK(c.find(token) == std::string::npos);
        const std::string d = lubancode::agent::RedactSecrets("password=" + token);
        CHECK(d.find(token) == std::string::npos);
        const std::string e = lubancode::agent::RedactSecrets("key sk-abc123def456ghi789xyz");
        CHECK(e.find("sk-abc123def456ghi789xyz") == std::string::npos);
        // 无关文本原样保留
        CHECK(lubancode::agent::RedactSecrets("read README.md and run tests") ==
              "read README.md and run tests");
    }

    SUBCASE("入参字段抹掉 + 嵌套 + 值内打码") {
        const nlohmann::json sanitized = lubancode::agent::SanitizeToolInput(nlohmann::json{
            {"command", "curl -H \"Authorization: Bearer " + token + "\" https://example.test"},
            {"api_key", token},
            {"nested", nlohmann::json{{"auth_token", token}, {"path", "src/main.cpp"}}},
        });
        const std::string dump = sanitized.dump();
        CHECK(dump.find(token) == std::string::npos);
        CHECK(sanitized.at("api_key") == "[已打码]");
        CHECK(sanitized.at("nested").at("auth_token") == "[已打码]");
        CHECK(sanitized.at("nested").at("path") == "src/main.cpp");
        CHECK(sanitized.at("command").get<std::string>().find("example.test") != std::string::npos);
    }

    SUBCASE("录制件落盘全文无 token") {
        TempRecordings temp;
        auto recorder = WorkflowRecorder::Start(temp.Root(), DemoInfo());
        REQUIRE(recorder.has_value());
        recorder->RecordToolCall("run_command",
                                 nlohmann::json{{"command", "curl -H \"Authorization: Bearer " + token +
                                                                 "\" https://example.test"},
                                                {"api_key", token}});
        recorder->RecordToolResult("run_command", false, "done with " + token);
        const std::string all =
            ReadFileBytes(recorder->dir() / "events.jsonl") + ReadFileBytes(recorder->dir() / "manifest.json");
        CHECK(all.find(token) == std::string::npos);
    }
}

TEST_CASE("录制件命名:中文名落成安全 slug,危险字符清洗") {
    using lubancode::agent::MakeRecordingSlug;
    CHECK(MakeRecordingSlug("release demo") == "release-demo");
    CHECK(MakeRecordingSlug("发版演示") == "发版演示");  // 中文按码点保留
    CHECK(MakeRecordingSlug("a/b\\c:d*e?f") == "a-b-c-d-e-f");
    CHECK(MakeRecordingSlug("  ") == "recording");
    const std::string id_like = MakeRecordingSlug("---weird---");
    CHECK(id_like.find("..") == std::string::npos);
}

TEST_CASE("/record 命令解析:主命令与二级参数") {
    CHECK(lubancode::cli::ParseSlashCommand("/record").command == lubancode::cli::SlashCommand::Record);
    CHECK(lubancode::cli::ParseSlashCommand("记录").command == lubancode::cli::SlashCommand::NotSlash);

    const auto parse = [](const std::string& args) { return lubancode::cli::ParseRecordCommand(args); };
    using A = lubancode::cli::RecordCommandAction;

    CHECK(parse("").action == A::Status);
    CHECK(parse("status").action == A::Status);
    const auto start = parse("start release-demo");
    CHECK(start.action == A::Start);
    CHECK(start.name == "release-demo");
    CHECK(parse("start").action == A::Invalid);
    CHECK(parse("start a b").action == A::Invalid);
    const auto note = parse("note 这一步先跑测试");
    CHECK(note.action == A::Note);
    CHECK(note.text == "这一步先跑测试");
    CHECK(parse("note").action == A::Invalid);
    CHECK(parse("pause").action == A::Pause);
    CHECK(parse("resume").action == A::Resume);
    CHECK(parse("stop").action == A::Stop);
    CHECK(parse("cancel").action == A::Cancel);
    CHECK(parse("list").action == A::List);
    const auto install_default = parse("install 20260814-000000-x");
    CHECK(install_default.action == A::Install);
    CHECK(install_default.to_project);
    const auto install_home = parse("install 20260814-000000-x home");
    CHECK(install_home.action == A::Install);
    CHECK_FALSE(install_home.to_project);
    CHECK(parse("install 20260814-000000-x weird").action == A::Invalid);
    CHECK(parse("install").action == A::Invalid);
    const auto discard = parse("discard 20260814-000000-x");
    CHECK(discard.action == A::Discard);
    CHECK(parse("frobnicate x").action == A::Invalid);
}
