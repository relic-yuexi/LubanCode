// /goal 单第 4 期前半:goal 事件行 serialize/parse/roundtrip、SessionStore
// 落盘口、LoadedSession 整收、坏行跳过、老版本兼容(goal 行被旧 parse 当
// 坏行跳过的语义不破)。

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "sessions/goal_session.hpp"
#include "sessions/session_store.hpp"
#include "platform/text_encoding.hpp"  // IsValidUtf8:坏串落档行的出口校验

using lubancode::agent::GoalEvidenceRecord;
using lubancode::agent::GoalSessionEvent;
using lubancode::agent::ParseGoalEvent;
using lubancode::agent::ParseGoalEvidence;
using lubancode::agent::SerializeGoalEvent;
using lubancode::agent::SerializeGoalEvidence;

namespace {

// 每个测试一只独立临时目录(先关柄再删,remove_all 用 error_code)。
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        const auto base = std::filesystem::temp_directory_path();
        std::error_code ec;
        const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() ^
                                          (std::size_t)std::hash<std::string>{}("goal-test"));
        for (int i = 0; i < 100; ++i) {
            const auto candidate = base / ("wc-goal-test-" + nonce + "-" + std::to_string(i));
            std::filesystem::create_directories(candidate, ec);
            if (!ec) {
                path = candidate;
                return;
            }
        }
        path = base / "wc-goal-test-fallback";
        std::filesystem::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

GoalSessionEvent MakeCreated() {
    GoalSessionEvent e;
    e.type = "goal_v1";
    e.event = "created";
    e.goal_id = "goal-3";
    e.revision = 1;
    e.payload["objective"] = "迁移认证层";
    e.payload["objective_sha256"] = "abc123";
    e.timestamp_ms = 1720000000000;
    return e;
}

}  // namespace

TEST_CASE("goal 事件行:serialize → parse roundtrip 全字段") {
    const GoalSessionEvent created = MakeCreated();
    const std::string line = SerializeGoalEvent(created, "2026-08-23 10:00:00");
    REQUIRE(line.find("\"type\":\"goal_v1\"") != std::string::npos);
    REQUIRE(line.find("\"event\":\"created\"") != std::string::npos);
    REQUIRE(line.find('\n') == std::string::npos);

    const auto parsed = ParseGoalEvent(line);
    REQUIRE(parsed.has_value());
    CHECK(parsed->type == "goal_v1");
    CHECK(parsed->event == "created");
    CHECK(parsed->goal_id == "goal-3");
    CHECK(parsed->revision == 1);
    CHECK(parsed->payload.at("objective") == "迁移认证层");
    CHECK(parsed->timestamp_ms == 1720000000000);
    CHECK(parsed->iteration_id.empty());  // goal 级事件不带
}

TEST_CASE("goal 事件行:iteration 类带 iteration_id,五种 type 全认") {
    for (const char* type : {"goal_v1", "goal_iteration_v1", "goal_evidence_v1",
                             "goal_checkpoint_v1", "goal_evaluation_v1"}) {
        GoalSessionEvent e;
        e.type = type;
        e.event = "scheduled";
        e.goal_id = "goal-1";
        e.iteration_id = "goal-1/iter-2";
        e.revision = 2;
        e.payload["dedupe_key"] = "goal-1:r2:i2";
        const auto parsed = ParseGoalEvent(SerializeGoalEvent(e, "ts"));
        REQUIRE(parsed.has_value());
        CHECK(parsed->type == type);
        CHECK(parsed->iteration_id == "goal-1/iter-2");
    }
}

TEST_CASE("goal 事件行:payload 夹坏 UTF-8 不抛 316,清洗后落档行仍可解析") {
    // 0.26.41 真机崩的兜底面:工具输出被字节帽劈进字腰(0xE5 开头的半截
    // 汉字)流进 payload,裸 dump 当场 type_error.316 穿透顶层。落档行
    // 走 DumpJsonSanitized:不抛、出口合法、重新 parse 收得回。
    GoalSessionEvent e = MakeCreated();
    // "迁"(U+8FC1 = E8 BF 81)劈掉后两个续字节只留首字节 0xE8;note 里
    // 再单放一枚汉字首字节 0xE5(真机崩报的就是它)。std::string(...) 拼
    // 装避开字符串字面量里悬空续字节跟后文字节粘连的歧义。
    std::string objective = "迁移";
    objective.push_back('\xE8');
    objective += "认证层";
    e.payload["objective"] = objective;
    e.payload["note"] = std::string("半截字:") + std::string(1, '\xE5');

    std::string line;
    REQUIRE_NOTHROW(line = SerializeGoalEvent(e, "ts"));
    CHECK(lubancode::platform::IsValidUtf8(line));
    const auto parsed = ParseGoalEvent(line);
    REQUIRE(parsed.has_value());
    CHECK(parsed->goal_id == "goal-3");
    CHECK(lubancode::platform::IsValidUtf8(parsed->payload.at("objective").get<std::string>()));
    // 坏字节如实替换成 U+FFFD,好字节不动(合法片段原样保留)。
    CHECK(parsed->payload.at("objective").get<std::string>().find("认证层") != std::string::npos);
}

