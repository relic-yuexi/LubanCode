// P0-4 聚合诊断(§13.1):/doctor trajectory 的折叠口是只读聚合——现扫
// 现折现报,不持有运行期状态。这里钉 BuildSessionDoctorReport 的 stream
// 清单与容量并入、BuildWorkspaceDoctorReport 的磁盘余量、渲染成行、
// HasDiskReserve 的保守判。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "trajectory/directory.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/metrics.hpp"
#include "trajectory/recorder.hpp"

using namespace lubancode::trajectory;

namespace {

std::filesystem::path FreshDir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void PutFile(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out << content;
}

EventScope ScopeOf(const std::string& run_id, RunKind kind) {
    EventScope scope;
    scope.workspace_key = "demo-000000000000";
    scope.session_id = "20260831-120000-AAAAAA";
    scope.run_id = run_id;
    scope.run_kind = kind;
    scope.visibility = {Visibility::HostOnly};
    return scope;
}

// 建一间真 session:main 收口、子代理没收口、派生文件与 blob 各一。
std::filesystem::path FabricateSession(const std::filesystem::path& parent) {
    const auto session = parent / "20260831-120000-AAAAAA";
    std::error_code ec;
    std::filesystem::create_directories(session / "artifacts/sha256/ab", ec);
    PutFile(session / "artifacts/sha256/ab" / std::string(62, 'b'), "blob");
    PutFile(session / "derived/episode.json", "{}");
    // 真 manifest(空 JSON 过不了 FromJson,doctor 的 manifest_readable 要如实)。
    SessionManifest manifest;
    manifest.workspace_key = "demo-000000000000";
    manifest.session_id = "20260831-120000-AAAAAA";
    manifest.launch_cwd = "D:/work/demo";
    manifest.main_run_id = "main-0001";
    manifest.status = "closed";
    manifest.lubancode_version = "test";
    (void)WriteSessionJsonAtomic(session, manifest);

    {  // main:开张即收口(terminal 齐)。
        auto main = TrajectoryRecorder::Start(session / "main.jsonl", session / "artifacts",
                                              ScopeOf("main-0001", RunKind::MainSession));
        REQUIRE(main.has_value());
        main->WriteRunStarted(nlohmann::json{{"run_kind", "main_session"}}, Durability::PowerLoss);
        main->FinishRun(EventKind::RunCompleted, "done", Durability::PowerLoss);
        main->Close();
    }
    {  // 子代理:开了没收口(崩溃残留)。
        std::error_code sub_ec;
        std::filesystem::create_directories(session / "subagents", sub_ec);
        auto sub = TrajectoryRecorder::Start(session / "subagents/agent-1.jsonl",
                                             session / "artifacts",
                                             ScopeOf("agent-1-main-0001", RunKind::Subagent));
        REQUIRE(sub.has_value());
        EventLinks links;
        links.parent_run_id = "main-0001";
        sub->WriteRunStarted(nlohmann::json{{"run_kind", "subagent"}}, Durability::PowerLoss,
                             std::move(links));
        // 不 Finish、不 Close:unterminated。
    }
    return session;
}

}  // namespace

TEST_CASE("BuildSessionDoctorReport:全 stream 现扫,未收口数得清,容量并入") {
    const auto root = FreshDir("lubancode-p4-doctor");
    const auto session = FabricateSession(root);

    const auto report = BuildSessionDoctorReport(session);
    CHECK(report.session_id == "20260831-120000-AAAAAA");
    CHECK(report.manifest_readable);  // session.json 落了(哪怕空对象)
    REQUIRE(report.streams.size() == 2);

    // 两份 stream 各验链:main 完好且收口;子代理完好但未收口。
    bool saw_main = false;
    bool saw_agent = false;
    for (const auto& stream : report.streams) {
        if (stream.label == "main") {
            saw_main = true;
            CHECK(stream.verify.ok);
            CHECK(stream.run_terminal);
        } else if (stream.label == "agent:agent-1") {
            saw_agent = true;
            CHECK(stream.verify.ok);
            CHECK_FALSE(stream.run_terminal);
        }
    }
    CHECK(saw_main);
    CHECK(saw_agent);
    CHECK(report.unterminated_stream_count == 1);

    // 容量四笔同扫(§12.2 与 usage 同一口径)。
    CHECK(report.capacity.journal_files >= 3);  // main+sub+session.json
    CHECK(report.capacity.derived_files == 1);
    CHECK(report.capacity.referenced_blob_files == 1);
}

TEST_CASE("BuildWorkspaceDoctorReport + 渲染:磁盘余量、active session、队列高水位") {
    const auto root = FreshDir("lubancode-p4-doctor-ws");
    const auto workspaces = root / "trajectories/workspaces";
    const auto session = FabricateSession(workspaces / "demo-000000000000/sessions");
    (void)session;

    const auto report = BuildWorkspaceDoctorReport(root / "trajectories", workspaces / "demo-000000000000",
                                                   "demo-000000000000", std::string("20260831-120000-AAAAAA"),
                                                   {"io.append_failed:x"});
    CHECK(report.disk_space_known);
    CHECK(report.disk_free_bytes > 0);
    REQUIRE(report.active_session.has_value());
    CHECK(report.active_session->session_id == "20260831-120000-AAAAAA");
    CHECK(report.recent_errors == std::vector<std::string>{"io.append_failed:x"});
    // 单写者同步提交:高水位恒 1(§13.1 的指标以真实现为准,不虚标)。
    CHECK(report.queue_high_water_mark == 1);

    const auto lines = FormatWorkspaceDoctorReport(report);
    REQUIRE_FALSE(lines.empty());
    const auto has = [&lines](const std::string& needle) {
        return std::any_of(lines.begin(), lines.end(),
                           [&](const std::string& line) { return line.find(needle) != std::string::npos; });
    };
    CHECK(has("workspace: demo-000000000000"));
    CHECK(has("磁盘余量"));
    CHECK(has("队列高水位: 1"));
    CHECK(has("active session: 20260831-120000-AAAAAA"));
    CHECK(has("agent:agent-1"));
    CHECK(has("未收口 stream 数: 1"));
    CHECK(has("最近 I/O 错误"));
}

TEST_CASE("BuildWorkspaceDoctorReport:没有活动 session 只报概况") {
    const auto root = FreshDir("lubancode-p4-doctor-idle");
    std::error_code ec;
    std::filesystem::create_directories(root / "trajectories/workspaces/demo-x", ec);

    const auto report =
        BuildWorkspaceDoctorReport(root / "trajectories", root / "trajectories/workspaces/demo-x",
                                   "demo-x", std::optional<std::string>(), {});
    CHECK(report.disk_space_known);
    CHECK_FALSE(report.active_session.has_value());
    const auto lines = FormatWorkspaceDoctorReport(report);
    CHECK(std::any_of(lines.begin(), lines.end(), [](const std::string& line) {
        return line.find("active session: 无") != std::string::npos;
    }));
}

TEST_CASE("HasDiskReserve:余量查询失败/不足保守判,目录未建沿祖先探") {
    const auto root = FreshDir("lubancode-p4-reserve");
    CHECK(HasDiskReserve(root, 1));                    // 1 字节一定有
    CHECK_FALSE(HasDiskReserve(root, UINT64_MAX));     // 装不下宇宙的盘
    CHECK(HasDiskReserve(root / "not/created/yet", 1));  // 沿祖先探
    CHECK_FALSE(HasDiskReserve(root / "not/created/yet", UINT64_MAX));
}
