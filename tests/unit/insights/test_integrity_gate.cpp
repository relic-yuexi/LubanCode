// IntegrityGate 的册(Token 账本单 A4):analyzed/active/incomplete/corrupt
// /missing 五态各自成例;坏账整间排除且理由可见;终 hash 账齐(stale 判定
// 的源)。fixtures 直写 recorder(不走 session_manager),session.json 由
// 本册按场景补写——manifest 是产品接线写口,夹具侧如实模拟。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "insights/integrity_gate.hpp"
#include "trajectory/directory.hpp"

#include "insights_fixtures.hpp"

using namespace lubancode;
using namespace lubancode::insights;
using namespace lubancode::insights_fixtures;

namespace {

constexpr const char* kWorkspaceKey = "ws-000000000000";

void WriteSessionJson(const std::filesystem::path& dir, const std::string& session_id,
                      const std::string& status) {
    WriteFixtureSessionJson(dir, kWorkspaceKey, session_id, status);
}

// 一场完整封口 session(单 stream,带 usage)。
std::filesystem::path SealedSession(const std::filesystem::path& root, const char* tag,
                                    const std::string& session_id) {
    const auto dir = PrepareDir(root / tag);
    const FixedClock clock;
    FixtureStream stream(dir / "main.jsonl", dir / "artifacts", kWorkspaceKey, session_id,
                         "main-0001", trajectory::RunKind::MainSession, 2, clock);
    stream.StartRun();
    stream.StartTurn("turn-0001");
    UsageSpec usage;
    usage.input = 1000;
    usage.output = 100;
    stream.ModelExchange("turn-0001", "req-0001", "main_turn", usage);
    stream.EndTurn("turn-0001");
    stream.Seal();
    WriteSessionJson(dir, session_id, "closed");
    return dir;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

}  // namespace

TEST_CASE("analyzed:验过且封口,终 hash 账与事件都在") {
    const auto root = std::filesystem::temp_directory_path() / "lubancode-a4-gate";
    const auto dir = SealedSession(root, "ok", "20260831-000001-GATE01");
    const SessionGateReport report = GateSession(dir);
    REQUIRE(report.status == SessionGateStatus::Analyzed);
    CHECK(report.error_code.empty());
    CHECK(report.session_status == "closed");
    CHECK(report.workspace_key == kWorkspaceKey);
    REQUIRE(report.streams.size() == 1);
    CHECK(report.streams.front().first == "main-0001");
    REQUIRE(report.stream_terminal_hashes.contains("main-0001"));
    CHECK(!report.stream_terminal_hashes.at("main-0001").empty());
    CHECK(std::string(SessionGateStatusName(report.status)) == "analyzed");
}

TEST_CASE("active:未封口(session.json 非 closed)整间标 active,不放行") {
    const auto root = std::filesystem::temp_directory_path() / "lubancode-a4-gate-active";
    const auto dir = SealedSession(root, "active", "20260831-000002-GATE02");
    WriteSessionJson(dir, "20260831-000002-GATE02", "running");
    const SessionGateReport report = GateSession(dir);
    CHECK(report.status == SessionGateStatus::Active);
    CHECK(report.error_code == "gate.active");
    // include_active 是调用方的事;gate 只如实给状态。
}

TEST_CASE("incomplete:尾行截断,整间排除,理由点名") {
    const auto root = std::filesystem::temp_directory_path() / "lubancode-a4-gate-trunc";
    const auto dir = SealedSession(root, "trunc", "20260831-000003-GATE03");
    const auto path = dir / "main.jsonl";
    std::string content = ReadFile(path);
    REQUIRE(!content.empty());
    REQUIRE(content.back() == '\n');
    content.pop_back();
    WriteFile(path, content);
    const SessionGateReport report = GateSession(dir);
    REQUIRE(report.status == SessionGateStatus::Incomplete);
    CHECK(report.error_code == "gate.incomplete");
    bool noted = false;
    for (const auto& note : report.notes) {
        if (note.find("gate.stream_truncated") != std::string::npos) {
            noted = true;
        }
    }
    CHECK(noted);
    CHECK(report.streams.empty());
}

TEST_CASE("corrupt:中行改字节,hash 链断,整间排除") {
    const auto root = std::filesystem::temp_directory_path() / "lubancode-a4-gate-corrupt";
    const auto dir = SealedSession(root, "corrupt", "20260831-000004-GATE04");
    const auto path = dir / "main.jsonl";
    std::string content = ReadFile(path);
    const std::size_t middle = content.size() / 2;
    for (std::size_t i = middle; i < content.size(); ++i) {
        if (content[i] == '0') {
            content[i] = '1';
            break;
        }
    }
    WriteFile(path, content);
    const SessionGateReport report = GateSession(dir);
    REQUIRE(report.status == SessionGateStatus::Corrupt);
    CHECK(!report.error_code.empty());
    CHECK(report.streams.empty());
}

TEST_CASE("missing:目录不存在") {
    const SessionGateReport report = GateSession(std::filesystem::temp_directory_path() /
                                                 "lubancode-a4-gate-none");
    CHECK(report.status == SessionGateStatus::Missing);
    CHECK(report.error_code == "gate.session_missing");
}

TEST_CASE("session.json 缺席:status=unknown 按 active 口径,不冒充封口") {
    const auto root = std::filesystem::temp_directory_path() / "lubancode-a4-gate-nomanifest";
    const auto dir = SealedSession(root, "nomanifest", "20260831-000005-GATE05");
    std::error_code ec;
    std::filesystem::remove(dir / "session.json", ec);
    const SessionGateReport report = GateSession(dir);
    CHECK(report.status == SessionGateStatus::Active);
    CHECK(report.session_status == "unknown");
    bool noted = false;
    for (const auto& note : report.notes) {
        if (note.find("gate.session_manifest_missing") != std::string::npos) {
            noted = true;
        }
    }
    CHECK(noted);
}