TEST_CASE("goal 证据行:facts 夹坏 UTF-8 同款不抛,清洗后仍可解析") {
    GoalEvidenceRecord e;
    e.id = "ev-bad";
    e.kind = "command_exit";
    e.goal_id = "goal-3";
    e.producer = "run_command";
    e.facts["head"] = std::string("输出前段:") + std::string(1, '\xE5');  // 汉字首字节悬空
    e.content_sha256 = "sha-x";

    std::string line;
    REQUIRE_NOTHROW(line = SerializeGoalEvidence(e, "ts"));
    CHECK(lubancode::platform::IsValidUtf8(line));
    const auto parsed = ParseGoalEvidence(line);
    REQUIRE(parsed.has_value());
    CHECK(parsed->id == "ev-bad");
    CHECK(lubancode::platform::IsValidUtf8(parsed->facts.at("head").get<std::string>()));
}

TEST_CASE("goal 事件行:坏行跳过,不废整场") {
    CHECK_FALSE(ParseGoalEvent("not json").has_value());
    CHECK_FALSE(ParseGoalEvent("{}").has_value());
    CHECK_FALSE(ParseGoalEvent(R"({"type":"compact"})").has_value());       // 非 goal 族
    CHECK_FALSE(ParseGoalEvent(R"({"type":"goal_v1"})").has_value());       // 缺 event/goal_id
    CHECK_FALSE(ParseGoalEvent(R"({"type":"goal_v1","event":"x"})").has_value());  // 缺 goal_id
    CHECK_FALSE(ParseGoalEvent(R"({"type":"goal_v9","event":"x","goal_id":"g"})").has_value());
}

TEST_CASE("goal 证据行:serialize → parse roundtrip") {
    GoalEvidenceRecord e;
    e.id = "ev-7";
    e.kind = "command_exit";
    e.goal_id = "goal-3";
    e.iteration_id = "goal-3/iter-1";
    e.tool_use_id = "toolu-9";
    e.producer = "run_command";
    e.facts["argv"] = {"ctest", "--test-dir", "build"};
    e.facts["exit_code"] = 0;
    e.content_sha256 = "sha-xyz";
    e.observed_at_ms = 1720000001000;
    e.fresh = true;
    e.truncated = false;

    const std::string line = SerializeGoalEvidence(e, "ts");
    const auto parsed = ParseGoalEvidence(line);
    REQUIRE(parsed.has_value());
    CHECK(parsed->id == "ev-7");
    CHECK(parsed->kind == "command_exit");
    CHECK(parsed->goal_id == "goal-3");
    CHECK(parsed->tool_use_id == "toolu-9");
    CHECK(parsed->facts.at("exit_code") == 0);
    CHECK(parsed->facts.at("argv").size() == 3);
    CHECK(parsed->content_sha256 == "sha-xyz");
    CHECK(parsed->fresh);
    CHECK_FALSE(ParseGoalEvidence(R"({"type":"goal_v1"})").has_value());
}

TEST_CASE("SessionStore:AppendGoalEvent/AppendGoalEvidence 落盘,回放整收") {
    TempDir tmp;
    lubancode::agent::SessionStore store(tmp.path.string());
    lubancode::agent::SessionMeta meta;
    meta.wire = "anthropic";
    meta.model = "m";
    meta.cwd = "/repo";
    REQUIRE(store.Begin(meta, "goal-test-session"));

    lubancode::agent::GoalSessionEvent created = MakeCreated();
    REQUIRE(store.AppendGoalEvent(created));
    GoalSessionEvent scheduled = MakeCreated();
    scheduled.type = "goal_iteration_v1";
    scheduled.event = "scheduled";
    scheduled.iteration_id = "goal-3/iter-1";
    scheduled.payload["dedupe_key"] = "goal-3:r1:i1";
    REQUIRE(store.AppendGoalEvent(scheduled));

    GoalEvidenceRecord evidence;
    evidence.id = "ev-1";
    evidence.kind = "tool_result";
    evidence.goal_id = "goal-3";
    evidence.iteration_id = "goal-3/iter-1";
    evidence.content_sha256 = "s1";
    REQUIRE(store.AppendGoalEvidence(evidence));

    // 读回:文件里 meta + 3 行事件(created/scheduled/observed)。
    const auto bytes = lubancode::agent::ReadSessionFileBytes(store.file_path());
    REQUIRE(bytes.has_value());
    const auto loaded = lubancode::agent::ParseSessionFile(*bytes);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->goal_events.size() == 3);
    CHECK(loaded->goal_events[0].event == "created");
    CHECK(loaded->goal_events[1].event == "scheduled");
    CHECK(loaded->goal_events[1].iteration_id == "goal-3/iter-1");
    // 证据行(goal_evidence_v1 顶层 type 也是 goal_ 族)同样整收。
    CHECK(loaded->goal_events[2].type == "goal_evidence_v1");
    CHECK(loaded->goal_events[2].payload.at("evidence_id") == "ev-1");
    CHECK(loaded->messages.empty());  // 事件行不算消息
    CHECK(loaded->skipped_lines == 0);
}

TEST_CASE("IsGoalEventLine 粗筛与尾行截断") {
    CHECK(lubancode::agent::IsGoalEventLine(R"({"type":"goal_v1","event":"created"})"));
    CHECK(lubancode::agent::IsGoalEventLine(R"({"type": "goal_iteration_v1"})"));
    CHECK_FALSE(lubancode::agent::IsGoalEventLine(R"({"type":"compact"})"));
    CHECK_FALSE(lubancode::agent::IsGoalEventLine(R"({"type":"title"})"));

    // 尾行截断:半截 JSON 的 goal 行跳过,前面的完整事件为准。
    const std::string full = SerializeGoalEvent(MakeCreated(), "ts");
    const std::string truncated = full.substr(0, full.size() - 10);
    CHECK_FALSE(ParseGoalEvent(truncated).has_value());
}
